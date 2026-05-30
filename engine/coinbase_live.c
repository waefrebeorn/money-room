/**
 * coinbase_live.c — Coinbase live BTC 1-min collector (T1188)
 * Fetches latest BTC/USD candles from Coinbase Exchange public REST API.
 * No auth key needed for market data.
 *
 * Build: gcc -O3 -march=native coinbase_live.c -o coinbase_live -lcurl -ljansson -lsqlite3 -lm
 * Usage: ./coinbase_live          # fetch last 300 1-min candles
 * Output: writes to timeline.db (coinbase_1min source)
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <sys/stat.h>

#define DB_PATH     "/home/wubu2/.hermes/pm_logs/historical/historical.db"
#define TIMELINE_DB "/home/wubu2/.hermes/pm_logs/timeline.db"
#define CACHE_DIR   "/home/wubu2/.hermes/pm_logs/historical"
#define HB_DIR      "/home/wubu2/.hermes/infra/heartbeats"
#define HB_PATH     HB_DIR "/coinbase-live.heartbeat"
#define PAIR_NAME   "Coinbase_BTC"
#define API_URL     "https://api.exchange.coinbase.com/products/BTC-USD/candles?granularity=60"

typedef struct { char *data; size_t len; } HttpBuf;

static size_t write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t total = sz * nm; HttpBuf *b = (HttpBuf *)ud;
    char *np = realloc(b->data, b->len + total + 1);
    if (!np) return 0; b->data = np;
    memcpy(b->data + b->len, ptr, total);
    b->len += total; b->data[b->len] = '\0';
    return total;
}

static char *fetch(const char *url) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    HttpBuf b = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "MoneyRoom/1.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return r == CURLE_OK ? b.data : (free(b.data), NULL);
}

static int heartbeat(void) {
    struct stat st = {0};
    if (stat(HB_DIR, &st) == -1) mkdir(HB_DIR, 0755);
    FILE *f = fopen(HB_PATH, "w");
    if (!f) return -1;
    fprintf(f, "%ld\n", (long)time(NULL));
    fclose(f);
    return 0;
}

static void write_timeline(sqlite3 *db, long ts, double open, double high,
                            double low, double close, double vol) {
    /* Write to historical.db candles table */
    const char *sql_c = "INSERT OR IGNORE INTO candles "
        "(pair, ts, open, high, low, close, volume) "
        "VALUES ('Coinbase_BTC', ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql_c, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, ts);
        sqlite3_bind_double(s, 2, open);
        sqlite3_bind_double(s, 3, high);
        sqlite3_bind_double(s, 4, low);
        sqlite3_bind_double(s, 5, close);
        sqlite3_bind_double(s, 6, vol);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    /* Write to timeline.db timeline table */
    static sqlite3 *tl_db = NULL;
    if (!tl_db) {
        struct stat st = {0};
        if (stat(CACHE_DIR, &st) == -1) mkdir(CACHE_DIR, 0755);
        sqlite3_open(TIMELINE_DB, &tl_db);
    }
    if (tl_db) {
        const char *sql_t = "INSERT OR IGNORE INTO timeline "
            "(ts, source, category, data, collected_at) "
            "VALUES (?, 'coinbase', 'crypto', ?, ?)";
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(tl_db, sql_t, -1, &st, NULL) == SQLITE_OK) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "{\"pair\":\"BTC-USD\",\"open\":%.2f,\"high\":%.2f,\"low\":%.2f,"
                "\"close\":%.2f,\"volume\":%.2f}",
                open, high, low, close, vol);
            sqlite3_bind_int64(st, 1, ts);
            sqlite3_bind_text(st, 2, buf, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 3, (long)time(NULL));
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
}

static sqlite3 *open_hist_db(void) {
    struct stat st = {0};
    if (stat(CACHE_DIR, &st) == -1) mkdir(CACHE_DIR, 0755);
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return NULL;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS candles ("
        "  pair TEXT NOT NULL, ts INTEGER NOT NULL,"
        "  open REAL, high REAL, low REAL, close REAL, volume REAL, trades INTEGER,"
        "  PRIMARY KEY (pair, ts)"
        ");";
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    return db;
}

int main(void) {
    printf("[COINBASE_LIVE] Fetching BTC-USD 1-min candles...\n");
    
    char *json = fetch(API_URL);
    if (!json) {
        fprintf(stderr, "[COINBASE_LIVE] ERROR: Failed to fetch from Coinbase\n");
        return 1;
    }

    json_error_t err;
    json_t *root = json_loads(json, 0, &err);
    free(json);
    if (!root) {
        fprintf(stderr, "[COINBASE_LIVE] ERROR: JSON parse: %s\n", err.text);
        return 1;
    }
    if (!json_is_array(root)) {
        fprintf(stderr, "[COINBASE_LIVE] ERROR: Expected array\n");
        json_decref(root);
        return 1;
    }

    size_t n = json_array_size(root);
    printf("[COINBASE_LIVE] Got %zu candles\n", n);

    sqlite3 *db = open_hist_db();
    if (!db) {
        fprintf(stderr, "[COINBASE_LIVE] ERROR: Can't open historical.db\n");
        json_decref(root);
        return 1;
    }

    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    int inserted = 0;
    double last_close = 0;
    long last_ts = 0;
    double sum_open = 0, sum_high = 0, sum_low = 0, sum_close = 0, sum_vol = 0;
    int count = 0;

    for (size_t i = 0; i < n; i++) {
        json_t *c = json_array_get(root, i);
        if (!json_is_array(c) || json_array_size(c) < 6) continue;

        /* Coinbase: [ts, low, high, open, close, volume] */
        long ts = (long)json_number_value(json_array_get(c, 0));
        double low = json_number_value(json_array_get(c, 1));
        double high = json_number_value(json_array_get(c, 2));
        double open = json_number_value(json_array_get(c, 3));
        double close = json_number_value(json_array_get(c, 4));
        double vol = json_number_value(json_array_get(c, 5));

        write_timeline(db, ts, open, high, low, close, vol);
        inserted++;
        
        if (ts > last_ts) {
            last_close = close;
            last_ts = ts;
        }
        
        /* Running stats for summary */
        if (count == 0) { sum_open = open; }
        sum_high = fmax(sum_high, high);
        sum_low = (count == 0) ? low : fmin(sum_low, low);
        sum_close = close;
        sum_vol += vol;
        count++;
    }

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    sqlite3_close(db);
    json_decref(root);
    heartbeat();

    printf("[COINBASE_LIVE] DONE: %d candles inserted\n", inserted);
    if (count > 0) {
        printf("[COINBASE_LIVE] Latest: O=%.2f H=%.2f L=%.2f C=%.2f V=%.2f\n",
               sum_open, sum_high, sum_low, sum_close, sum_vol);
        printf("[COINBASE_LIVE] Current BTC: $%.2f\n", last_close);
    }
    printf("[COINBASE_LIVE] Heartbeat: %s\n", HB_PATH);
    return 0;
}
