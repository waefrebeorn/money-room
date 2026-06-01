/**
 * kraken_collector.c — Kraken OHLCV data collector (replaces 90-line Python)
 * Fetches 4 assets x 4 intervals, stores in historical.db.
 * Build: gcc -O2 kraken_collector.c -o kraken_collector -lcurl -ljansson -lsqlite3 -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <sys/stat.h>

#define DB_PATH "/home/wubu2/.hermes/pm_logs/historical/historical.db"
#define HB_PATH "/home/wubu2/.hermes/infra/heartbeats/kraken-collector.heartbeat"

typedef struct { const char *asset; const char *pair; } AssetPair;
static const AssetPair ASSETS[] = {
    {"BTC", "XXBTZUSD"}, {"ETH", "XETHZUSD"},
    {"SOL", "SOLUSD"},   {"XRP", "XXRPZUSD"}, {NULL, NULL}};
static const int INTERVALS[] = {1, 5, 60, 1440};

struct MemBuf { char *data; size_t size; };
static size_t write_cb(void *p, size_t s, size_t n, void *u) {
    size_t t = s * n; struct MemBuf *m = u;
    char *np = realloc(m->data, m->size + t + 1);
    if (!np) return 0; m->data = np;
    memcpy(m->data + m->size, p, t); m->size += t; m->data[m->size] = 0;
    return t;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    struct MemBuf mb = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "kraken-collector/1.0");
    CURLcode r = curl_easy_perform(c); curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(mb.data); return NULL; }
    return mb.data;
}

int main(void) {
    sqlite3 *db;
    sqlite3_open(DB_PATH, &db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS candles_multi ("
        "asset TEXT, interval_sec INTEGER, ts INTEGER,"
        "open REAL, high REAL, low REAL, close REAL, volume REAL, trades INTEGER,"
        "PRIMARY KEY (asset, interval_sec, ts))", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    int total = 0; (void)total;
    for (int a = 0; ASSETS[a].asset; a++) {
        for (int i = 0; i < 4; i++) {
            char url[256];
            snprintf(url, sizeof(url),
                "https://api.kraken.com/0/public/OHLC?pair=%s&interval=%d",
                ASSETS[a].pair, INTERVALS[i]);
            char *raw = http_get(url);
            if (!raw) { fprintf(stderr, "Fetch failed for %s@%d\n", ASSETS[a].asset, INTERVALS[i]); continue; }

            json_error_t err;
            json_t *root = json_loads(raw, 0, &err); free(raw);
            if (!root) { fprintf(stderr, "JSON err for %s\n", ASSETS[a].asset); continue; }

            json_t *err_arr = json_object_get(root, "error");
            if (err_arr && json_array_size(err_arr) > 0) { json_decref(root); continue; }

            json_t *result = json_object_get(root, "result");
            if (!json_is_object(result)) { json_decref(root); continue; }

            const char *k; json_t *v;
            json_object_foreach(result, k, v) {
                if (!json_is_array(v)) continue;
                sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
                sqlite3_stmt *stmt;
                sqlite3_prepare_v2(db,
                    "INSERT OR REPLACE INTO candles_multi VALUES (?,?,?,?,?,?,?,?,?)",
                    -1, &stmt, NULL);
                size_t idx; json_t *c;
                json_array_foreach(v, idx, c) {
                    if (!json_is_array(c) || json_array_size(c) < 8) continue;
                    json_t *ts_j = json_array_get(c, 0);
                    json_t *o_j = json_array_get(c, 1);
                    json_t *h_j = json_array_get(c, 2);
                    json_t *l_j = json_array_get(c, 3);
                    json_t *cl_j = json_array_get(c, 4);
                    json_t *vol_j = json_array_get(c, 6);
                    json_t *tr_j = json_array_get(c, 7);
                    if (!json_is_integer(ts_j) || !json_is_real(o_j)) continue;
                    double close_val = json_number_value(cl_j);
                    if (close_val <= 0.0) continue;  // Skip zero-close candles

                    sqlite3_bind_text(stmt, 1, ASSETS[a].asset, -1, SQLITE_STATIC);
                    sqlite3_bind_int(stmt, 2, INTERVALS[i]);
                    sqlite3_bind_int64(stmt, 3, json_integer_value(ts_j));
                    sqlite3_bind_double(stmt, 4, json_number_value(o_j));
                    sqlite3_bind_double(stmt, 5, json_number_value(h_j));
                    sqlite3_bind_double(stmt, 6, json_number_value(l_j));
                    sqlite3_bind_double(stmt, 7, json_number_value(cl_j));
                    sqlite3_bind_double(stmt, 8, json_number_value(vol_j));
                    sqlite3_bind_int(stmt, 9, tr_j ? (int)json_integer_value(tr_j) : 0);
                    sqlite3_step(stmt); sqlite3_reset(stmt);
                }
                sqlite3_finalize(stmt);
                sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
            }
            json_decref(root);
            struct timespec ts = {0, 300000000}; // 0.3s delay
            nanosleep(&ts, NULL);
        }
    }
    sqlite3_close(db);

    // Write heartbeat
    mkdir("/home/wubu2/.hermes/infra/heartbeats", 0755);
    FILE *hf = fopen(HB_PATH, "w");
    if (hf) { fprintf(hf, "%ld", time(NULL)); fclose(hf); }

    time_t now = time(NULL); struct tm *tm = gmtime(&now);
    char tb[32]; strftime(tb, sizeof(tb), "%H:%M UTC", tm);
    printf("Kraken data collected at %s\n", tb);
    return 0;
}
