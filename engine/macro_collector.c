/*
 * macro_collector.c — Stock indices & macro data collector
 * Fetches from Yahoo Finance v8 API (no key needed).
 * Writes to timeline.db with source names that training_pile.c expects.
 *
 * Sources: fred_sp500, fred_vix, stock_gold, stock_crude_oil,
 *          stock_usd_index, stock_nasdaq, stock_dow, stock_vix
 *
 * gcc -O2 -o macro_collector macro_collector.c -lcurl -ljansson -lsqlite3 -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <jansson.h>

#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define UA "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"

typedef struct { const char *source; const char *symbol; const char *category; } IndexSource;
static const IndexSource SOURCES[] = {
    {"fred_sp500",    "^GSPC",  "stock_index"},
    {"fred_vix",      "^VIX",   "stock_index"},
    {"stock_vix",     "^VIX",   "stock_index"},
    {"stock_nasdaq",  "^IXIC",  "stock_index"},
    {"stock_dow",     "^DJI",   "stock_index"},
    {"stock_gold",    "GC=F",   "commodity"},
    {"stock_crude_oil","CL=F",  "commodity"},
    {"stock_usd_index","DX-Y.NYB","currency"},
    {NULL, NULL, NULL}
};

struct mbuf { char *d; size_t l; };
static size_t wcb(void *p, size_t s, size_t n, void *u) {
    size_t t = s*n; struct mbuf *m = u;
    char *np = realloc(m->d, m->l + t + 1);
    if (!np) return 0; m->d = np;
    memcpy(m->d + m->l, p, t); m->l += t; m->d[m->l] = 0;
    return t;
}

static json_t *fetch_chart(const char *symbol) {
    char url[512];
    snprintf(url, sizeof(url),
        "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=5d",
        symbol);
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    struct mbuf mb = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (r != CURLE_OK || !mb.d) { free(mb.d); return NULL; }
    json_error_t err;
    json_t *j = json_loads(mb.d, 0, &err);
    free(mb.d);
    return j;
}

static sqlite3 *g_db = NULL;

static void db_init(void) {
    sqlite3_open(DB_PATH, &g_db);
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;", NULL, NULL, NULL);
}

static void db_insert(const char *source, long long ts, const char *cat, double close, double open) {
    if (!g_db) return;
    char json[512];
    snprintf(json, sizeof(json),
        "{\"close\":%.2f,\"open\":%.2f}",
        isfinite(close) ? close : 0.0,
        isfinite(open)  ? open  : 0.0);
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO timeline (ts, source, category, data, collected_at) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, cat, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (long long)time(NULL));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    (void)rc; /* silent ignore */
}

static void db_close(void) { if (g_db) sqlite3_close(g_db); }

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    db_init();

    int total = 0, errors = 0;
    time_t now = time(NULL);

    for (int i = 0; SOURCES[i].source; i++) {
        printf("  %s (%s)... ", SOURCES[i].source, SOURCES[i].symbol);
        fflush(stdout);

        json_t *root = fetch_chart(SOURCES[i].symbol);
        if (!root) { printf("FETCH FAILED\n"); errors++; continue; }

        json_t *result = json_object_get(root, "chart");
        result = result ? json_object_get(result, "result") : NULL;
        if (!result || !json_is_array(result) || json_array_size(result) == 0) {
            printf("NO DATA\n"); json_decref(root); errors++; continue;
        }
        json_t *r0 = json_array_get(result, 0);
        json_t *ts_arr = json_object_get(r0, "timestamp");
        json_t *quotes = json_object_get(r0, "indicators");
        quotes = quotes ? json_object_get(quotes, "quote") : NULL;
        if (!ts_arr || !json_is_array(ts_arr) || !quotes || !json_is_array(quotes) || json_array_size(quotes) == 0) {
            printf("INVALID\n"); json_decref(root); errors++; continue;
        }
        json_t *q0 = json_array_get(quotes, 0);

        int n = (int)json_array_size(ts_arr);
        json_t *close_arr = json_object_get(q0, "close");
        json_t *open_arr  = json_object_get(q0, "open");
        int written = 0;
        for (int j = 0; j < n; j++) {
            long long ts = (long long)json_integer_value(json_array_get(ts_arr, j));
            double close = 0, open = 0;
            if (close_arr && json_is_array(close_arr) && j < (int)json_array_size(close_arr))
                close = json_number_value(json_array_get(close_arr, j));
            if (open_arr && json_is_array(open_arr) && j < (int)json_array_size(open_arr))
                open  = json_number_value(json_array_get(open_arr, j));
            if (!isfinite(close) || close <= 0.0) continue;

            /* For VIX/stock_vix, read from the most recent close */
            db_insert(SOURCES[i].source, ts, SOURCES[i].category, close, open);
            written++;
        }
        json_decref(root);
        printf("%d written\n", written);
        total += written;

        /* Rate limit */
        struct timespec ts = {0, 200000000};
        nanosleep(&ts, NULL);
    }

    db_close();
    curl_global_cleanup();

    printf("\n  macro_collector: %d total entries, %d errors\n", total, errors);
    printf("  Data sources: SP500, VIX, NASDAQ, DOW, Gold, Crude Oil, USD Index\n");
    return errors > 3 ? 1 : 0;
}
