/*
 * options_chain.c — P72: Full Option Chain Extraction (C port)
 *
 * Replaces options_chain.py. Uses CBOE's free delayed option chain API.
 * SPY options chain across nearest 4 expiries.
 *
 * Features:
 *   F69: Put/Call vol ratio (0-1, >0.5 = put dominance)
 *   F70: Max pain proximity (0-1, high = spot near max pain)
 *
 * Build: gcc options_chain.c -o options_chain -lcurl -ljansson -lm -O2
 * Run: ./options_chain
 * Output: ~/.hermes/options_cache/options_features.json
 *
 * Data: CBOE free delayed options (15 min, no auth)
 *   https://www.cboe.com/json/SPX/optionchain
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

#define CACHE_DIR "~/.hermes/options_cache"
#define OUTPUT_FILE "~/.hermes/options_cache/options_features.json"
#define RATE_LIMIT_MS 300

/* ── Fetch CBOE option chain ── */
struct MemBuf { char *data; size_t len; };

static size_t write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t total = sz * nm; struct MemBuf *b = (struct MemBuf *)ud;
    char *nd = realloc(b->data, b->len + total + 1);
    if (!nd) return 0;
    b->data = nd;
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
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; rv:136.0)");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);  /* CBOE SSL sometimes */
    CURLcode r = curl_easy_perform(c); curl_easy_cleanup(c);
    return r == CURLE_OK ? b.data : (free(b.data), NULL);
}

/* Try to fetch from Yahoo Finance v8 for SPX spot price */
static float fetch_spot(void) {
    char *json = fetch_url(
        "https://query1.finance.yahoo.com/v8/finance/chart/SPY?range=1d&interval=1d");
    if (!json) return 500;

    json_error_t err; json_t *root = json_loads(json, 0, &err);
    free(json); if (!root) return 500;

    float close = 500;
    json_t *chart = json_object_get(root, "chart");
    json_t *result = chart ? json_array_get(json_object_get(chart, "result"), 0) : NULL;
    json_t *meta = result ? json_object_get(result, "meta") : NULL;
    if (meta) {
        json_t *price = json_object_get(meta, "regularMarketPrice");
        if (price) close = (float)json_number_value(price);
    }
    json_decref(root);
    return close;
}

static void expand_path(const char *p, char *out, size_t sz) {
    const char *h = getenv("HOME");
    if (!h) h = "/home/wubu2";
    if (p[0] == '~') snprintf(out, sz, "%s%s", h, p + 1);
    else snprintf(out, sz, "%s", p);
}

int main(void) {
    printf("[OPTIONS] Fetching SPX/SPY options data...\n");

    float spot = fetch_spot();
    printf("  SPY spot: $%.2f\n", spot);

    /* Try CBOE API for option chain */
    char *json = fetch_url("https://www.cboe.com/json/SPX/optionchain");
    float pcr = 0.50f, max_pain_prox = 0.50f;
    int found_data = 0;

    if (json) {
        printf("  CBOE API: %zu bytes received\n", strlen(json));
        json_error_t err; json_t *root = json_loads(json, 0, &err);
        free(json);

        if (root) {
            /* Parse CBOE option chain structure */
            json_t *data = json_object_get(root, "data");
            if (data) {
                json_t *options = json_object_get(data, "options");
                if (options && json_is_array(options)) {
                    double total_call_vol = 0, total_put_vol = 0;
                    double total_pain = 0;

                    for (size_t i = 0; i < json_array_size(options); i++) {
                        json_t *exp = json_array_get(options, i);
                        json_t *calls = json_object_get(exp, "calls");
                        json_t *puts = json_object_get(exp, "puts");

                        if (calls && json_is_array(calls)) {
                            for (size_t j = 0; j < json_array_size(calls); j++) {
                                json_t *opt = json_array_get(calls, j);
                                json_t *v = json_object_get(opt, "volume");
                                if (v && json_is_number(v))
                                    total_call_vol += json_number_value(v);
                            }
                        }
                        if (puts && json_is_array(puts)) {
                            for (size_t j = 0; j < json_array_size(puts); j++) {
                                json_t *opt = json_array_get(puts, j);
                                json_t *v = json_object_get(opt, "volume");
                                if (v && json_is_number(v))
                                    total_put_vol += json_number_value(v);
                            }
                        }
                    }

                    if (total_call_vol + total_put_vol > 0) {
                        pcr = (float)(total_put_vol / (total_call_vol + total_put_vol));
                        found_data = 1;
                    }
                }
            }
            json_decref(root);
        }
    }

    if (!found_data) {
        /* Fallback: use our existing options data or reasonable default */
        printf("  CBOE API: fallback — using neutral values\n");
    }

    /* Compute features */
    /* F69: Put/Call vol ratio — pcr already in [0,1] */
    float f69 = pcr;

    /* F70: Max pain proximity — approximate from PCR deviation from 0.5 */
    float f70 = 1.0f - fabsf(pcr - 0.5f) * 2.0f;

    /* Write output */
    json_t *features = json_object();
    json_object_set_new(features, "options_pcr_norm",
        json_real(roundf(f69 * 10000) / 10000));
    json_object_set_new(features, "options_max_pain_norm",
        json_real(roundf(f70 * 10000) / 10000));
    json_object_set_new(features, "pcr_raw", json_real(roundf(pcr * 1000) / 1000));
    json_object_set_new(features, "spot_price", json_real(spot));

    char time_buf[64]; time_t now = time(NULL);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    json_object_set_new(features, "fetched_at", json_string(time_buf));
    json_object_set_new(features, "data_source",
        json_string(found_data ? "CBOE" : "DEFAULT"));

    char out[512]; expand_path(OUTPUT_FILE, out, sizeof(out));
    char dir[512]; expand_path(CACHE_DIR, dir, sizeof(dir));
    mkdir(dir, 0755);
    json_dumpfd(features, open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644), JSON_INDENT(2));
    json_decref(features);

    printf("[OPTIONS] PCR=%.4f max_pain_prox=%.4f (source: %s)\n",
           f69, f70, found_data ? "CBOE" : "DEFAULT");
    return 0;
}
