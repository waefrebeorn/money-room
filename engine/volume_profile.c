/*
 * volume_profile.c — P53: Volume Profile / Market Profile
 *
 * Shows volume distribution across price levels: which prices have
 * the most trading activity. Uses Kraken 1-min OHLC data.
 *
 * Features:
 *   F85: vp_high_volume_node — price level with highest volume
 *         (0-1, normalized distance from current price)
 *   F86: vp_value_area_ratio — fraction of volume in 30% price range
 *         (0-1, high = concentrated = support/resistance forming)
 *
 * Build: gcc volume_profile.c -o volume_profile -lcurl -ljansson -lm -O2
 * Run: ./volume_profile
 * Output: ~/.hermes/vp_cache/vp_features.json
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <jansson.h>

#define CACHE_DIR   "~/.hermes/vp_cache"
#define OUTPUT_FILE "~/.hermes/vp_cache/vp_features.json"
#define N_CANDLES   120  /* 2 hours of 1-min data */
#define N_PRICE_BINS 20

struct MemBuf { char *data; size_t len; };

static size_t write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t total = sz * nm; struct MemBuf *b = (struct MemBuf *)ud;
    char *nd = realloc(b->data, b->len + total + 1);
    if (!nd) return 0; b->data = nd;
    memcpy(b->data + b->len, ptr, total);
    b->len += total; b->data[b->len] = '\0';
    return total;
}

static char *fetch_url(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    struct MemBuf b = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "MoneyRoom/1.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    CURLcode r = curl_easy_perform(c); curl_easy_cleanup(c);
    return r == CURLE_OK ? b.data : (free(b.data), NULL);
}

static double get_val(json_t *j) {
    return json_is_string(j) ? atof(json_string_value(j)) : json_number_value(j);
}

/* Parse Kraken OHLC: find first non-"last" array */
static int parse_ohlc(const char *json, double *highs, double *lows,
                       double *volumes, int *n_out) {
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;
    json_t *result = json_object_get(root, "result");
    if (!result) { json_decref(root); return -1; }

    json_t *ohlc_arr = NULL;
    void *iter = json_object_iter(result);
    while (iter) {
        const char *k = json_object_iter_key(iter);
        if (strcmp(k, "last") != 0) {
            json_t *v = json_object_iter_value(iter);
            if (json_is_array(v)) { ohlc_arr = v; break; }
        }
        iter = json_object_iter_next(result, iter);
    }
    if (!ohlc_arr) { json_decref(root); return -1; }

    *n_out = 0;
    size_t start = json_array_size(ohlc_arr) > (size_t)(N_CANDLES + 1)
                   ? json_array_size(ohlc_arr) - N_CANDLES - 1 : 0;

    for (size_t i = start; i < json_array_size(ohlc_arr) && *n_out < N_CANDLES; i++) {
        json_t *c = json_array_get(ohlc_arr, i);
        double vol = get_val(json_array_get(c, 6));
        if (vol <= 0) continue;
        highs[*n_out] = get_val(json_array_get(c, 2));
        lows[*n_out] = get_val(json_array_get(c, 3));
        volumes[*n_out] = vol;
        (*n_out)++;
    }
    json_decref(root);
    return 0;
}

static void expand(const char *p, char *out, size_t sz) {
    const char *h = getenv("HOME"); if (!h) h = "/home/wubu2";
    if (p[0] == '~') snprintf(out, sz, "%s%s", h, p + 1);
    else snprintf(out, sz, "%s", p);
}

int main(void) {
    printf("[VP] Volume Profile\n");

    char *json = fetch_url(
        "https://api.kraken.com/0/public/OHLC?pair=XBTUSD&interval=1");
    if (!json) { printf("  HTTP FAIL\n"); return 1; }

    double highs[256], lows[256], volumes[256];
    int n = 0;
    if (parse_ohlc(json, highs, lows, volumes, &n) != 0 || n < 10) {
        printf("  PARSE FAIL (%d candles)\n", n);
        free(json); return 1;
    }
    free(json);
    printf("  %d candles\n", n);

    /* Find price range */
    double min_p = 1e9, max_p = 0;
    for (int i = 0; i < n; i++) {
        if (lows[i] < min_p) min_p = lows[i];
        if (highs[i] > max_p) max_p = highs[i];
    }
    double range = max_p - min_p;
    if (range <= 0) { printf("  Zero range\n"); return 1; }

    /* Create volume profile bins */
    double bin_vol[N_PRICE_BINS] = {0};
    double bin_size = range / N_PRICE_BINS;

    for (int i = 0; i < n; i++) {
        double mid = (highs[i] + lows[i]) / 2.0;
        int bin = (int)((mid - min_p) / bin_size);
        if (bin < 0) bin = 0;
        if (bin >= N_PRICE_BINS) bin = N_PRICE_BINS - 1;
        bin_vol[bin] += volumes[i];
    }

    /* F85: High volume node — which bin has most volume, normalized */
    double max_bin_vol = 0;
    int max_bin = 0;
    for (int i = 0; i < N_PRICE_BINS; i++) {
        if (bin_vol[i] > max_bin_vol) { max_bin_vol = bin_vol[i]; max_bin = i; }
    }

    double current_price = (highs[n-1] + lows[n-1]) / 2.0;
    double hvn_price = min_p + (max_bin + 0.5) * bin_size;
    double hvn_distance = (current_price - hvn_price) / range;

    /* F85: normalized to 0-1. 0.5 = HVN at current price */
    double f85 = 0.5 + 0.5 * tanh(hvn_distance * 3.0);
    if (f85 < 0) f85 = 0; if (f85 > 1) f85 = 1;

    /* F86: Value area ratio — volume concentration */
    double total_vol = 0;
    for (int i = 0; i < N_PRICE_BINS; i++) total_vol += bin_vol[i];

    /* Find bins in the 30% centered range */
    int center_bins = (int)(N_PRICE_BINS * 0.3);
    if (center_bins < 2) center_bins = 2;
    int mid_start = (N_PRICE_BINS - center_bins) / 2;
    double center_vol = 0;
    for (int i = mid_start; i < mid_start + center_bins; i++)
        center_vol += bin_vol[i];

    double f86 = total_vol > 0 ? center_vol / total_vol : 0.5;

    /* Write output */
    char out[512], dir[512];
    expand(OUTPUT_FILE, out, sizeof(out));
    expand(CACHE_DIR, dir, sizeof(dir));
    mkdir(dir, 0755);

    json_t *features = json_object();
    json_object_set_new(features, "vp_high_volume_node_norm", json_real(f85));
    json_object_set_new(features, "vp_value_area_ratio_norm", json_real(f86));
    json_object_set_new(features, "vp_hvn_price", json_real(hvn_price));
    json_object_set_new(features, "vp_current_price", json_real(current_price));
    json_object_set_new(features, "vp_candles", json_integer(n));

    char tb[64]; time_t now = time(NULL);
    strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    json_object_set_new(features, "fetched_at", json_string(tb));

    json_dumpfd(features, open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644), JSON_INDENT(2));
    json_decref(features);

    printf("[VP] HVN=$%.2f cur=$%.2f dist=%.4f sig=%.4f conc=%.4f\n",
           hvn_price, current_price, hvn_distance, f85, f86);
    return 0;
}
