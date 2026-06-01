/*
 * seasonality.c — P73: Market Seasonality Engine (C port)
 *
 * Replaces seasonality.py. Pure computation from SPY daily history
 * via Yahoo Finance v8 chart API (5 years of data).
 *
 * Features:
 *   F71: Day-of-week seasonality (0-1, >0.5 = bullish DOW)
 *   F72: Month-of-year seasonality (0-1, >0.5 = bullish MOY)
 *
 * Build: gcc seasonality.c -o seasonality -lcurl -ljansson -lm -O2
 * Run: ./seasonality
 * Output: ~/.hermes/seasonality_cache/seasonality_features.json
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

#define CACHE_DIR "~/.hermes/seasonality_cache"
#define OUTPUT_FILE "~/.hermes/seasonality_cache/seasonality_features.json"

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
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c); curl_easy_cleanup(c);
    return r == CURLE_OK ? b.data : (free(b.data), NULL);
}

/* ── Fetch 5 years of SPY daily closes ── */
static int fetch_spy_history(float *closes, int *tstamps, int max_n) {
    time_t now = time(NULL);
    time_t five_years = now - 5 * 365 * 86400;

    char url[256];
    snprintf(url, sizeof(url),
        "https://query1.finance.yahoo.com/v8/finance/chart/SPY"
        "?period1=%ld&period2=%ld&interval=1d",
        (long)five_years, (long)now);

    char *json = fetch_url(url);
    if (!json) return 0;

    json_error_t err; json_t *root = json_loads(json, 0, &err);
    free(json); if (!root) return 0;

    int n = 0;
    json_t *chart = json_object_get(root, "chart");
    json_t *result = chart ? json_array_get(json_object_get(chart, "result"), 0) : NULL;
    if (!result) { json_decref(root); return 0; }

    json_t *tstamps_j = json_object_get(result, "timestamp");
    json_t *indicators = json_object_get(result, "indicators");
    json_t *quote = indicators ? json_array_get(json_object_get(indicators, "quote"), 0) : NULL;
    json_t *close_j = quote ? json_object_get(quote, "close") : NULL;

    if (!tstamps_j || !close_j) { json_decref(root); return 0; }

    size_t len = json_array_size(tstamps_j);
    for (size_t i = 0; i < len && n < max_n; i++) {
        json_t *c = json_array_get(close_j, i);
        json_t *t = json_array_get(tstamps_j, i);
        if (c && t && json_is_number(c) && json_is_number(t)) {
            double cv = json_number_value(c);
            if (cv > 0) {
                closes[n] = (float)cv;
                tstamps[n] = (int)json_number_value(t);
                n++;
            }
        }
    }

    json_decref(root);
    return n;
}

static void expand_path(const char *p, char *out, size_t sz) {
    const char *h = getenv("HOME");
    if (!h) h = "/home/wubu2";
    if (p[0] == '~') snprintf(out, sz, "%s%s", h, p + 1);
    else snprintf(out, sz, "%s", p);
}

int main(void) {
    printf("[SEASON] Fetching 5yr SPY history...\n");
    fflush(stdout);

    float closes[2000];
    int tstamps[2000];
    int n = fetch_spy_history(closes, tstamps, 2000);
    if (n < 20) {
        printf("[SEASON] ERROR: only %d data points\n", n);
        return 1;
    }
    printf("  %d daily bars fetched\n", n);

    /* ── Day-of-week seasonality ── */
    float dow_returns[5] = {0};
    int dow_counts[5] = {0};

    for (int i = 1; i < n; i++) {
        struct tm *tm = gmtime((time_t*)&tstamps[i]);
        int dow = tm->tm_wday;  /* 0=Sun, 1=Mon, ..., 6=Sat */
        if (dow < 1 || dow > 5) continue;  /* Skip weekends */
        int idx = dow - 1;  /* 0=Mon, 4=Fri */

        float ret = (closes[i] - closes[i-1]) / closes[i-1];
        dow_returns[idx] += ret;
        dow_counts[idx]++;
    }

    /* Find today's DOW */
    time_t now = time(NULL);
    struct tm *tm_now = gmtime(&now);
    int today_dow = tm_now->tm_wday - 1;  /* 0=Mon */
    if (today_dow < 0 || today_dow > 4) today_dow = 0;

    float avg_dow = 0;
    for (int i = 0; i < 5; i++) {
        if (dow_counts[i] > 0) {
            float avg = dow_returns[i] / dow_counts[i];
            if (i == today_dow) avg_dow = avg;
        }
    }

    /* Normalize to [0,1]: >0 = bullish */
    float f71 = 0.5f + avg_dow * 50.0f;
    if (f71 < 0) f71 = 0;
    if (f71 > 1) f71 = 1;

    /* ── Month-of-year seasonality ── */
    float moy_returns[12] = {0};
    int moy_counts[12] = {0};

    for (int i = 1; i < n; i++) {
        struct tm *tm = gmtime((time_t*)&tstamps[i]);
        int moy = tm->tm_mon;  /* 0=Jan */
        float ret = (closes[i] - closes[i-1]) / closes[i-1];
        moy_returns[moy] += ret;
        moy_counts[moy]++;
    }

    int this_month = tm_now->tm_mon;
    float avg_moy = 0;
    for (int i = 0; i < 12; i++) {
        if (moy_counts[i] > 0) {
            float avg = moy_returns[i] / moy_counts[i];
            if (i == this_month) avg_moy = avg;
        }
    }

    float f72 = 0.5f + avg_moy * 30.0f;
    if (f72 < 0) f72 = 0;
    if (f72 > 1) f72 = 1;

    /* ── Write output ── */
    json_t *features = json_object();
    json_object_set_new(features, "dow_seasonality_norm",
        json_real(roundf(f71 * 10000) / 10000));
    json_object_set_new(features, "moy_seasonality_norm",
        json_real(roundf(f72 * 10000) / 10000));

    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    json_object_set_new(features, "fetched_at", json_string(time_buf));
    json_object_set_new(features, "bars", json_integer(n));

    char out[512]; expand_path(OUTPUT_FILE, out, sizeof(out));
    char dir[512]; expand_path(CACHE_DIR, dir, sizeof(dir));
    mkdir(dir, 0755);
    json_dumpfd(features, open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644), JSON_INDENT(2));
    json_decref(features);

    printf("[SEASON] DOW=%.4f (today=%d) MOY=%.4f (month=%d) bars=%d\n",
           f71, today_dow, f72, this_month, n);
    return 0;
}
