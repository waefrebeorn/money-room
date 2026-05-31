/**
 * yahoo_collector.c — Yahoo Finance v7 chart data collector
 * Uses v7 endpoint with range param (not v8 with period — was returning 429)
 * Free, no API key needed.
 *
 * Fetches historical OHLCV for stocks, ETFs, forex, bonds, commodities
 * Writes to timeline.db with source='yahoo_{ticker}'
 *
 * Tickers: SPY,QQQ,IWM,DIA,GLD,SLV,USO,TLT,IEF,EEM,VNQ,
 *          EURUSD=X,GBPUSD=X,USDJPY=X,^VIX,^TNX,GC=F,CL=F
 *
 * Compile: gcc -O2 -Wall -o yahoo_collector yahoo_collector.c -lcurl -ljansson -lsqlite3 -lm
 * Usage:   ./yahoo_collector [--range 1y|5y|max] [--interval 1d|1wk|1mo]
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
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    h = curl_slist_append(h, "Accept: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return r == CURLE_OK ? b.d : (free(b.d), NULL);
}

// Normalize ticker for source name
static void norms(const char *in, char *out, int sz) {
    int i = 0;
    for (int j = 0; in[j] && i < sz-1; j++) {
        char c = in[j];
        if (c == '=' || c == '^' || c == '-') out[i++] = '_';
        else out[i++] = c;
    }
    out[i] = 0;
}

static const char *TICKERS[] = {
    "SPY","QQQ","IWM","DIA",
    "GLD","SLV","USO","DBC",
    "TLT","IEF","SHY","LQD","HYG","JNK",
    "EEM","FXI","EWJ","EWZ",
    "XLF","XLK","XLE","XLB","XLI","XLV","XLY","XLP","XLU","XLRE",
    "VIG","SCHD","DGRO","VNQ","REET",
    "BITO","GBTC","IBIT",
    "UVXY","SVXY",
    "EURUSD=X","GBPUSD=X","USDJPY=X",
    "USDCAD=X","USDCHF=X","AUDUSD=X","NZDUSD=X","USDMXN=X",
    "^VIX","^TNX","^FVX","^TYX","^IRX","^RUT",
    "GC=F","CL=F","SI=F","NG=F","HG=F",
    "BTC-USD","ETH-USD",
    NULL
};

int main(int argc, char **argv) {
    const char *range = "5y";
    const char *interval = "1d";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--range") == 0 && i+1 < argc) range = argv[++i];
        if (strcmp(argv[i], "--interval") == 0 && i+1 < argc) interval = argv[++i];
    }

    sqlite3 *db = NULL;
    sqlite3_open(DB, &db);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS timeline(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "ts INTEGER NOT NULL,source TEXT NOT NULL,category TEXT NOT NULL,data TEXT NOT NULL,"
        "collected_at INTEGER DEFAULT(strftime('%s','now')))",0,0,0);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS i_ts ON timeline(ts)",0,0,0);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS i_src ON timeline(source)",0,0,0);

    int ins = 0, errs = 0;
    for (int t = 0; TICKERS[t]; t++) {
        const char *tkr = TICKERS[t];
        char sn[64]; norms(tkr, sn, sizeof(sn));
        char src[80]; snprintf(src, sizeof(src), "yahoo_%s", sn);

        // Check last timestamp
        sqlite3_stmt *st = NULL;
        int64_t last_ts = 0;
        if (sqlite3_prepare_v2(db, "SELECT MAX(ts) FROM timeline WHERE source=?1", -1, &st, 0) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, src, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) last_ts = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }

        // Fetch from v7 endpoint (using range= parameter — avoids 429 from period-based v8)
        char url[512];
        snprintf(url, sizeof(url),
            "https://query2.finance.yahoo.com/v7/finance/chart/%s?range=%s&interval=%s",
            tkr, range, interval);

        char *body = get(url);
        if (!body) { errs++; printf("[yahoo] %s: FAIL (no response)\n", tkr); continue; }

        json_t *root = json_loads(body, 0, NULL); free(body);
        if (!root) { errs++; printf("[yahoo] %s: FAIL (JSON parse)\n", tkr); continue; }

        json_t *ch = json_object_get(root, "chart");
        json_t *res_a = ch ? json_object_get(ch, "result") : NULL;
        json_t *res = (res_a && json_array_size(res_a) > 0) ? json_array_get(res_a, 0) : NULL;
        if (!res) {
            json_t *err = ch ? json_object_get(ch, "error") : NULL;
            if (err) {
                json_t *jd = json_object_get(err, "description");
                const char *desc = jd && json_is_string(jd) ? json_string_value(jd) : "unknown";
                printf("[yahoo] %s: API error: %s\n", tkr, desc);
            } else printf("[yahoo] %s: no result\n", tkr);
            json_decref(root); errs++; continue;
        }

        json_t *ts_a = json_object_get(res, "timestamp");
        json_t *ind = json_object_get(res, "indicators");
        json_t *quo_a = ind ? json_object_get(ind, "quote") : NULL;
        json_t *quo = (quo_a && json_array_size(quo_a) > 0) ? json_array_get(quo_a, 0) : NULL;
        if (!ts_a || !json_is_array(ts_a) || !quo) {
            json_decref(root); errs++; continue;
        }

        // Determine category
        const char *cat = "stocks";
        if (strstr(tkr, "=X")) cat = "forex";
        else if (tkr[0] == '^') {
            if (strstr(tkr, "VIX")) cat = "volatility";
            else if (strstr(tkr, "TNX") || strstr(tkr, "FVX") || strstr(tkr, "TYX") || strstr(tkr, "IRX")) cat = "bond_yields";
            else cat = "index";
        } else if (strstr(tkr, "=F")) cat = "futures";
        else if (strstr(tkr, "BTC") || strstr(tkr, "ETH")) cat = "crypto";

        size_t n = json_array_size(ts_a);
        int tkr_ins = 0;

        for (size_t i = 0; i < n; i++) {
            json_t *jt = json_array_get(ts_a, i);
            if (!jt || !json_is_integer(jt)) continue;
            int64_t ts = json_integer_value(jt);
            if (ts <= last_ts) continue;

            double o = 0, h = 0, l = 0, c = 0, v = 0;
            json_t *jo = json_array_get(json_object_get(quo, "open"), i);
            json_t *jh = json_array_get(json_object_get(quo, "high"), i);
            json_t *jl = json_array_get(json_object_get(quo, "low"), i);
            json_t *jc = json_array_get(json_object_get(quo, "close"), i);
            json_t *jv = json_array_get(json_object_get(quo, "volume"), i);
            if (jo && json_is_real(jo)) o = json_real_value(jo);
            if (jh && json_is_real(jh)) h = json_real_value(jh);
            if (jl && json_is_real(jl)) l = json_real_value(jl);
            if (jc && json_is_real(jc)) c = json_real_value(jc);
            if (jv && json_is_real(jv)) v = json_real_value(jv);
            if (c == 0) continue;

            json_t *val = json_pack("{s:f,s:f,s:f,s:f,s:f,s:s}",
                "open", o, "high", h, "low", l, "close", c, "volume", v, "interval", interval);
            char *vs = json_dumps(val, JSON_COMPACT); json_decref(val);

            sqlite3_stmt *ist = NULL;
            if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO timeline(ts,source,category,data,collected_at)"
                    "VALUES(?1,?2,?3,?4,strftime('%s','now'))", -1, &ist, 0) == SQLITE_OK) {
                sqlite3_bind_int64(ist, 1, ts);
                sqlite3_bind_text(ist, 2, src, -1, SQLITE_STATIC);
                sqlite3_bind_text(ist, 3, cat, -1, SQLITE_STATIC);
                sqlite3_bind_text(ist, 4, vs, -1, SQLITE_STATIC);
                if (sqlite3_step(ist) == SQLITE_DONE) tkr_ins++;
                sqlite3_finalize(ist);
            }
            free(vs);
        }
        json_decref(root);
        ins += tkr_ins;
        printf("[yahoo] %s: %d new rows (%s, %s)\n", tkr, tkr_ins, range, interval);
    }

    printf("[yahoo] Done. %d inserted, %d errors\n", ins, errs);
    sqlite3_close(db);
    return 0;
}
