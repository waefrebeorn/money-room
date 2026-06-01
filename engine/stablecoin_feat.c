/*
 * stablecoin_feat.c — P34: Stablecoin Flow Features
 *
 * Fetches USDT/USDC/DAI from CoinGecko, computes risk appetite metrics.
 * Output: JSON -> feed_bridge -> engine features (F30-F32).
 *
 * Build: gcc -O3 -march=native stablecoin_feat.c -o stablecoin_feat -lcurl -ljansson -lm
 * Cron: every 15m (same as onchain_feat)
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
#define OUT_FILE HOME_DIR "/.hermes/options_cache/stablecoin_features.json"
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

static double get_mcap(json_t *coin) {
    if (!coin) return 0;
    json_t *md = json_object_get(coin, "market_data");
    if (!md) return 0;
    json_t *mc = json_object_get(md, "market_cap");
    return mc ? json_number_value(json_object_get(mc, "usd")) : 0;
}

static double get_vol(json_t *coin) {
    if (!coin) return 0;
    json_t *md = json_object_get(coin, "market_data");
    if (!md) return 0;
    json_t *tv = json_object_get(md, "total_volume");
    return tv ? json_number_value(json_object_get(tv, "usd")) : 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    json_t *usdt = http_get("https://api.coingecko.com/api/v3/coins/tether?localization=false&tickers=false&community_data=false&developer_data=false&sparkline=false");
    json_t *usdc = http_get("https://api.coingecko.com/api/v3/coins/usd-coin?localization=false&tickers=false&community_data=false&developer_data=false&sparkline=false");
    json_t *dai  = http_get("https://api.coingecko.com/api/v3/coins/dai?localization=false&tickers=false&community_data=false&developer_data=false&sparkline=false");

    if (!usdt) { fprintf(stderr, "USDT fetch failed\n"); goto cleanup; }

    double usdt_mcap = get_mcap(usdt);
    double usdt_vol  = get_vol(usdt);
    double usdc_mcap = get_mcap(usdc);
    double usdc_vol  = get_vol(usdc);
    double dai_mcap  = get_mcap(dai);
    double dai_vol   = get_vol(dai);

    double total_stable_mcap = usdt_mcap + usdc_mcap + dai_mcap;
    double total_stable_vol  = usdt_vol + usdc_vol + dai_vol;
    double usdt_dominance    = total_stable_mcap > 0 ? usdt_mcap / total_stable_mcap : 0.7;
    double stable_vol_ratio  = total_stable_mcap > 0 ? total_stable_vol / total_stable_mcap : 0;

    double stable_mcap_b = total_stable_mcap / 1e9;
    double usdt_dom_pct  = usdt_dominance * 100.0;

    json_t *root = json_object();
    json_object_set_new(root, "stable_total_mcap_usd", json_real(total_stable_mcap));
    json_object_set_new(root, "stable_total_mcap_b", json_real(stable_mcap_b));
    json_object_set_new(root, "stable_total_vol_24h", json_real(total_stable_vol));
    json_object_set_new(root, "usdt_mcap", json_real(usdt_mcap));
    json_object_set_new(root, "usdc_mcap", json_real(usdc_mcap));
    json_object_set_new(root, "dai_mcap", json_real(dai_mcap));
    json_object_set_new(root, "usdt_dominance_pct", json_real(usdt_dom_pct));
    json_object_set_new(root, "stable_vol_ratio", json_real(stable_vol_ratio));
    json_object_set_new(root, "source", json_string("coingecko"));

    FILE *f = fopen(OUT_FILE, "w");
    if (f) { json_dumpf(root, f, JSON_INDENT(2) | JSON_SORT_KEYS); fclose(f); }
    json_decref(root);

    printf("Wrote %s\n", OUT_FILE);
    printf("  Stable total: $%.0fB (USDT $%.0fB + USDC $%.0fB + DAI $%.0fB)\n",
           stable_mcap_b, usdt_mcap/1e9, usdc_mcap/1e9, dai_mcap/1e9);
    printf("  USDT dom: %.1f%% | Vol ratio: %.2f\n", usdt_dom_pct, stable_vol_ratio);

cleanup:
    if (usdt) json_decref(usdt);
    if (usdc) json_decref(usdc);
    if (dai) json_decref(dai);
    curl_global_cleanup();
    return 0;
}
