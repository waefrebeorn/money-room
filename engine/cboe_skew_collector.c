/**
 * cboe_skew_collector.c — CBOE SKEW Index + VIX data collector
 * Free CSV endpoints, no API key needed
 * SKEW: cdn.cboe.com/api/global/us_indices/daily_prices/SKEW_History.csv
 * VIX:  same pattern
 *
 * Compile: gcc -O2 -Wall -o cboe_skew_collector cboe_skew_collector.c -lcurl -ljansson -lsqlite3 -lm
 * Usage:   ./cboe_skew_collector
 */
#define _POSIX_C_SOURCE 199309L
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <sqlite3.h>

#define DB "/home/wubu2/.hermes/pm_logs/timeline.db"

typedef struct { char *d; size_t l; } buf_t;
static size_t wcb(void *p, size_t s, size_t n, void *u) {
    size_t t = s * n; buf_t *b = u;
    char *np = realloc(b->d, b->l + t + 1);
    if (!np) return 0; b->d = np;
    memcpy(b->d + b->l, p, t); b->l += t; b->d[b->l] = 0;
    return t;
}

static char *get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    buf_t b = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return r == CURLE_OK ? b.d : (free(b.d), NULL);
}

// Parse CSV line (simple: split by comma). Returns field count.
static int parse_csv(const char *line, const char **fields, int maxf) {
    int n = 0; const char *p = line;
    while (*p && n < maxf) {
        fields[n++] = p;
        while (*p && *p != ',') p++;
        if (*p) { *(char*)p = 0; p++; }
    }
    return n;
}

static const char *INDICES[] = {"SKEW", "VIX", NULL};

int main(void) {
    sqlite3 *db = NULL;
    sqlite3_open(DB, &db);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS timeline(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "ts INTEGER NOT NULL,source TEXT NOT NULL,category TEXT NOT NULL,data TEXT NOT NULL,"
        "collected_at INTEGER DEFAULT(strftime('%s','now')))",0,0,0);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS i_ts ON timeline(ts)",0,0,0);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS i_src ON timeline(source)",0,0,0);

    int ins = 0;

    for (int i = 0; INDICES[i]; i++) {
        const char *idx = INDICES[i];
        char url[256];
        snprintf(url, sizeof(url), "https://cdn.cboe.com/api/global/us_indices/daily_prices/%s_History.csv", idx);

        char *body = get(url);
        if (!body) { printf("[cboe] %s: FAIL\n", idx); continue; }

        // Parse CSV
        char *line = body;
        int line_n = 0;
        char *next;
        while ((next = strchr(line, '\n'))) {
            *next = 0;
            line_n++;
            if (line_n == 1) { line = next + 1; continue; } // Skip header

            const char *f[4] = {0};
            int nf = parse_csv(line, f, 4);
            if (nf < 2) { line = next + 1; continue; }

            // f[0] = date (YYYY-MM-DD), f[1] = close value
            struct tm tm = {0};
            if (!strptime(f[0], "%m/%d/%Y", &tm)) { line = next + 1; continue; }
            int64_t ts = (int64_t)timegm(&tm);
            double val = atof(f[1]);
            if (val == 0) { line = next + 1; continue; }

            char src[64]; snprintf(src, sizeof(src), "cboe_%s", idx);
            const char *cat = (strcmp(idx, "VIX") == 0) ? "volatility" : "options";

            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO timeline(ts,source,category,data,collected_at)"
                    "VALUES(?1,?2,?3,?4,strftime('%s','now'))", -1, &st, 0) == SQLITE_OK) {
                char val_json[64]; snprintf(val_json, sizeof(val_json), "{\"close\":%s}", f[1]);
                sqlite3_bind_int64(st, 1, ts);
                sqlite3_bind_text(st, 2, src, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 3, cat, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 4, val_json, -1, SQLITE_STATIC);
                if (sqlite3_step(st) == SQLITE_DONE) ins++;
                sqlite3_finalize(st);
            }
            line = next + 1;
        }
        free(body);
        printf("[cboe] %s: %d new rows\n", idx, ins);
    }

    printf("[cboe] Done. %d total rows inserted\n", ins);
    sqlite3_close(db);
    return 0;
}
