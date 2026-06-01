/*
 * etf_flow_feat.c — P40: Bitcoin ETF Flow Data
 *
 * Fetches BTC ETF volume/price from Yahoo Finance (free, no key).
 * Computes ETF flow proxy signals for institutional demand.
 * Uses proper User-Agent to avoid rate limiting.
 *
 * Build: gcc -O3 -march=native etf_flow_feat.c -o etf_flow_feat -lcurl -ljansson -lm
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
#define OUT_FILE HOME_DIR "/.hermes/options_cache/etf_flow_features.json"
#define CURL_TIMEOUT 15L
#define MAX_ETFS 10

typedef struct { char *data; size_t len; } HttpBuf;
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    HttpBuf *b = (HttpBuf*)user; size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    memcpy(p + b->len, ptr, n);
    b->data = p; b->len += n; b->data[b->len] = '\0'; return n;
}

typedef struct {
    const char *ticker;
    double price, vol_total;
    int found;
} EtfData;

static EtfData fetch_etf(const char *ticker) {
    EtfData e = {ticker, 0, 0, 0};
    char url[256];
    snprintf(url, sizeof(url),
        "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=5d&interval=1d",
        ticker);

    CURL *curl = curl_easy_init();
    if (!curl) return e;

    HttpBuf buf = {NULL, 0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || !buf.data) { free(buf.data); return e; }

    json_error_t err;
    json_t *root = json_loads(buf.data, 0, &err);
    free(buf.data);
    if (!root) return e;

    json_t *result = json_object_get(json_object_get(root, "chart"), "result");
    if (json_is_array(result) && json_array_size(result) > 0) {
        json_t *first = json_array_get(result, 0);
        json_t *meta = json_object_get(first, "meta");
        json_t *mp = json_object_get(meta, "regularMarketPrice");
        if (json_is_number(mp)) e.price = json_number_value(mp);

        json_t *quotes = json_object_get(
            json_object_get(first, "indicators"), "quote");
        if (json_is_array(quotes) && json_array_size(quotes) > 0) {
            json_t *q = json_array_get(quotes, 0);
            json_t *vol_arr = json_object_get(q, "volume");
            if (json_is_array(vol_arr)) {
                size_t n = json_array_size(vol_arr);
                double sum_vol = 0;
                int valid = 0;
                for (size_t i = 0; i < n; i++) {
                    json_t *v = json_array_get(vol_arr, i);
                    if (json_is_number(v)) {
                        double val = json_number_value(v);
                        if (val > 0) { sum_vol += val; valid++; }
                    }
                }
                e.vol_total = valid > 0 ? sum_vol / valid : 0;
                e.found = (e.price > 0 && e.vol_total > 0);
            }
        }
    }
    json_decref(root);
    return e;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char *tickers[] = {"IBIT", "FBTC", "GBTC", "ARKB", "BITB", "HODL", "BTCO"};
    int n_etfs = 7;
    EtfData etfs[MAX_ETFS];

    double total_price = 0, total_flow = 0;
    int found_count = 0;
    double max_flow = 0;
    const char *max_ticker = "";

    printf("[ETF] Fetching %d BTC ETFs...\n", n_etfs);

    for (int i = 0; i < n_etfs; i++) {
        etfs[i] = fetch_etf(tickers[i]);
        if (etfs[i].found) {
            double flow = etfs[i].price * etfs[i].vol_total;
            total_price += etfs[i].price;
            total_flow += flow;
            found_count++;
            if (flow > max_flow) { max_flow = flow; max_ticker = etfs[i].ticker; }
            printf("  %s: $%.2f | avg vol: %.0f | flow: $%.0f\n",
                   etfs[i].ticker, etfs[i].price, etfs[i].vol_total, flow);
        } else {
            printf("  %s: not found\n", tickers[i]);
        }
    }

    /* Compute signals */
    /* F46: Total ETF flow proxy ($B) — institutional demand */
    double total_flow_b = total_flow / 1e9;
    double etf_flow_norm = total_flow_b / 5.0; /* normalize, cap at $5B */
    if (etf_flow_norm > 1.0) etf_flow_norm = 1.0;

    /* F47: ETF concentration ratio — how dominant is the largest ETF */
    /* High = concentrated in one provider (IBIT dominates), Low = diversified */
    double conc_ratio = total_flow > 0 ? max_flow / total_flow : 0.5;
    double conc_norm = conc_ratio; /* already 0-1 */

    /* F48: Average ETF flow per fund — institutional breadth */
    double avg_flow = found_count > 0 ? total_flow / found_count : 0;
    double avg_flow_norm = (avg_flow / 1e9) / 1.0; /* normalize, $0-1B */
    if (avg_flow_norm > 1.0) avg_flow_norm = 1.0;

    json_t *root = json_object();
    json_object_set_new(root, "total_flow_proxy_usd", json_real(total_flow));
    json_object_set_new(root, "total_flow_proxy_b", json_real(total_flow_b));
    json_object_set_new(root, "etf_count", json_integer(found_count));
    json_object_set_new(root, "dominant_etf", json_string(max_ticker));
    json_object_set_new(root, "dominant_flow_usd", json_real(max_flow));
    json_object_set_new(root, "concentration_ratio", json_real(conc_ratio));
    json_object_set_new(root, "etf_flow_norm", json_real(etf_flow_norm));
    json_object_set_new(root, "conc_norm", json_real(conc_norm));
    json_object_set_new(root, "avg_flow_norm", json_real(avg_flow_norm));
    json_object_set_new(root, "source", json_string("yahoo_finance"));

    FILE *f = fopen(OUT_FILE, "w");
    if (f) { json_dumpf(root, f, JSON_INDENT(2) | JSON_SORT_KEYS); fclose(f); }
    json_decref(root);

    printf("\nWrote %s\n", OUT_FILE);
    printf("  Total flow proxy: $%.2fB | ETFs found: %d\n", total_flow_b, found_count);
    printf("  Dominant: %s ($%.0f) | Concentration: %.2f\n", max_ticker, max_flow, conc_ratio);
    printf("  Flow norm: %.3f | Avg norm: %.3f\n", etf_flow_norm, avg_flow_norm);

    curl_global_cleanup();
    return 0;
}
