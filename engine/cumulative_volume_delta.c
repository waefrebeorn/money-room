/*
 * cumulative_volume_delta.c — P51: Cumulative Volume Delta (CVD)
 *
 * Measures buying vs selling pressure from order book data.
 * CVD = cumulative signed volume (buy - sell) over time window.
 * Positive = net buying pressure, Negative = net selling pressure.
 *
 * Reads from order book archive SQLite DB (order_book_archive.c),
 * computes CVD from L2 snapshot deltas between consecutive reads.
 *
 * Feature output:
 *   F81: cvd_signal — normalized CVD (-1 to 1 mapped to 0-1)
 *   F82: cvd_momentum — rate of change of CVD over last N snapshots
 *
 * Build: gcc cumulative_volume_delta.c -o cumulative_volume_delta
 *        -lcurl -ljansson -lsqlite3 -lm -O2
 * Run: ./cumulative_volume_delta
 * Output: ~/.hermes/cvd_cache/cvd_features.json
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
#include <sqlite3.h>
#include <curl/curl.h>
#include <jansson.h>

/* ── Config ── */
#define CACHE_DIR     "~/.hermes/cvd_cache"
#define OUTPUT_FILE   "~/.hermes/cvd_cache/cvd_features.json"
#define ARCHIVE_DB    "~/.hermes/orderbook_cache/orderbook_archive.db"
#define COINBASE_API  "https://api.exchange.coinbase.com/products/BTC-USD/book?level=2"
#define N_PRICE_LEVELS 10  /* Top 10 bids and asks */

/* ── Fetch current L2 order book from Coinbase ── */
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

/* ── Parse Coinbase L2 order book JSON ── */
static int parse_orderbook(const char *json, double *bid_vol, double *ask_vol) {
    json_error_t err; json_t *root = json_loads(json, 0, &err);
    if (!root) return -1;

    *bid_vol = 0; *ask_vol = 0;

    /* Parse bids: [["price_str", "size_str", order_count], ...] */
    json_t *bids = json_object_get(root, "bids");
    if (bids && json_is_array(bids)) {
        size_t n = json_array_size(bids);
        if (n > N_PRICE_LEVELS) n = N_PRICE_LEVELS;
        for (size_t i = 0; i < n; i++) {
            json_t *entry = json_array_get(bids, i);
            json_t *size = json_array_get(entry, 1);
            if (size && json_is_string(size))
                *bid_vol += atof(json_string_value(size));
        }
    }

    /* Parse asks: [["price_str", "size_str", order_count], ...] */
    json_t *asks = json_object_get(root, "asks");
    if (asks && json_is_array(asks)) {
        size_t n = json_array_size(asks);
        if (n > N_PRICE_LEVELS) n = N_PRICE_LEVELS;
        for (size_t i = 0; i < n; i++) {
            json_t *entry = json_array_get(asks, i);
            json_t *size = json_array_get(entry, 1);
            if (size && json_is_string(size))
                *ask_vol += atof(json_string_value(size));
        }
    }

    json_decref(root);
    return 0;
}

/* ── Try to get historical CVD from SQLite archive ── */
static int read_archive_history(double *cvd_values, int max_n, int *n_out) {
    *n_out = 0;
    char db_path[512];
    const char *h = getenv("HOME"); if (!h) h = "/home/wubu2";
    if (ARCHIVE_DB[0] == '~') snprintf(db_path, sizeof(db_path), "%s%s", h, ARCHIVE_DB + 1);
    else snprintf(db_path, sizeof(db_path), "%s", ARCHIVE_DB);

    if (access(db_path, F_OK) != 0) return -1;  /* No archive */

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return -1;

    /* Check if snapshots table exists with bid_volume / ask_volume */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT bid_volume, ask_volume FROM snapshots "
        "WHERE bid_volume IS NOT NULL AND ask_volume IS NOT NULL "
        "ORDER BY fetched_at DESC LIMIT ?", -1, &stmt, 0);

    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }

    sqlite3_bind_int(stmt, 1, max_n + 1);  /* +1 for delta */

    int n = 0;
    double prev_bid = 0, prev_ask = 0;
    int has_prev = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && n < max_n) {
        double bid = sqlite3_column_double(stmt, 0);
        double ask = sqlite3_column_double(stmt, 1);

        if (has_prev) {
            double bid_delta = bid - prev_bid;
            double ask_delta = ask - prev_ask;
            /* CVD delta = bid volume increase - ask volume increase */
            /* Positive means more liquidity on bid side (buyers adding) */
            cvd_values[n] = bid_delta - ask_delta;
            n++;
        }

        prev_bid = bid;
        prev_ask = ask;
        has_prev = 1;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    *n_out = n;
    return 0;
}

