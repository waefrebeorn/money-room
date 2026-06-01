/*
 * liquidation_feat.c — P38: Liquidation Data Features
 *
 * Fetches BTC perpetual liquidation orders from OKX (free, no key).
 * Computes long/short liquidation volume, cascade detection, and
 * total liquidation intensity.
 *
 * Build: gcc -O3 -march=native liquidation_feat.c -o liquidation_feat -lcurl -ljansson -lm
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
#define OUT_FILE HOME_DIR "/.hermes/options_cache/liquidation_features.json"
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

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Fetch liquidation orders — 100 most recent filled */
    json_t *root = http_get(
        "https://www.okx.com/api/v5/public/liquidation-orders?"
        "instType=SWAP&uly=BTC-USD&state=filled&limit=100");
    if (!root) { fprintf(stderr, "Failed OKX liquidations\n"); curl_global_cleanup(); return 1; }

    json_t *groups = json_object_get(root, "data");
    if (!groups || json_array_size(groups) == 0) {
        fprintf(stderr, "No liquidation data\n");
        json_decref(root); curl_global_cleanup(); return 1;
    }

    /* Aggregate */
    double long_sz = 0, short_sz = 0;
    int long_count = 0, short_count = 0, total_events = 0;
    double long_usd = 0, short_usd = 0;
    double min_px = 1e9, max_px = 0;
    long long latest_ts = 0;

    size_t n_groups = json_array_size(groups);
    for (size_t g = 0; g < n_groups; g++) {
        json_t *group = json_array_get(groups, g);
        json_t *details = json_object_get(group, "details");
        if (!json_is_array(details)) continue;
        size_t n = json_array_size(details);
        for (size_t i = 0; i < n; i++) {
            json_t *liq = json_array_get(details, i);
            const char *side = json_string_value(json_object_get(liq, "posSide"));
            double sz = strtod(json_string_value(json_object_get(liq, "sz")), NULL);
            double px = strtod(json_string_value(json_object_get(liq, "bkPx")), NULL);
            long long ts = atoll(json_string_value(json_object_get(liq, "ts")));

            if (ts > latest_ts) latest_ts = ts;
            if (px > 0 && px < min_px) min_px = px;
            if (px > max_px) max_px = px;

            double usd_val = sz * px;
            if (side && strcmp(side, "long") == 0) {
                long_sz += sz;
                long_usd += usd_val;
                long_count++;
            } else if (side && strcmp(side, "short") == 0) {
                short_sz += sz;
                short_usd += usd_val;
                short_count++;
            }
            total_events++;
        }
    }

    double total_sz = long_sz + short_sz;
    double total_usd = long_usd + short_usd;

    /* F40: Long liq vs short liq ratio — high = longs being rekt (bearish) */
    double liq_ls_ratio = (short_sz > 0) ? long_sz / short_sz : 10.0;
    if (liq_ls_ratio > 10.0) liq_ls_ratio = 10.0;

    /* F41: Total liquidation intensity — recent 24h total (normalized) */
    double liq_intensity = total_usd / 1e9; /* normalize to $0-1B range */
    if (liq_intensity > 1.0) liq_intensity = 1.0;

    /* F42: Long liquidation dominance — what fraction is long (0-1) */
    double long_dominance = (total_sz > 0) ? long_sz / total_sz : 0.5;

    /* Cascade signal: are many positions being liquidated at similar prices?
     * High cascade = narrow price range with high volume */
    double price_span = (max_px - min_px) / ((min_px + max_px) / 2.0);
    double cascade_signal = (price_span > 0) ? (total_sz / price_span) / 1000.0 : 0;
    if (cascade_signal > 1.0) cascade_signal = 1.0;

    /* Normalized features for engine */
    double liq_ls_ratio_norm = liq_ls_ratio / 10.0;  /* 0-1 */

    json_t *out = json_object();
    json_object_set_new(out, "total_liq_btc", json_real(total_sz));
    json_object_set_new(out, "total_liq_usd", json_real(total_usd));
    json_object_set_new(out, "long_liq_btc", json_real(long_sz));
    json_object_set_new(out, "short_liq_btc", json_real(short_sz));
    json_object_set_new(out, "long_liq_count", json_integer(long_count));
    json_object_set_new(out, "short_liq_count", json_integer(short_count));
    json_object_set_new(out, "liq_ls_ratio", json_real(liq_ls_ratio));
    json_object_set_new(out, "liq_ls_ratio_norm", json_real(liq_ls_ratio_norm));
    json_object_set_new(out, "liq_intensity", json_real(liq_intensity));
    json_object_set_new(out, "long_dominance", json_real(long_dominance));
    json_object_set_new(out, "cascade_signal", json_real(cascade_signal));
    json_object_set_new(out, "min_bk_px", json_real(min_px));
    json_object_set_new(out, "max_bk_px", json_real(max_px));
    json_object_set_new(out, "total_events", json_integer(total_events));
    json_object_set_new(out, "source", json_string("okx_liquidations"));

    FILE *f = fopen(OUT_FILE, "w");
    if (f) { json_dumpf(out, f, JSON_INDENT(2) | JSON_SORT_KEYS); fclose(f); }
    json_decref(out);

    printf("Wrote %s\n", OUT_FILE);
    printf("  Liq: %.1f BTC ($%.0f) | Long: %.1f Short: %.1f\n", total_sz, total_usd, long_sz, short_sz);
    printf("  L/S ratio: %.2f | Intensity: %.3f | Cascade: %.3f\n", liq_ls_ratio, liq_intensity, cascade_signal);
    printf("  Events: %d long, %d short | Price range: $%.0f-$%.0f\n",
           long_count, short_count, min_px, max_px);

    json_decref(root);
    curl_global_cleanup();
    return 0;
}
