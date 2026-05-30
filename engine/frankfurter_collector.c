/**
 * frankfurter_collector.c — Frankfurter.dev forex rate collector (T281)
 * Fetches EUR/USD and other major forex pairs — free, no key.
 * Historical data from 1999-01-04 to present.
 * Stores in timeline.db (separate from historical.db — no lock conflicts).
 *
 * API: https://api.frankfurter.app/START..END?from=BASE&to=TARGET
 *
 * Build: gcc -O2 frankfurter_collector.c -o frankfurter_collector -lcurl -ljansson -lsqlite3 -lm
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

#define DB_PATH     "/home/wubu2/.hermes/pm_logs/timeline.db"
#define HB_DIR      "/home/wubu2/.hermes/infra/heartbeats"
#define HB_PATH     HB_DIR "/frankfurter.heartbeat"
#define FEAT_DIR    "/home/wubu2/.hermes/news_cache"
#define FEAT_PATH   FEAT_DIR "/forex_features.json"

/* Major pairs to track */
static const char *PAIRS[][2] = {
    {"USD", "EUR"}, {"USD", "GBP"}, {"USD", "JPY"},
    {"USD", "CHF"}, {"USD", "CAD"}, {"USD", "AUD"},
    {"USD", "CNH"}, {"USD", "MXN"}, {"USD", "KRW"},
    {"EUR", "GBP"}, {"EUR", "JPY"}, {"GBP", "JPY"},
};
#define N_PAIRS (sizeof(PAIRS) / sizeof(PAIRS[0]))

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
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "money-room/1.0");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(mb.data); return NULL; }
    return mb.data;
}

static void touch_heartbeat(void) {
    mkdir(HB_DIR, 0755);
    FILE *f = fopen(HB_PATH, "w");
    if (f) { fprintf(f, "%ld\n", time(NULL)); fclose(f); }
}

static int ensure_tables(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS forex_rates ("
        "  ts INTEGER NOT NULL,"
        "  pair TEXT NOT NULL,"
        "  rate REAL NOT NULL,"
        "  PRIMARY KEY (ts, pair)"
        ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int count_existing(sqlite3 *db, const char *pair) {
    sqlite3_stmt *s;
    int count = 0;
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM forex_rates WHERE pair = '%s';", pair);
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) count = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return count;
}

static int insert_rate(sqlite3 *db, long ts, const char *pair, double rate) {
    sqlite3_stmt *s;
    const char *sql = "INSERT OR IGNORE INTO forex_rates (ts, pair, rate) VALUES (?1, ?2, ?3);";
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)ts);
    sqlite3_bind_text(s, 2, pair, -1, SQLITE_STATIC);
    sqlite3_bind_double(s, 3, rate);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static int date_to_ts(const char *date) {
    struct tm tm = {0};
    if (sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (int)mktime(&tm);
}

static int write_features(sqlite3 *db) {
    mkdir(FEAT_DIR, 0755);
    json_t *root = json_object();
    json_object_set_new(root, "source", json_string("frankfurter"));
    json_object_set_new(root, "timestamp", json_integer((json_int_t)time(NULL)));

    json_t *features = json_object();
    json_t *pairs = json_object();

    for (size_t i = 0; i < N_PAIRS; i++) {
        char pair[16];
        snprintf(pair, sizeof(pair), "%s%s", PAIRS[i][0], PAIRS[i][1]);

        sqlite3_stmt *s;
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT rate FROM forex_rates WHERE pair = '%s' ORDER BY ts DESC LIMIT 1;", pair);
        double last_rate = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW)
                last_rate = sqlite3_column_double(s, 0);
            sqlite3_finalize(s);
        }

        /* 30-day change */
        long month_ago = (long)time(NULL) - 30 * 86400;
        snprintf(sql, sizeof(sql),
            "SELECT rate FROM forex_rates WHERE pair = '%s' AND ts <= ?1 ORDER BY ts DESC LIMIT 1;", pair);
        double month_ago_rate = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)month_ago);
            if (sqlite3_step(s) == SQLITE_ROW)
                month_ago_rate = sqlite3_column_double(s, 0);
            sqlite3_finalize(s);
        }

        json_t *p = json_object();
        json_object_set_new(p, "rate", json_real(last_rate));
        if (month_ago_rate > 0)
            json_object_set_new(p, "change_30d_pct", json_real((last_rate / month_ago_rate - 1.0) * 100.0));
        json_object_set_new(p, "pair", json_string(pair));
        json_object_set_new(pairs, pair, p);

        /* Feature outputs */
        char feat_key[64];
        snprintf(feat_key, sizeof(feat_key), "F40_%s_rate", pair);
        json_object_set_new(features, feat_key, json_real(last_rate));
    }

    json_object_set_new(root, "pairs", pairs);
    json_object_set_new(root, "features", features);

    int ret = 0;
    if (json_dump_file(root, FEAT_PATH, JSON_INDENT(2) | JSON_REAL_PRECISION(2)) != 0) {
        fprintf(stderr, "Failed to write features\n");
        ret = -1;
    }
    json_decref(root);
    return ret;
}

