/**
 * fear_greed_collector.c — C102: Fear & Greed Index Collector
 *
 * Fetches Fear & Greed index from alternative.me free API.
 * Writes to timeline.db with source='fear_greed_*'.
 *
 * Build: gcc -O2 -o fear_greed_collector fear_greed_collector.c -lcurl -ljansson -lsqlite3
 * Usage: ./fear_greed_collector
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <jansson.h>

#define FG_API "https://api.alternative.me/fng"
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
        curl_easy_setopt(c, CURLOPT_USERAGENT, "fng-collector/1.0");
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
    printf("[FG] Fear & Greed Collector\n");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    db_init();
    long long now = (long long)time(NULL);
    int total = 0;

    /* Current value */
    {
        RespBuf resp = http_get(FG_API "/?limit=1");
        json_error_t err;
        json_t *root = json_loads(resp.data, 0, &err);
        free(resp.data);

        if (root) {
            json_t *data = json_object_get(root, "data");
            if (data && json_is_array(data) && json_array_size(data) > 0) {
                json_t *entry = json_array_get(data, 0);
                const char *val = json_string_value(json_object_get(entry, "value"));
                const char *cls = json_string_value(json_object_get(entry, "value_classification"));
                int v = val ? atoi(val) : 50;

                char json_data[256];
                snprintf(json_data, sizeof(json_data),
                    "{\"value\":%d,\"classification\":\"%s\"}", v, cls ? cls : "neutral");
                db_insert("fear_greed_current", now, "sentiment", json_data);
                total++;
                printf("  Current: %d (%s)\n", v, cls ? cls : "neutral");
            }
            json_decref(root);
        }
    }

    /* Historical (last 30 days) */
    {
        RespBuf resp = http_get(FG_API "/?limit=30");
        json_error_t err;
        json_t *root = json_loads(resp.data, 0, &err);
        free(resp.data);

        if (root) {
            json_t *data = json_object_get(root, "data");
            if (data && json_is_array(data)) {
                int n = (int)json_array_size(data);
                for (int i = 0; i < n; i++) {
                    json_t *entry = json_array_get(data, i);
                    const char *val = json_string_value(json_object_get(entry, "value"));
                    const char *ts_str = json_string_value(json_object_get(entry, "timestamp"));
                    const char *cls = json_string_value(json_object_get(entry, "value_classification"));
                    if (!val || !ts_str) continue;

                    int v = atoi(val);
                    long long entry_ts = (long long)atol(ts_str);

                    char json_data[256];
                    snprintf(json_data, sizeof(json_data),
                        "{\"value\":%d,\"classification\":\"%s\",\"timestamp\":%s}",
                        v, cls ? cls : "neutral", ts_str);

                    db_insert("fear_greed_daily", entry_ts, "sentiment", json_data);
                    total++;
                }
            }
            json_decref(root);
        }
    }

    db_close();
    curl_global_cleanup();
    printf("[FG] %d sentiment sources updated\n", total);
    return 0;
}
