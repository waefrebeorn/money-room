/**
 * btc_csv_refresher.c — Keep BTC 1-min CSV up to date
 * 
 * Fetches latest 1-min candles from Coinbase public API (no key needed),
 * reads the last timestamp from btc_1min_latest.csv, appends new candles.
 * 
 * Compile:
 *   gcc -O2 -o btc_csv_refresher btc_csv_refresher.c -lcurl -ljansson -lm
 * 
 * Usage:
 *   ./btc_csv_refresher              # Default: append new candles
 *   ./btc_csv_refresher --backfill   # Fetch full 300-candle history
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>

#define BTC_CSV          "/home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv"
#define COINBASE_API     "https://api.exchange.coinbase.com/products/BTC-USD/candles?granularity=60"
#define KRAKEN_API       "https://api.kraken.com/0/public/OHLC?pair=XXBTZUSD&interval=1"

// HTTP response buffer
typedef struct { char *data; size_t len; } http_buf_t;

static size_t write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t total = sz * nm;
    http_buf_t *b = (http_buf_t*)ud;
    char *np = realloc(b->data, b->len + total + 1);
    if (!np) return 0;
    b->data = np;
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

static char *http_get(const char *url, int timeout) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    http_buf_t b = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "btc-refresher/1.0");
    // Coinbase requires a User-Agent header
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    CURLcode r = curl_easy_perform(c);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(b.data); return NULL; }
    return b.data;
}

// Read last timestamp from existing CSV
static int64_t get_last_ts(void) {
    FILE *f = fopen(BTC_CSV, "r");
    if (!f) return 0;
    char buf[256];
    int64_t last_ts = 0;
    // Skip header
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    while (fgets(buf, sizeof(buf), f)) {
        int64_t ts;
        if (sscanf(buf, "%ld,", &ts) >= 1 && ts > last_ts) last_ts = ts;
    }
    fclose(f);
    return last_ts;
}

// Append a single candle row to CSV
static void append_candle(int64_t ts, double o, double h, double l, double c, double v) {
    FILE *f = fopen(BTC_CSV, "a");
    if (!f) { fprintf(stderr, "[REFRESH] Cannot open %s\n", BTC_CSV); return; }
    fprintf(f, "%ld,%.2f,%.2f,%.2f,%.2f,%.6f\n", (long)ts, o, h, l, c, v);
    fclose(f);
}

// Fetch from Coinbase (primary)
static int fetch_coinbase(int64_t last_ts) {
    char *resp = http_get(COINBASE_API, 15);
    if (!resp) { fprintf(stderr, "[REFRESH] Coinbase API failed\n"); return -1; }
    
    json_error_t err;
    json_t *root = json_loads(resp, 0, &err);
    free(resp);
    if (!root || !json_is_array(root)) {
        fprintf(stderr, "[REFRESH] Coinbase: invalid JSON (%s)\n", err.text);
        if (root) json_decref(root);
        return -1;
    }
    
    int n = (int)json_array_size(root);
    int appended = 0, skipped = 0;
    
    // Coinbase returns [timestamp, low, high, open, close, volume]
    // Process in reverse (oldest first) to maintain chronological order
    for (int i = n - 1; i >= 0; i--) {
        json_t *c = json_array_get(root, i);
        if (!c || !json_is_array(c) || json_array_size(c) < 6) continue;
        
        int64_t ts = (int64_t)json_number_value(json_array_get(c, 0));
        double low = json_number_value(json_array_get(c, 1));
        double high = json_number_value(json_array_get(c, 2));
        double open = json_number_value(json_array_get(c, 3));
        double close = json_number_value(json_array_get(c, 4));
        double volume = json_number_value(json_array_get(c, 5));
        
        if (ts <= last_ts) { skipped++; continue; }
        if (close <= 0 || volume <= 0) continue;
        
        append_candle(ts, open, high, low, close, volume);
        appended++;
    }
    
    json_decref(root);
    printf("[REFRESH] Coinbase: %d new candles (skipped %d existing)\n", appended, skipped);
    return appended;
}

// Fallback: fetch from Kraken
static int fetch_kraken(int64_t last_ts) {
    char *resp = http_get(KRAKEN_API, 15);
    if (!resp) { fprintf(stderr, "[REFRESH] Kraken API failed\n"); return -1; }
    
    json_error_t err;
    json_t *root = json_loads(resp, 0, &err);
    free(resp);
    if (!root) {
        fprintf(stderr, "[REFRESH] Kraken: invalid JSON (%s)\n", err.text);
        return -1;
    }
    
    json_t *jresult = json_object_get(root, "result");
    if (!jresult) { json_decref(root); return -1; }
    
    // Find the XXBTZUSD key
    json_t *jcandles = NULL;
    const char *key; json_t *val;
    json_object_foreach(jresult, key, val) {
        if (strstr(key, "XBT") || strstr(key, "BTC")) {
            jcandles = val;
            break;
        }
    }
    
    if (!jcandles || !json_is_array(jcandles)) {
        json_decref(root);
        return -1;
    }
    
    int n = (int)json_array_size(jcandles);
    int appended = 0;
    
    // Kraken: [ts, open, high, low, close, vwap, volume, count]
    for (int i = 0; i < n; i++) {
        json_t *c = json_array_get(jcandles, i);
        if (!c || !json_is_array(c) || json_array_size(c) < 8) continue;
        
        int64_t ts = (int64_t)json_integer_value(json_array_get(c, 0));
        double open = atof(json_string_value(json_array_get(c, 1)));
        double high = atof(json_string_value(json_array_get(c, 2)));
        double low = atof(json_string_value(json_array_get(c, 3)));
        double close = atof(json_string_value(json_array_get(c, 4)));
        double volume = atof(json_string_value(json_array_get(c, 6)));
        
        if (ts <= last_ts) continue;
        if (close <= 0) continue;
        
        append_candle(ts, open, high, low, close, volume);
        appended++;
    }
    
    json_decref(root);
    printf("[REFRESH] Kraken: %d new candles\n", appended);
    return appended;
}

int main(int argc, char **argv) {
    int backfill = (argc > 1 && strcmp(argv[1], "--backfill") == 0);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    int64_t last_ts = get_last_ts();
    if (last_ts == 0) {
        printf("[REFRESH] Creating new BTC CSV\n");
        FILE *f = fopen(BTC_CSV, "w");
        if (f) { fprintf(f, "ts,open,high,low,close,volume\n"); fclose(f); }
    } else {
        char tb[32]; time_t t = last_ts; 
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", gmtime(&t));
        printf("[REFRESH] Last candle: %s (ts=%ld)\n", tb, (long)last_ts);
    }
    
    // Try Coinbase first (free, no key)
    int r = fetch_coinbase(last_ts);
    if (r <= 0) {
        printf("[REFRESH] Coinbase failed, trying Kraken...\n");
        r = fetch_kraken(last_ts);
    }
    
    curl_global_cleanup();
    
    // Re-read and show new last timestamp
    int64_t new_ts = get_last_ts();
    if (new_ts > last_ts) {
        char tb[32]; time_t t = new_ts;
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", gmtime(&t));
        printf("[REFRESH] Now ends at: %s\n", tb);
    } else {
        printf("[REFRESH] No new data (already up to date)\n");
    }
    
    return r > 0 ? 0 : 1;
}
