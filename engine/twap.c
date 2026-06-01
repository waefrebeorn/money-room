/*
 * twap.c — P52: Time-Weighted Average Price (TWAP)
 *
 * Computes TWAP from Kraken BTC/USD 1-min OHLCV data.
 * TWAP = Σ((high + low) / 2 * volume) / Σ(volume)
 * Above current price = downward pressure, below = upward pressure.
 *
 * Feature output:
 *   F83: twap_signal — current price vs TWAP (0-1, >0.5 = price above TWAP)
 *   F84: twap_trend — TWAP slope over last hour
 *
 * Build: gcc twap.c -o twap -lcurl -ljansson -lm -O2
 * Run: ./twap
 * Output: ~/.hermes/twap_cache/twap_features.json
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

#define CACHE_DIR   "~/.hermes/twap_cache"
#define OUTPUT_FILE "~/.hermes/twap_cache/twap_features.json"
#define N_CANDLES   60  /* 60 min of 1-min data for 1h TWAP */

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

/* ── Parse Kraken OHLC response ── */
/* Response: {"error":[],"result":{"last":1234567,"XXBTZUSD":[[ts,open,high,low,close,vwap,volume,count],...]}} */
static int parse_ohlc(const char *json, double *closes, double *highs,
                       double *lows, double *volumes, int *n_out) {
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;

    json_t *result = json_object_get(root, "result");
    if (!result) { json_decref(root); return -1; }

    /* Find OHLC array — key is something like "XXBTZUSD" or "XBTUSDT" */
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

    size_t n = json_array_size(ohlc_arr);
    if (n > N_CANDLES) n = N_CANDLES;
    *n_out = 0;

    /* Take last N_CANDLES candles (most recent), skip incomplete (vol=0) */
    size_t start = json_array_size(ohlc_arr) > N_CANDLES + 1
                   ? json_array_size(ohlc_arr) - N_CANDLES - 1 : 0;

    for (size_t i = start; i < json_array_size(ohlc_arr) && *n_out < N_CANDLES; i++) {
        json_t *candle = json_array_get(ohlc_arr, i);
        /* Kraken format: prices/volumes are STRINGS, not numbers */
        json_t *v_j = json_array_get(candle, 6);  /* volume */
        double vol = json_is_string(v_j) ? atof(json_string_value(v_j))
                   : json_number_value(v_j);
        if (vol <= 0) continue;  /* Skip incomplete candles */

        json_t *h_j = json_array_get(candle, 2);  /* high */
        json_t *l_j = json_array_get(candle, 3);  /* low */
        json_t *c_j = json_array_get(candle, 4);  /* close */

        closes[*n_out] = json_is_string(c_j) ? atof(json_string_value(c_j))
                        : json_number_value(c_j);
        highs[*n_out] = json_is_string(h_j) ? atof(json_string_value(h_j))
                       : json_number_value(h_j);
        lows[*n_out] = json_is_string(l_j) ? atof(json_string_value(l_j))
                      : json_number_value(l_j);
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
    printf("[TWAP] Time-Weighted Average Price\n");

    /* Fetch 60 1-min candles for BTC/USD from Kraken */
    char *json = fetch_url(
        "https://api.kraken.com/0/public/OHLC?pair=XBTUSD&interval=1");
    if (!json) { printf("  Kraken HTTP FAIL\n"); return 1; }

    double closes[128], highs[128], lows[128], volumes[128];
    int n = 0;
    if (parse_ohlc(json, closes, highs, lows, volumes, &n) != 0 || n < 5) {
        printf("  PARSE FAIL (%d candles)\n", n);
        free(json); return 1;
    }
    free(json);
    printf("  %d candles fetched\n", n);

    /* Compute TWAP = Σ(mid_price * volume) / Σ(volume) */
    double twap_num = 0, twap_vol = 0;
    for (int i = 0; i < n; i++) {
        double mid = (highs[i] + lows[i]) / 2.0;
        twap_num += mid * volumes[i];
        twap_vol += volumes[i];
    }
    double twap = twap_vol > 0 ? twap_num / twap_vol : 0;

    /* Current price = last close */
    double current_price = closes[n - 1];

    /* F83: Price vs TWAP signal */
    double deviation = (current_price - twap) / twap;  /* fraction above/below */
    double f83 = 0.5 + 0.5 * tanh(deviation * 100.0);  /* normalize to 0-1 */

    /* F84: TWAP trend (first 30 min avg vs last 30 min avg) */
    double f84 = 0.5;
    if (n >= 6) {
        int half = n / 2;
        double first_half = 0, last_half = 0;
        int first_n = 0, last_n = 0;

        for (int i = 0; i < half; i++) {
            double mid = (highs[i] + lows[i]) / 2.0;
            first_half += mid * volumes[i];
            first_n++;
        }
        for (int i = half; i < n; i++) {
            double mid = (highs[i] + lows[i]) / 2.0;
            last_half += mid * volumes[i];
            last_n++;
        }
        double first_avg = first_n > 0 ? first_half / first_n : 0;
        double last_avg = last_n > 0 ? last_half / last_n : 0;
        double trend = (last_avg - first_avg) / (first_avg > 0 ? first_avg : 1);
        f84 = 0.5 + 0.5 * tanh(trend * 50.0);
    }

    /* Write output */
    char out[512], dir[512];
    expand(OUTPUT_FILE, out, sizeof(out));
    expand(CACHE_DIR, dir, sizeof(dir));
    mkdir(dir, 0755);

    json_t *features = json_object();
    json_object_set_new(features, "twap_signal_norm", json_real(f83));
    json_object_set_new(features, "twap_trend_norm", json_real(f84));
    json_object_set_new(features, "twap_price", json_real(twap));
    json_object_set_new(features, "current_price", json_real(current_price));
    json_object_set_new(features, "deviation_pct", json_real(deviation * 100));
    json_object_set_new(features, "candles", json_integer(n));

    char tb[64]; time_t now = time(NULL);
    strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    json_object_set_new(features, "fetched_at", json_string(tb));

    json_dumpfd(features, open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644), JSON_INDENT(2));
    json_decref(features);

    printf("[TWAP] price=%.2f twap=%.2f dev=%.4f%% sig=%.4f trend=%.4f\n",
           current_price, twap, deviation * 100, f83, f84);
    return 0;
}
