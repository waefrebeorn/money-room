/*
 * funding_feat.c — P35: Perpetual Funding Rate Features
 *
 * Fetches BTC perpetual funding rate from OKX public API.
 * Computes current rate + 7-day average for positioning signal.
 *
 * Build: gcc -O3 -march=native funding_feat.c -o funding_feat -lcurl -ljansson -lm
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
#define OUT_FILE HOME_DIR "/.hermes/options_cache/funding_features.json"
#define CURL_TIMEOUT 15L

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

static double parse_rate(const char *s) {
    if (!s) return 0;
    return strtod(s, NULL);
}

#define FUNDING_HISTORY_PERIODS 21

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    json_t *current = http_get("https://www.okx.com/api/v5/public/funding-rate?instId=BTC-USD-SWAP");
    if (!current) { fprintf(stderr, "Failed to fetch funding rate\n"); curl_global_cleanup(); return 1; }

    double funding_rate = 0;
    json_t *data_arr = json_object_get(current, "data");
    if (data_arr && json_array_size(data_arr) > 0) {
        const char *r = json_string_value(json_object_get(json_array_get(data_arr, 0), "fundingRate"));
        if (r) funding_rate = parse_rate(r);
    }
    json_decref(current);

    json_t *history = http_get("https://www.okx.com/api/v5/public/funding-rate-history?instId=BTC-USD-SWAP&limit=25");
    double rate_ma7 = 0;
    int n_rates = 0;
    if (history) {
        json_t *hist_arr = json_object_get(history, "data");
        if (hist_arr) {
            int n = json_array_size(hist_arr);
            double sum = 0;
            for (int i = 0; i < n && i < FUNDING_HISTORY_PERIODS; i++) {
                const char *r = json_string_value(json_object_get(json_array_get(hist_arr, i), "realizedRate"));
                if (r) sum += parse_rate(r);
                n_rates++;
            }
            if (n_rates > 0) rate_ma7 = sum / n_rates;
        }
        json_decref(history);
    }

    double funding_signal = rate_ma7 > 0 ? (funding_rate - rate_ma7) / fabs(rate_ma7) : 0;
    if (funding_signal < -1) funding_signal = -1;
    if (funding_signal > 1) funding_signal = 1;

    double rate_norm = (funding_rate * 10000.0) / 5.0 + 0.5;
    if (rate_norm < 0) rate_norm = 0;
    if (rate_norm > 1) rate_norm = 1;

    json_t *root = json_object();
    json_object_set_new(root, "funding_rate", json_real(funding_rate));
    json_object_set_new(root, "funding_rate_pct", json_real(funding_rate * 100.0));
    json_object_set_new(root, "funding_rate_ma7", json_real(rate_ma7));
    json_object_set_new(root, "funding_signal", json_real(funding_signal));
    json_object_set_new(root, "funding_rate_norm", json_real(rate_norm));
    json_object_set_new(root, "source", json_string("okx"));

    FILE *f = fopen(OUT_FILE, "w");
    if (f) { json_dumpf(root, f, JSON_INDENT(2) | JSON_SORT_KEYS); fclose(f); }
    json_decref(root);

    printf("Wrote %s\n", OUT_FILE);
    printf("  Funding rate: %.6f (%.4f%%) | 7d avg: %.6f | signal: %.3f\n",
           funding_rate, funding_rate * 100, rate_ma7, funding_signal);

    curl_global_cleanup();
    return 0;
}
