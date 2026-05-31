/**
 * exchange_api.c — Exchange REST API Client Module
 * 
 * Public ticker fetchers for Kraken, Binance, Coinbase.
 * No API keys needed for public endpoints.
 * 
 * Dependencies: libcurl, libjansson (same as room_feed_bridge)
 * 
 * Compile: gcc -O3 -c exchange_api.c -lcurl -ljansson
 * Link:   gcc -o exchange_api_test exchange_api_test.c exchange_api.o -lcurl -ljansson -lm
 *
 * Private endpoints (trades, balances) require API keys via ExchangeConfig 
 * — keys are empty by default, populated from file or env.
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>
#include "exchange_api.h"

// ── HTTP Response Buffer ──
typedef struct {
    char *data;
    size_t len;
} http_buf_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    http_buf_t *buf = (http_buf_t*)userdata;
    char *newp = realloc(buf->data, buf->len + total + 1);
    if (!newp) return 0;
    buf->data = newp;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *http_get(const char *url, long timeout_sec) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    http_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "exchange-api/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[exchange] HTTP error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

// ── ExchangeTicker zero-init ──
static ExchangeTicker empty_ticker(void) {
    ExchangeTicker t = {0, 0, 0, 0, 0, 0, 0, 0};
    return t;
}

// ═══════════════════════════════════════════════
// Kraken: https://docs.kraken.com/rest/#tag/Spot-Market-Data
// Public endpoint: GET /0/public/Ticker?pair=XBTUSD
// No API key required.
// ═══════════════════════════════════════════════
// ── Helper: parse a Kraken price string (Kraken uses string values in arrays) ──
static double kraken_val(json_t *arr, int idx) {
    if (!arr || json_array_size(arr) <= idx) return 0;
    json_t *j = json_array_get(arr, idx);
    if (!j) return 0;
    if (json_is_string(j)) return atof(json_string_value(j));
    return json_number_value(j);
}

ExchangeTicker fetch_kraken_ticker(const char *pair, int timeout_sec) {
    char url[512];
    snprintf(url, sizeof(url), "https://api.kraken.com/0/public/Ticker?pair=%s", pair);
    
    char *resp = http_get(url, timeout_sec);
    if (!resp) return empty_ticker();
    
    json_error_t err;
    json_t *root = json_loads(resp, 0, &err);
    free(resp);
    if (!root) {
        fprintf(stderr, "[exchange] Kraken JSON parse error: %s\n", err.text);
        return empty_ticker();
    }
    
    ExchangeTicker t = empty_ticker();
    json_t *jerror = json_object_get(root, "error");
    if (jerror && json_array_size(jerror) > 0) {
        const char *err_str = json_string_value(json_array_get(jerror, 0));
        fprintf(stderr, "[exchange] Kraken API error: %s\n", err_str ? err_str : "unknown");
        json_decref(root);
        return t;
    }
    
    json_t *jresult = json_object_get(root, "result");
    if (!jresult || !json_is_object(jresult)) {
        json_decref(root);
        return t;
    }
    
    // Kraken returns result as object with pair name as key — find first entry
    const char *key;
    json_t *val;
    json_object_foreach(jresult, key, val) {
        (void)key; // key is the pair name (e.g. "XXBTZUSD")
        
        // Kraken values are string-typed in arrays: e.g. "c": ["73332.40", "0.0055"]
        json_t *jc = json_object_get(val, "c");  // Close array [price, lot_volume]
        t.price = kraken_val(jc, 0);
        
        json_t *jv = json_object_get(val, "v");  // Volume array [today, last24h]
        t.volume_24h = kraken_val(jv, 1);        // last 24h volume
        
        json_t *jh = json_object_get(val, "h");  // High array [today, last24h]
        t.high_24h = kraken_val(jh, 1);
        
        json_t *jl = json_object_get(val, "l");  // Low array [today, last24h]
        t.low_24h = kraken_val(jl, 1);
        
        json_t *jb = json_object_get(val, "b");  // Bid array [price, lot_volume, timestamp]
        t.bid = kraken_val(jb, 0);
        
        json_t *ja = json_object_get(val, "a");  // Ask array [price, lot_volume, timestamp]
        t.ask = kraken_val(ja, 0);
        
        json_t *jo = json_object_get(val, "o");  // Open price (string)
        if (jo) {
            double open = kraken_val(json_array(), 0); // open is single string, not array
            // Kraken "o" is a single string price, not an array
            if (json_is_string(jo)) open = atof(json_string_value(jo));
            else open = json_number_value(jo);
            if (open > 0 && t.price > 0) {
                t.change_24h = (t.price - open) / open * 100.0;
            }
        }
        
        t.has_data = (t.price > 0) ? 1 : 0;
        break;  // First pair entry is ours
    }
    
    json_decref(root);
    
    if (t.has_data) {
        printf("[exchange] Kraken %s: price=%.2f bid=%.2f ask=%.2f vol=%.0f\n",
               pair, t.price, t.bid, t.ask, t.volume_24h);
    }
    return t;
}

// ═══════════════════════════════════════════════
// Binance: https://binance-docs.github.io/apidocs/spot/en/#24hr-ticker-price-change-statistics
// Public endpoint: GET /api/v3/ticker/24hr?symbol=BTCUSDT
// No API key required.
// ═══════════════════════════════════════════════
ExchangeTicker fetch_binance_ticker(const char *symbol, int timeout_sec) {
    char url[512];
    snprintf(url, sizeof(url), "https://api.binance.com/api/v3/ticker/24hr?symbol=%s", symbol);
    
    char *resp = http_get(url, timeout_sec);
    if (!resp) return empty_ticker();
    
    json_error_t err;
    json_t *root = json_loads(resp, 0, &err);
    free(resp);
    if (!root) {
        fprintf(stderr, "[exchange] Binance JSON parse error: %s\n", err.text);
        return empty_ticker();
    }
    
    ExchangeTicker t = empty_ticker();
    
    json_t *jprice = json_object_get(root, "lastPrice");
    if (jprice) t.price = atof(json_string_value(jprice));
    
    json_t *jvol = json_object_get(root, "volume");
    if (jvol) t.volume_24h = atof(json_string_value(jvol));
    
    json_t *jhigh = json_object_get(root, "highPrice");
    if (jhigh) t.high_24h = atof(json_string_value(jhigh));
    
    json_t *jlow = json_object_get(root, "lowPrice");
    if (jlow) t.low_24h = atof(json_string_value(jlow));
    
    json_t *jbid = json_object_get(root, "bidPrice");
    if (jbid) t.bid = atof(json_string_value(jbid));
    
    json_t *jask = json_object_get(root, "askPrice");
    if (jask) t.ask = atof(json_string_value(jask));
    
    json_t *jchange = json_object_get(root, "priceChangePercent");
    if (jchange) t.change_24h = atof(json_string_value(jchange));
    
    t.has_data = (t.price > 0) ? 1 : 0;
    
    json_decref(root);
    
    if (t.has_data) {
        printf("[exchange] Binance %s: price=%.2f bid=%.2f ask=%.2f vol=%.0f chg=%.2f%%\n",
               symbol, t.price, t.bid, t.ask, t.volume_24h, t.change_24h);
    }
    return t;
}

// ═══════════════════════════════════════════════
// Coinbase: https://docs.cloud.coinbase.com/exchange/reference/exchange-rates
// Public endpoint: GET /products/{product_id}/ticker
// No API key required.
// ═══════════════════════════════════════════════
ExchangeTicker fetch_coinbase_ticker(const char *product_id, int timeout_sec) {
    char url[512];
    snprintf(url, sizeof(url), "https://api.exchange.coinbase.com/products/%s/ticker", product_id);
    
    char *resp = http_get(url, timeout_sec);
    if (!resp) return empty_ticker();
    
    json_error_t err;
    json_t *root = json_loads(resp, 0, &err);
    free(resp);
    if (!root) {
        fprintf(stderr, "[exchange] Coinbase JSON parse error: %s\n", err.text);
        return empty_ticker();
    }
    
    ExchangeTicker t = empty_ticker();
    
    json_t *jprice = json_object_get(root, "price");
    if (jprice) t.price = atof(json_string_value(jprice));
    
    json_t *jvol = json_object_get(root, "volume");
    if (jvol) t.volume_24h = json_number_value(jvol);
    
    json_t *jbid = json_object_get(root, "bid");
    if (jbid) t.bid = atof(json_string_value(jbid));
    
    json_t *jask = json_object_get(root, "ask");
    if (jask) t.ask = atof(json_string_value(jask));
    
    json_t *jhigh = json_object_get(root, "high");
    if (jhigh) t.high_24h = atof(json_string_value(jhigh));
    
    json_t *jlow = json_object_get(root, "low");
    if (jlow) t.low_24h = atof(json_string_value(jlow));
    
    // Coinbase ticker doesn't have 24h change directly
    // Volume in 24h
    
    t.has_data = (t.price > 0) ? 1 : 0;
    json_decref(root);
    
    if (t.has_data) {
        printf("[exchange] Coinbase %s: price=%.2f bid=%.2f ask=%.2f\n",
               product_id, t.price, t.bid, t.ask);
    }
    return t;
}

// ── Config management ──
void exchange_config_init(ExchangeConfig *cfg) {
    memset(cfg, 0, sizeof(ExchangeConfig));
}

int exchange_config_load(ExchangeConfig *cfg, const char *path) {
    exchange_config_init(cfg);
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[exchange] No config at %s (using defaults)\n", path);
        return 0;
    }
    
    json_error_t err;
    json_t *root = json_loadf(fp, 0, &err);
    fclose(fp);
    if (!root) {
        fprintf(stderr, "[exchange] Config parse error: %s\n", err.text);
        return 0;
    }
    
    json_t *j;
    j = json_object_get(root, "kraken_key");
    if (j && json_is_string(j)) strncpy(cfg->kraken_key, json_string_value(j), sizeof(cfg->kraken_key)-1);
    j = json_object_get(root, "kraken_secret");
    if (j && json_is_string(j)) strncpy(cfg->kraken_secret, json_string_value(j), sizeof(cfg->kraken_secret)-1);
    j = json_object_get(root, "binance_key");
    if (j && json_is_string(j)) strncpy(cfg->binance_key, json_string_value(j), sizeof(cfg->binance_key)-1);
    j = json_object_get(root, "binance_secret");
    if (j && json_is_string(j)) strncpy(cfg->binance_secret, json_string_value(j), sizeof(cfg->binance_secret)-1);
    j = json_object_get(root, "coinbase_key");
    if (j && json_is_string(j)) strncpy(cfg->coinbase_key, json_string_value(j), sizeof(cfg->coinbase_key)-1);
    j = json_object_get(root, "coinbase_secret");
    if (j && json_is_string(j)) strncpy(cfg->coinbase_secret, json_string_value(j), sizeof(cfg->coinbase_secret)-1);
    j = json_object_get(root, "coinbase_passphrase");
    if (j && json_is_string(j)) strncpy(cfg->coinbase_passphrase, json_string_value(j), sizeof(cfg->coinbase_passphrase)-1);
    
    json_decref(root);
    return 1;
}

void exchange_config_print_status(const ExchangeConfig *cfg) {
    if (!cfg) return;
    printf("[exchange] Config status:\n");
    printf("  Kraken API key:    %s\n", cfg->kraken_key[0] ? "SET ✓" : "EMPTY (public only)");
    printf("  Binance API key:   %s\n", cfg->binance_key[0] ? "SET ✓" : "EMPTY (public only)");
    printf("  Coinbase API key:  %s\n", cfg->coinbase_key[0] ? "SET ✓" : "EMPTY (public only)");
}

// ── Utility ──
double exchange_spread_pct(double price_a, double price_b) {
    if (price_a <= 0 || price_b <= 0) return 0;
    return fabs(price_a - price_b) / ((price_a + price_b) / 2.0) * 100.0;
}
