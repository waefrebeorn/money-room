/**
 * forex_collector.c — C103: Forex Rate Collector
 *
 * Fetches forex rates from frankfurter.app free API.
 * Writes to timeline.db with source='forex_<pair>'.
 *
 * Build: gcc -O2 -o forex_collector forex_collector.c -lcurl -ljansson -lsqlite3
 * Usage: ./forex_collector
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <jansson.h>

#define FX_API "https://api.frankfurter.app"
#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"

typedef struct { char *data; size_t len; size_t cap; } RespBuf;

static size_t write_cb(void *p, size_t s, size_t n, void *u) {
    size_t t = s*n; RespBuf *r = u;
    if (r->len + t >= r->cap) {
        r->cap = r->len + t + 65536;
        r->data = realloc(r->data, r->cap);
    }
    memcpy(r->data + r->len, p, t);
    r->len += t; r->data[r->len] = 0;
    return t;
}

static RespBuf http_get(const char *url) {
    RespBuf r = {calloc(1, 65536), 0, 65536};
    CURL *c = curl_easy_init();
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "forex-collector/1.0");
        curl_easy_perform(c);
        curl_easy_cleanup(c);
    }
    return r;
}

static sqlite3 *g_db = NULL;
static void db_init(void) {
    sqlite3_open(DB_PATH, &g_db);
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
}

static void db_insert(const char *source, long long ts, const char *cat, const char *json_data) {
    if (!g_db) return;
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO timeline (ts, source, category, data, collected_at) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, cat, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, json_data, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (long long)time(NULL));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void db_close(void) { if (g_db) sqlite3_close(g_db); }

int main(void) {
    printf("[FX] Forex Rate Collector\n");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    db_init();
    long long now = (long long)time(NULL);
    int total = 0;

    /* Fetch latest rates from EUR base */
    RespBuf resp = http_get(FX_API "/latest?from=USD");
    json_error_t err;
    json_t *root = json_loads(resp.data, 0, &err);
    free(resp.data);

    if (!root) {
        printf("  API error\n");
        db_close();
        curl_global_cleanup();
        return 1;
    }

    /* Extract date */
    const char *date = json_string_value(json_object_get(root, "date"));

    /* Extract rates */
    json_t *rates = json_object_get(root, "rates");
    if (rates) {
        /* EUR/USD */
        double eurusd = json_number_value(json_object_get(rates, "EUR"));
        if (eurusd > 0) {
            char json_data[256];
            snprintf(json_data, sizeof(json_data),
                "{\"pair\":\"EUR/USD\",\"rate\":%.6f,\"date\":\"%s\",\"inverse\":%.6f}",
                eurusd, date ? date : "", 1.0/eurusd);
            db_insert("forex_eurusd", now, "forex", json_data);
            total++;
        }

        /* GBP/USD */
        double gbpusd = json_number_value(json_object_get(rates, "GBP"));
        if (gbpusd > 0) {
            char json_data[256];
            snprintf(json_data, sizeof(json_data),
                "{\"pair\":\"GBP/USD\",\"rate\":%.6f,\"date\":\"%s\",\"inverse\":%.6f}",
                gbpusd, date ? date : "", 1.0/gbpusd);
            db_insert("forex_gbpusd", now, "forex", json_data);
            total++;
        }

        /* USD/JPY = inverse of JPY/USD */
        double jpyusd = json_number_value(json_object_get(rates, "JPY"));
        if (jpyusd > 0) {
            double usdjpy = 1.0 / jpyusd;
            char json_data[256];
            snprintf(json_data, sizeof(json_data),
                "{\"pair\":\"USD/JPY\",\"rate\":%.6f,\"date\":\"%s\",\"inverse\":%.6f}",
                usdjpy, date ? date : "", jpyusd);
            db_insert("forex_usdjpy", now, "forex", json_data);
            total++;
        }

        /* CHF/USD */
        double chfusd = json_number_value(json_object_get(rates, "CHF"));
        if (chfusd > 0) {
            char json_data[256];
            snprintf(json_data, sizeof(json_data),
                "{\"pair\":\"USD/CHF\",\"rate\":%.6f,\"date\":\"%s\"}",
                1.0/chfusd, date ? date : "");
            db_insert("forex_usdchf", now, "forex", json_data);
            total++;
        }

        /* AUD/USD */
        double audusd = json_number_value(json_object_get(rates, "AUD"));
        if (audusd > 0) {
            char json_data[256];
            snprintf(json_data, sizeof(json_data),
                "{\"pair\":\"AUD/USD\",\"rate\":%.6f,\"date\":\"%s\",\"inverse\":%.6f}",
                audusd, date ? date : "", 1.0/audusd);
            db_insert("forex_audusd", now, "forex", json_data);
            total++;
        }

        /* CAD/USD */
        double cadusd = json_number_value(json_object_get(rates, "CAD"));
        if (cadusd > 0) {
            char json_data[256];
            snprintf(json_data, sizeof(json_data),
                "{\"pair\":\"USD/CAD\",\"rate\":%.6f,\"date\":\"%s\",\"inverse\":%.6f}",
                1.0/cadusd, date ? date : "", cadusd);
            db_insert("forex_usdcad", now, "forex", json_data);
            total++;
        }
    }

    json_decref(root);
    db_close();
    curl_global_cleanup();
    printf("[FX] %d forex sources updated\n", total);
    return 0;
}
