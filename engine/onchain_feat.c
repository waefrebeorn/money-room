/*
 * onchain_feat.c — P33: On-Chain BTC Features
 *
 * Fetches CoinGecko BTC data + global metrics, computes derived on-chain
 * features beyond blockchain.info's basic stats.
 *
 * Output: JSON → feed_bridge → engine features (F27-F29).
 *
 * Dependencies: libcurl, libjansson, libm
 * Build: gcc -O3 -march=native onchain_feat.c -o onchain_feat -lcurl -ljansson -lm
 * Cron: hermes cron create --every 15m "./onchain_feat"
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>

#define HOME_DIR    "/home/wubu2"
#define OUT_FILE    HOME_DIR "/.hermes/options_cache/onchain_features.json"

#define CG_BTC_URL  "https://api.coingecko.com/api/v3/coins/bitcoin?localization=false&tickers=false&community_data=false&developer_data=false&sparkline=false"
#define CG_GLOBAL   "https://api.coingecko.com/api/v3/global"
#define CURL_TIMEOUT 15L

typedef struct { char *data; size_t len; } HttpBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    HttpBuf *b = (HttpBuf*)user;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    memcpy(p + b->len, ptr, n);
    b->data = p;
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static json_t *http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    HttpBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/8.0");
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) { free(buf.data); return NULL; }
    json_error_t err;
    json_t *root = json_loads(buf.data, 0, &err);
    free(buf.data);
    return root;
}

static double json_get_double(json_t *obj, const char *path) {
    // Path like "market_data.market_cap.usd"
    char copy[256];
    strncpy(copy, path, 255);
    copy[255] = '\0';
    json_t *cur = obj;
    char *tok = strtok(copy, ".");
    while (tok && cur) {
        cur = json_object_get(cur, tok);
        tok = strtok(NULL, ".");
    }
    return cur ? json_number_value(cur) : 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    json_t *btc = http_get(CG_BTC_URL);
    json_t *global = http_get(CG_GLOBAL);
    
    if (!btc && !global) {
        fprintf(stderr, "Both CoinGecko requests failed\n");
        curl_global_cleanup();
        return 1;
    }
    
    // Extract BTC market data
    double mcap = btc ? json_get_double(btc, "market_data.market_cap.usd") : 0;
    double vol_24h = btc ? json_get_double(btc, "market_data.total_volume.usd") : 0;
    double circ_supply = btc ? json_get_double(btc, "market_data.circulating_supply") : 0;
    double ath = btc ? json_get_double(btc, "market_data.ath.usd") : 0;
    double chg_24h = btc ? json_get_double(btc, "market_data.price_change_percentage_24h") : 0;
    double chg_7d = btc ? json_get_double(btc, "market_data.price_change_percentage_7d") : 0;
    double chg_30d = btc ? json_get_double(btc, "market_data.price_change_percentage_30d") : 0;
    double price = btc ? json_get_double(btc, "market_data.current_price.usd") : 0;
    
    // Extract global data
    double btc_dom = 0, total_mcap = 0, total_vol = 0;
    if (global) {
        json_t *data = json_object_get(global, "data");
        if (data) {
            json_t *mcap_pct = json_object_get(data, "market_cap_percentage");
            if (mcap_pct) btc_dom = json_number_value(json_object_get(mcap_pct, "btc"));
            total_mcap = json_number_value(json_object_get(json_object_get(data, "total_market_cap"), "usd"));
            total_vol = json_number_value(json_object_get(json_object_get(data, "total_volume"), "usd"));
        }
    }
    
    // ── Compute derived features ──
    
    // F27 proxy: BTC dominance change signal (0-1, high = BTC leading)
    // Normalize: typical 40-70% range → 0-1
    double dominance_signal = (btc_dom - 40.0) / 30.0;
    if (dominance_signal < 0) dominance_signal = 0;
    if (dominance_signal > 1) dominance_signal = 1;
    
    // Exchange volume ratio: BTC spot volume / total crypto volume
    // High = concentrated exchange activity, low = fragmented
    double exchange_vol_ratio = total_vol > 0 ? vol_24h / total_vol : 0;
    
    // MVRV proxy: current price / ATH ratio (0-1)
    // Low = undervalued (below ATH), high = near ATH
    double mcap_to_ath = ath > 0 ? price / ath : 0.5;
    
    // Volatility signal: 7d change magnitude normalized 0-1
    double vol_signal = fabs(chg_7d) / 30.0;
    if (vol_signal > 1) vol_signal = 1;
    
    // Build output JSON
    json_t *root = json_object();
    json_object_set_new(root, "btc_market_cap", json_real(mcap));
    json_object_set_new(root, "btc_dominance_pct", json_real(btc_dom));
    json_object_set_new(root, "btc_price_24h_chg", json_real(chg_24h));
    json_object_set_new(root, "btc_price_7d_chg", json_real(chg_7d));
    json_object_set_new(root, "btc_price_30d_chg", json_real(chg_30d));
    json_object_set_new(root, "btc_circulating_supply", json_real(circ_supply));
    json_object_set_new(root, "btc_ath_price", json_real(ath));
    json_object_set_new(root, "btc_mcap_to_ath", json_real(mcap_to_ath));
    json_object_set_new(root, "btc_dominance_signal", json_real(dominance_signal));
    json_object_set_new(root, "btc_exchange_vol_ratio", json_real(exchange_vol_ratio));
    json_object_set_new(root, "btc_vol_signal", json_real(vol_signal));
    json_object_set_new(root, "total_crypto_mcap", json_real(total_mcap));
    json_object_set_new(root, "total_crypto_vol_24h", json_real(total_vol));
    json_object_set_new(root, "source", json_string("coingecko"));
    
    FILE *f = fopen(OUT_FILE, "w");
    if (!f) { fprintf(stderr, "Can't write %s\n", OUT_FILE); json_decref(root); goto cleanup; }
    json_dumpf(root, f, JSON_INDENT(2) | JSON_SORT_KEYS);
    fclose(f);
    json_decref(root);
    
    printf("Wrote %s\n", OUT_FILE);
    printf("  BTC: $%.0f | dom: %.1f%% | mcap: $%.0fB\n", price, btc_dom, mcap / 1e9);
    printf("  changes: 24h=%.1f%% 7d=%.1f%% 30d=%.1f%%\n", chg_24h, chg_7d, chg_30d);
    printf("  mcap/ath=%.3f | dom_signal=%.2f | vol_ratio=%.2f\n",
           mcap_to_ath, dominance_signal, exchange_vol_ratio);
    
cleanup:
    if (btc) json_decref(btc);
    if (global) json_decref(global);
    curl_global_cleanup();
    return 0;
}
