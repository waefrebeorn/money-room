/**
 * okx_collector.c — OKX crypto derivatives data collector
 * Fetches funding rates and open interest for perpetual swaps
 * Free API, no key needed
 *
 * Compile: gcc -O2 -Wall -o okx_collector okx_collector.c -lcurl -ljansson -lsqlite3 -lm
 * Usage:   ./okx_collector
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>

#define DB "/home/wubu2/.hermes/pm_logs/timeline.db"
#define API "https://www.okx.com"

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
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return r == CURLE_OK ? b.d : (free(b.d), NULL);
}

static const char *MKTS[] = {
    "BTC-USDT-SWAP","ETH-USDT-SWAP","SOL-USDT-SWAP",
    "XRP-USDT-SWAP","DOGE-USDT-SWAP","ADA-USDT-SWAP",
    "AVAX-USDT-SWAP","LINK-USDT-SWAP","DOT-USDT-SWAP",
    "ARB-USDT-SWAP","OP-USDT-SWAP","SUI-USDT-SWAP",
    "APT-USDT-SWAP","PEPE-USDT-SWAP",NULL
};

// Normalize market name for source field
static void norm(const char *in, char *out, int sz) {
    int i = 0;
    for (int j = 0; in[j] && i < sz-1; j++) {
        char c = in[j];
        if (c == '-') out[i++] = '_';
        else out[i++] = c;
    }
    out[i] = 0;
}

int main(void) {
    sqlite3 *db = NULL;
    sqlite3_open(DB, &db);

    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS timeline(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "ts INTEGER NOT NULL,source TEXT NOT NULL,category TEXT NOT NULL,data TEXT NOT NULL,"
        "collected_at INTEGER DEFAULT(strftime('%s','now')))",0,0,0);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS i_ts ON timeline(ts)",0,0,0);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS i_src ON timeline(source)",0,0,0);

    time_t now = time(NULL);
    time_t wh = now - (now % 3600);
    int ins = 0;

    for (int m = 0; MKTS[m]; m++) {
        char m_norm[64]; norm(MKTS[m], m_norm, sizeof(m_norm));

        // Funding rate
        char url[256];
        snprintf(url, sizeof(url), "%s/api/v5/public/funding-rate?instId=%s", API, MKTS[m]);
        char *body = get(url);
        if (body) {
            json_t *r = json_loads(body, 0, NULL); free(body);
            if (r) {
                json_t *d = json_object_get(r, "data");
                if (d && json_array_size(d) > 0) {
                    json_t *e = json_array_get(d, 0);
                    json_t *jf = json_object_get(e, "fundingRate");
                    json_t *jt = json_object_get(e, "fundingTime");
                    if (jf && json_is_string(jf)) {
                        double fr = atof(json_string_value(jf));
                        int64_t ft = (jt && json_is_string(jt)) ? atol(json_string_value(jt))/1000 : (int64_t)wh;
                        char src[128];
                        snprintf(src, sizeof(src), "okx_%s_funding", m_norm);
                        sqlite3_stmt *st = NULL;
                        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO timeline(ts,source,category,data,collected_at)"
                                "VALUES(?1,?2,'crypto_derivatives',?3,strftime('%s','now'))", -1, &st, 0) == SQLITE_OK) {
                            json_t *v = json_pack("{s:f}", "rate", fr);
                            char *vs = json_dumps(v, JSON_COMPACT); json_decref(v);
                            sqlite3_bind_int64(st, 1, ft);
                            sqlite3_bind_text(st, 2, src, -1, SQLITE_STATIC);
                            sqlite3_bind_text(st, 3, vs, -1, SQLITE_STATIC);
                            if (sqlite3_step(st) == SQLITE_DONE) ins++;
                            sqlite3_finalize(st); free(vs);
                        }
                    }
                }
                json_decref(r);
            }
        }

        // Open interest
        snprintf(url, sizeof(url), "%s/api/v5/public/open-interest?instId=%s&instType=SWAP", API, MKTS[m]);
        body = get(url);
        if (body) {
            json_t *r = json_loads(body, 0, NULL); free(body);
            if (r) {
                json_t *d = json_object_get(r, "data");
                if (d && json_array_size(d) > 0) {
                    json_t *e = json_array_get(d, 0);
                    json_t *jv = json_object_get(e, "oi");
                    json_t *jc = json_object_get(e, "oiCcy");
                    if (jv && json_is_string(jv)) {
                        double oi = atof(json_string_value(jv));
                        double oi_usd = (jc && json_is_string(jc)) ? atof(json_string_value(jc)) : 0;
                        char src[128];
                        snprintf(src, sizeof(src), "okx_%s_oi", m_norm);
                        sqlite3_stmt *st = NULL;
                        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO timeline(ts,source,category,data,collected_at)"
                                "VALUES(?1,?2,'crypto_derivatives',?3,strftime('%s','now'))", -1, &st, 0) == SQLITE_OK) {
                            json_t *v = json_pack("{s:f,s:f}", "oi_contracts", oi, "oi_usd", oi_usd);
                            char *vs = json_dumps(v, JSON_COMPACT); json_decref(v);
                            sqlite3_bind_int64(st, 1, wh);
                            sqlite3_bind_text(st, 2, src, -1, SQLITE_STATIC);
                            sqlite3_bind_text(st, 3, vs, -1, SQLITE_STATIC);
                            if (sqlite3_step(st) == SQLITE_DONE) ins++;
                            sqlite3_finalize(st); free(vs);
                        }
                    }
                }
                json_decref(r);
            }
        }
        printf("[okx] %s: OK\n", MKTS[m]);
    }
    printf("[okx] Done. %d new rows\n", ins);
    sqlite3_close(db);
    return 0;
}
