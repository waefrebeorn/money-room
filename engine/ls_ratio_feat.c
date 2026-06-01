/*
 * ls_ratio_feat.c — P37: Long/Short Ratio Features
 *
 * Uses OKX taker buy/sell volume (CONTRACTS) as L/S proxy.
 * Computes current hour ratio, 24h avg ratio, and signal.
 *
 * Build: gcc -O3 -march=native ls_ratio_feat.c -o ls_ratio_feat -lcurl -ljansson -lm
 * Cron: every 15m
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>

#define HOME_DIR "/home/wubu2"
#define OUT_FILE HOME_DIR "/.hermes/options_cache/ls_ratio_features.json"
#define CURL_TIMEOUT 15L

#define PERIODS_24H 24

typedef struct { char *data; size_t len; } HttpBuf;
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    HttpBuf *b = (HttpBuf*)user; size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0; memcpy(p + b->len, ptr, n);
    b->data = p; b->len += n; b->data[b->len] = '\0'; return n;
}

static json_t *http_get(const char *url) {
    CURL *curl = curl_easy_init(); if (!curl) return NULL;
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
    json_error_t err; json_t *root = json_loads(buf.data, 0, &err);
    free(buf.data); return root;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    json_t *data = http_get(
        "https://www.okx.com/api/v5/rubik/stat/taker-volume?"
        "instId=BTC-USD-SWAP&instType=CONTRACTS&ccy=BTC&period=1H&limit=25");
    if (!data) { fprintf(stderr, "Failed OKX taker volume\n"); curl_global_cleanup(); return 1; }

    json_t *arr = json_object_get(data, "data");
    if (!arr || json_array_size(arr) == 0) {
        fprintf(stderr, "Empty OKX taker volume data\n");
        json_decref(data); curl_global_cleanup(); return 1;
    }

    /* Current hour (most recent) */
    json_t *latest = json_array_get(arr, 0);
    double buy_vol = strtod(json_string_value(json_array_get(latest, 1)), NULL);
    double sell_vol = strtod(json_string_value(json_array_get(latest, 2)), NULL);
    const char *ts_str = json_string_value(json_array_get(latest, 0));
    long long ts = ts_str ? atoll(ts_str) : 0;

    double ls_ratio = (sell_vol > 0) ? buy_vol / sell_vol : 1.0;
    double buy_pct = (buy_vol + sell_vol > 0) ? buy_vol / (buy_vol + sell_vol) : 0.5;

    /* 24h (or available) average ratio */
    int n_periods = json_array_size(arr);
    if (n_periods > PERIODS_24H) n_periods = PERIODS_24H;

    double sum_ratio = 0;
    double buy_total = 0, sell_total = 0;
    int valid = 0;
    for (int i = 0; i < n_periods; i++) {
        json_t *row = json_array_get(arr, i);
        double b = strtod(json_string_value(json_array_get(row, 1)), NULL);
        double s = strtod(json_string_value(json_array_get(row, 2)), NULL);
        if (b > 0 && s > 0) {
            sum_ratio += b / s;
            buy_total += b;
            sell_total += s;
            valid++;
        }
    }

    double avg_ratio = (valid > 0) ? sum_ratio / valid : 1.0;
    double aggregate_ls = (sell_total > 0) ? buy_total / sell_total : 1.0;
    double aggregate_buy_pct = (buy_total + sell_total > 0) ? buy_total / (buy_total + sell_total) : 0.5;

    /* Signal: deviation from 24h avg — positive = more longs */
    double ls_signal = (avg_ratio > 0) ? (ls_ratio - avg_ratio) / avg_ratio : 0;
    if (ls_signal < -1) ls_signal = -1;
    if (ls_signal > 1) ls_signal = 1;

    /* Normalized features (0-1 range for engine) */
    double ls_ratio_norm = ls_ratio / 2.0;  /* cap at 2.0 = all buy */
    if (ls_ratio_norm > 1.0) ls_ratio_norm = 1.0;
    double buy_pct_norm = buy_pct;           /* already 0-1 */
    double ls_signal_norm = (ls_signal + 1.0) / 2.0; /* -1..1 → 0..1 */

    json_t *root = json_object();
    json_object_set_new(root, "ls_ratio", json_real(ls_ratio));
    json_object_set_new(root, "buy_pct", json_real(buy_pct));
    json_object_set_new(root, "sell_pct", json_real(1.0 - buy_pct));
    json_object_set_new(root, "buy_vol_usd", json_real(buy_vol));
    json_object_set_new(root, "sell_vol_usd", json_real(sell_vol));
    json_object_set_new(root, "ls_ratio_24h_avg", json_real(avg_ratio));
    json_object_set_new(root, "aggregate_ls_24h", json_real(aggregate_ls));
    json_object_set_new(root, "aggregate_buy_pct", json_real(aggregate_buy_pct));
    json_object_set_new(root, "ls_signal", json_real(ls_signal));
    json_object_set_new(root, "ls_ratio_norm", json_real(ls_ratio_norm));
    json_object_set_new(root, "buy_pct_norm", json_real(buy_pct_norm));
    json_object_set_new(root, "ls_signal_norm", json_real(ls_signal_norm));
    json_object_set_new(root, "timestamp", json_integer(ts));
    json_object_set_new(root, "periods_in_24h", json_integer(valid));
    json_object_set_new(root, "source", json_string("okx_taker_volume"));

    FILE *f = fopen(OUT_FILE, "w");
    if (f) { json_dumpf(root, f, JSON_INDENT(2) | JSON_SORT_KEYS); fclose(f); }
    json_decref(root);

    printf("Wrote %s\n", OUT_FILE);
    printf("  L/S ratio: %.4f | Buy: %.1f%% Sell: %.1f%%\n",
           ls_ratio, buy_pct * 100, (1.0 - buy_pct) * 100);
    printf("  24h avg: %.4f | Signal: %.3f\n", avg_ratio, ls_signal);
    printf("  Buy vol: $%.0f | Sell vol: $%.0f\n", buy_vol, sell_vol);

    curl_global_cleanup();
    return 0;
}
