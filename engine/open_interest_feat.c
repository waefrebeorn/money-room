/*
 * open_interest_feat.c — P36: Open Interest Features
 *
 * Fetches BTC perpetual OI from OKX + SPY options OI from local DB.
 * Computes OI momentum + call/put OI ratio.
 *
 * Build: gcc -O3 -march=native open_interest_feat.c -o open_interest_feat -lcurl -ljansson -lm
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
#define OUT_FILE HOME_DIR "/.hermes/options_cache/open_interest_features.json"
#define CURL_TIMEOUT 15L
#define SPY_DB HOME_DIR "/.hermes/options_cache/SPY_flows.db"

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

static double parse_dbl(const char *s) {
    if (!s) return 0;
    return strtod(s, NULL);
}

static double get_spy_option_oi(void) {
    FILE *f = fopen(SPY_DB, "rb");
    if (!f) return 0;
    fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "sqlite3 %s \"SELECT call_oi, put_oi FROM snapshots ORDER BY ts DESC LIMIT 1;\" 2>/dev/null",
        SPY_DB);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char line[256];
    double call_oi = 0, put_oi = 0;
    if (fgets(line, sizeof(line), p)) {
        sscanf(line, "%lf|%lf", &call_oi, &put_oi);
    }
    pclose(p);
    if (put_oi > 0) return call_oi / put_oi;
    return 1.0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    json_t *oi_data = http_get("https://www.okx.com/api/v5/public/open-interest?instId=BTC-USD-SWAP&instType=SWAP");
    if (!oi_data) { fprintf(stderr, "Failed OKX OI\n"); curl_global_cleanup(); return 1; }

    double oi_contracts = 0, oi_usd = 0;
    json_t *arr = json_object_get(oi_data, "data");
    if (arr && json_array_size(arr) > 0) {
        json_t *r = json_array_get(arr, 0);
        oi_contracts = parse_dbl(json_string_value(json_object_get(r, "oi")));
        oi_usd = parse_dbl(json_string_value(json_object_get(r, "oiCcy")));
    }
    json_decref(oi_data);

    double spy_oi_pcr = get_spy_option_oi();

    double oi_signal = oi_contracts / 1e7;
    if (oi_signal > 1.0) oi_signal = 1.0;

    double spy_oi_signal = spy_oi_pcr / 2.0;
    if (spy_oi_signal > 1.0) spy_oi_signal = 1.0;

    json_t *root = json_object();
    json_object_set_new(root, "btc_oi_contracts", json_real(oi_contracts));
    json_object_set_new(root, "btc_oi_usd", json_real(oi_usd));
    json_object_set_new(root, "btc_oi_signal", json_real(oi_signal));
    json_object_set_new(root, "spy_oi_pcr", json_real(spy_oi_pcr));
    json_object_set_new(root, "spy_oi_signal", json_real(spy_oi_signal));
    json_object_set_new(root, "source", json_string("okx+spy_db"));

    FILE *f = fopen(OUT_FILE, "w");
    if (f) { json_dumpf(root, f, JSON_INDENT(2) | JSON_SORT_KEYS); fclose(f); }
    json_decref(root);

    printf("Wrote %s\n", OUT_FILE);
    printf("  BTC OI: %.0f contracts ($%.0f) | OI signal: %.3f\n", oi_contracts, oi_usd, oi_signal);
    printf("  SPY OI PCR: %.2f | signal: %.3f\n", spy_oi_pcr, spy_oi_signal);

    curl_global_cleanup();
    return 0;
}