/* ── Path expand ── */
static void expand(const char *p, char *out, size_t sz) {
    const char *h = getenv("HOME");
    if (!h) h = "/home/wubu2";
    if (p[0] == '~') snprintf(out, sz, "%s%s", h, p + 1);
    else snprintf(out, sz, "%s", p);
}

/* ── Main ── */
int main(void) {
    printf("[CVD] Cumulative Volume Delta\n");

    /* 1. Fetch current order book */
    printf("  Fetching Coinbase L2... "); fflush(stdout);
    char *json = fetch_url(COINBASE_API);
    if (!json) { printf("HTTP FAIL\n"); return 1; }

    double cur_bid = 0, cur_ask = 0;
    if (parse_orderbook(json, &cur_bid, &cur_ask) != 0) {
        printf("PARSE FAIL\n"); free(json); return 1;
    }
    free(json);
    printf("bid_vol=%.4f ask_vol=%.4f spread=%.4f\n", cur_bid, cur_ask, cur_bid - cur_ask);

    /* 2. Get historical CVD from archive */
    double cvd_hist[50];
    int n_hist = 0;
    read_archive_history(cvd_hist, 20, &n_hist);

    printf("  Archive: %d CVD snapshots\n", n_hist);

    /* 3. Compute CVD signal */
    /* Current CVD = bid_volume - ask_volume (positive = buying pressure) */
    double raw_cvd = cur_bid - cur_ask;
    double cvd_signal = 0.5;  /* Neutral default */

    /* Normalize: use sigmoid-like tanh scaling */
    /* cvd_signal = 0.5 + 0.5 * tanh(raw_cvd / 1000) */
    /* At raw_cvd=0 → 0.5 (neutral), raw_cvd=+5000 → ~0.76 (strong bid pressure) */
    cvd_signal = 0.5 + 0.5 * tanh(raw_cvd / 1000.0);

    /* 4. Compute CVD momentum (rate of change) */
    double cvd_momentum = 0.5;  /* Neutral default */
    if (n_hist >= 3) {
        double recent_avg = (cvd_hist[0] + cvd_hist[1] + cvd_hist[2]) / 3.0;
        double prev_avg = n_hist >= 6
            ? (cvd_hist[3] + cvd_hist[4] + cvd_hist[5]) / 3.0
            : cvd_hist[n_hist - 1];
        double delta = recent_avg - prev_avg;
        cvd_momentum = 0.5 + 0.5 * tanh(delta / 500.0);
    }

    /* 5. Write output JSON */
    char out[512], dir[512];
    expand(OUTPUT_FILE, out, sizeof(out));
    expand(CACHE_DIR, dir, sizeof(dir));
    mkdir(dir, 0755);

    json_t *features = json_object();
    json_object_set_new(features, "cvd_signal_norm", json_real(cvd_signal));
    json_object_set_new(features, "cvd_momentum_norm", json_real(cvd_momentum));
    json_object_set_new(features, "cvd_raw", json_real(raw_cvd));
    json_object_set_new(features, "bid_volume", json_real(cur_bid));
    json_object_set_new(features, "ask_volume", json_real(cur_ask));
    json_object_set_new(features, "archive_snapshots", json_integer(n_hist));

    char time_buf[64]; time_t now = time(NULL);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    json_object_set_new(features, "fetched_at", json_string(time_buf));

    json_dumpfd(features, open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644), JSON_INDENT(2));
    json_decref(features);

    printf("[CVD] signal=%.4f momentum=%.4f raw=%.2f\n", cvd_signal, cvd_momentum, raw_cvd);
    return 0;
}