int main(void) {
    curl_global_init(CURL_GLOBAL_ALL);

    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open DB: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    if (ensure_tables(db) != 0) return 1;

    time_t t0 = time(NULL);

    /* Process each pair */
    for (size_t i = 0; i < N_PAIRS; i++) {
        const char *from = PAIRS[i][0];
        const char *to = PAIRS[i][1];
        char pair[16];
        snprintf(pair, sizeof(pair), "%s%s", from, to);

        int existing = count_existing(db, pair);
        if (existing > 1000) {
            printf("[FX %s] Skipping — %d rows already exist\n", pair, existing);
            continue;
        }

        /* Fetch full history (1999-01-04 to today) */
        char url[512];
        snprintf(url, sizeof(url),
                 "https://api.frankfurter.app/1999-01-04..2026-05-29?from=%s&to=%s",
                 from, to);

        printf("[FX %s] Fetching %s → %s...\n", pair, from, to);
        char *json = http_get(url);
        if (!json) {
            fprintf(stderr, "[FX %s] Failed to fetch\n", pair);
            continue;
        }

        json_t *root = json_loads(json, 0, NULL);
        free(json);

        if (!root) {
            fprintf(stderr, "[FX %s] Failed to parse JSON\n", pair);
            continue;
        }

        json_t *rates = json_object_get(root, "rates");
        if (!json_is_object(rates)) {
            fprintf(stderr, "[FX %s] No rates in response\n", pair);
            json_decref(root);
            continue;
        }

        int inserted = 0, skipped = 0;
        sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

        const char *date;
        json_t *val;
        json_object_foreach(rates, date, val) {
            json_t *rate_val = json_object_get(val, to);
            if (!json_is_real(rate_val)) { skipped++; continue; }
            double rate = json_real_value(rate_val);
            long ts = date_to_ts(date);
            if (ts == 0) { skipped++; continue; }
            if (insert_rate(db, ts, pair, rate) == 0)
                inserted++;
            else
                skipped++;
        }

        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        json_decref(root);

        printf("[FX %s] Inserted: %d, Skipped: %d, Total: %d\n",
               pair, inserted, skipped, count_existing(db, pair));
    }

    /* Write features */
    if (write_features(db) == 0) {
        printf("\n[FX] Features written to %s\n", FEAT_PATH);
    }

    /* Print latest rates */
    printf("\n[FX] Latest forex rates:\n");
    printf("┌──────────┬──────────┬───────────┐\n");
    printf("│ Pair     │ Rate     │ 30d Chg   │\n");
    printf("├──────────┼──────────┼───────────┤\n");
    for (size_t i = 0; i < N_PAIRS; i++) {
        char pair[16];
        snprintf(pair, sizeof(pair), "%s%s", PAIRS[i][0], PAIRS[i][1]);
        sqlite3_stmt *s;
        double last = 0, month_ago = 0;
        char sql[256];
        snprintf(sql, sizeof(sql), "SELECT rate FROM forex_rates WHERE pair='%s' ORDER BY ts DESC LIMIT 1;", pair);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) last = sqlite3_column_double(s, 0);
            sqlite3_finalize(s);
        }
        long ts = (long)time(NULL) - 30 * 86400;
        snprintf(sql, sizeof(sql), "SELECT rate FROM forex_rates WHERE pair='%s' AND ts<=?1 ORDER BY ts DESC LIMIT 1;", pair);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)ts);
            if (sqlite3_step(s) == SQLITE_ROW) month_ago = sqlite3_column_double(s, 0);
            sqlite3_finalize(s);
        }
        if (last > 0) {
            double chg = month_ago > 0 ? (last / month_ago - 1.0) * 100.0 : 0;
            printf("│ %-8s │ %7.4f  │ %+7.2f%% │\n", pair, last, chg);
        }
    }
    printf("└──────────┴──────────┴───────────┘\n");

    time_t elapsed = time(NULL) - t0;
    printf("\n[FX] Done in %lds\n", (long)elapsed);

    touch_heartbeat();
    sqlite3_close(db);
    curl_global_cleanup();
    return 0;
}
