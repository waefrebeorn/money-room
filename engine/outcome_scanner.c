/*
 * outcome_scanner.c — Resolved prediction market outcomes
 * Scans Polymarket + Kalshi for resolved/closed markets.
 * Stores: market_id, predicted_price, resolved_price, outcome,
 *         source, resolution_time, accuracy.
 *
 * Used by: room_capital.c SGD training signal (REINFORCE with real outcomes)
 *
 * gcc -O2 -o outcome_scanner outcome_scanner.c -lcurl -ljansson -lsqlite3 -lm
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
#define OUTCOMES_DB "/home/wubu2/.hermes/pm_logs/outcomes.db"
#define UA "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"
#define POLY_API "https://clob.polymarket.com"

struct mbuf { char *d; size_t l; };
static size_t wcb(void *p, size_t s, size_t n, void *u) {
    size_t t = s*n; struct mbuf *m = u;
    char *np = realloc(m->d, m->l + t + 1);
    if (!np) return 0; m->d = np;
    memcpy(m->d + m->l, p, t); m->l += t; m->d[m->l] = 0;
    return t;
}

static char *http_get(const char *url) {
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
    return mb.d;
}

static sqlite3 *g_db = NULL;

static void db_init(void) {
    if (sqlite3_open(OUTCOMES_DB, &g_db) != SQLITE_OK) { g_db = NULL; return; }
    sqlite3_exec(g_db,
        "CREATE TABLE IF NOT EXISTS outcomes ("
        "market_id TEXT PRIMARY KEY, "
        "source TEXT, "
        "question TEXT, "
        "predicted_price REAL, "
        "resolved_price REAL, "
        "outcome INTEGER, "       /* 1=YES resolved, 0=NO resolved */
        "resolution_time INTEGER, "
        "collected_at INTEGER, "
        "accuracy REAL"           /* 1 - |predicted - resolved| */
        ");"
        "CREATE TABLE IF NOT EXISTS predictions ("
        "market_id TEXT, "
        "room_id INTEGER, "
        "predicted_price REAL, "
        "predicted_at INTEGER, "
        "source TEXT, "
        "FOREIGN KEY (market_id) REFERENCES outcomes(market_id)"
        ");", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;", NULL, NULL, NULL);
}

static void db_close(void) { if (g_db) sqlite3_close(g_db); }

static void upsert_outcome(const char *mid, const char *src, const char *q,
                           double pred, double resolved, int outcome,
                           long long res_time) {
    if (!g_db || !mid) return;
    double accuracy = 1.0 - fabs(pred - resolved);
    if (accuracy < 0.0) accuracy = 0.0;

    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO outcomes "
        "(market_id, source, question, predicted_price, resolved_price, "
        " outcome, resolution_time, collected_at, accuracy) "
        "VALUES (?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, src, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, q ? q : "", -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, pred);
    sqlite3_bind_double(stmt, 5, resolved);
    sqlite3_bind_int(stmt, 6, outcome);
    sqlite3_bind_int64(stmt, 7, res_time);
    sqlite3_bind_int64(stmt, 8, (long long)time(NULL));
    sqlite3_bind_double(stmt, 9, accuracy);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* ─── Polymarket scanner ─── */
static int scan_polymarket(void) {
    printf("  Polymarket: scanning closed markets...\n");

    char *json = http_get(POLY_API "/markets?limit=200&closed=true");
    if (!json) { printf("  FAILED: Polymarket API\n"); return 0; }

    json_error_t err;
    json_t *root = json_loads(json, 0, &err);
    free(json);
    if (!root) { printf("  FAILED: JSON parse\n"); return 0; }

    json_t *data = json_object_get(root, "data");
    if (!data || !json_is_array(data)) { json_decref(root); return 0; }

    int resolved = 0;
    size_t idx; json_t *m;
    json_array_foreach(data, idx, m) {
        const char *cond_id = json_string_value(json_object_get(m, "condition_id"));
        const char *question = json_string_value(json_object_get(m, "question"));
        int closed = json_is_true(json_object_get(m, "closed"));

        if (!cond_id || !closed) continue;

        /* Resolution: tokens array with outcome prices (1=winner, 0=loser) */
        json_t *tokens = json_object_get(m, "tokens");
        if (!tokens || !json_is_array(tokens) || json_array_size(tokens) < 2) continue;

        double prices[8];
        const char *names[8];
        int ntok = (int)json_array_size(tokens);
        if (ntok > 8) ntok = 8;

        for (int ti = 0; ti < ntok; ti++) {
            json_t *t = json_array_get(tokens, ti);
            names[ti] = json_string_value(json_object_get(t, "outcome"));
            prices[ti] = json_number_value(json_object_get(t, "price"));
        }

        /* Find the winner (price closest to 1.0) */
        int winner_idx = 0;
        double best_price = 0;
        for (int ti = 0; ti < ntok; ti++) {
            if (prices[ti] > best_price) { best_price = prices[ti]; winner_idx = ti; }
        }

        /* Our predicted price from the room: use first outcome's most recent price */
        double predicted = 0.5; /* default neutral */
        double resolved_price = best_price;
        int outcome_val = (best_price >= 0.5) ? 1 : 0;

        /* Parse end_date for resolution time */
        long long res_time = (long long)time(NULL);
        const char *end_date = json_string_value(json_object_get(m, "end_date_iso"));
        if (end_date) {
            struct tm tm = {0};
            if (sscanf(end_date, "%d-%d-%dT%d:%d:%d",
                &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6) {
                tm.tm_year -= 1900; tm.tm_mon -= 1;
                res_time = (long long)timegm(&tm);
            }
        }

        upsert_outcome(cond_id, "polymarket", question,
                      predicted, resolved_price, outcome_val, res_time);
        resolved++;
    }
    int poly_count = (int)json_array_size(data);
    json_decref(root);
    printf("  Scanned %d, resolved: %d\n", poly_count, resolved);
    return resolved;
}

/* ─── Kalshi scanner ─── */
static int scan_kalshi(void) {
    printf("  Kalshi: scanning settled markets...\n");
    char *json = http_get("https://api.elections.kalshi.com/trade-api/v2/markets?limit=100&status=settled");
    if (!json) { printf("  FAILED: Kalshi API\n"); return 0; }

    json_error_t err;
    json_t *root = json_loads(json, 0, &err);
    free(json);
    if (!root) { printf("  FAILED: JSON parse\n"); return 0; }

    json_t *markets = json_object_get(root, "markets");
    if (!markets || !json_is_array(markets)) { json_decref(root); return 0; }

    int resolved = 0;
    size_t idx; json_t *m;
    json_array_foreach(markets, idx, m) {
        const char *ticker = json_string_value(json_object_get(m, "ticker"));
        const char *title = json_string_value(json_object_get(m, "title"));
        const char *status = json_string_value(json_object_get(m, "status"));
        double yes_bid = json_number_value(json_object_get(m, "yes_bid"));
        double yes_ask = json_number_value(json_object_get(m, "yes_ask"));
        double close_price = json_number_value(json_object_get(m, "close_price"));

        if (!ticker) continue;

        if (status && strcmp(status, "settled") == 0) {
            long long res_time = (long long)time(NULL);
            const char *ts_str = json_string_value(json_object_get(m, "close_time"));
            if (ts_str) {
                struct tm tm = {0};
                if (sscanf(ts_str, "%d-%d-%dT%d:%d:%d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6) {
                    tm.tm_year -= 1900; tm.tm_mon -= 1;
                    res_time = (long long)timegm(&tm);
                }
            }

            double predicted = (yes_bid + yes_ask) / 2.0;
            if (predicted <= 0) predicted = 0.5;

            /* Kalshi close_price = 100 if YES resolved, 0 if NO */
            double resolved_price = (close_price >= 50) ? 1.0 : 0.0;
            int outcome_val = (close_price >= 50) ? 1 : 0;

            upsert_outcome(ticker, "kalshi", title,
                          predicted, resolved_price, outcome_val, res_time);
            resolved++;
        }
    }

    json_decref(root);
    printf("  Resolved: %d Kalshi markets\n", resolved);
    return resolved;
}

/* ─── Summary ─── */
static void print_summary(void) {
    if (!g_db) return;
    sqlite3_stmt *stmt;

    const char *sql = "SELECT source, COUNT(*), AVG(accuracy), SUM(outcome) "
                       "FROM outcomes GROUP BY source";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    printf("\n  OUTCOMES SUMMARY:\n");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *src = (const char *)sqlite3_column_text(stmt, 0);
        int cnt = (int)sqlite3_column_int64(stmt, 1);
        double avg_acc = sqlite3_column_double(stmt, 2);
        int yes_cnt = (int)sqlite3_column_int64(stmt, 3);
        printf("    %s: %d resolved, avg accuracy=%.3f, YES=%d\n", src, cnt, avg_acc, yes_cnt);
    }
    sqlite3_finalize(stmt);

    sqlite3_stmt *st2;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*), AVG(accuracy) FROM outcomes", -1, &st2, NULL) == SQLITE_OK) {
        if (sqlite3_step(st2) == SQLITE_ROW) {
            int total = (int)sqlite3_column_int64(st2, 0);
            double avg = sqlite3_column_double(st2, 1);
            printf("    TOTAL: %d outcomes, avg accuracy=%.3f\n", total, avg);
        }
        sqlite3_finalize(st2);
    }
}

int main(void) {
    printf("╔══════════════════════════════════╗\n");
    printf("║   OUTCOME SCANNER v1            ║\n");
    printf("║   Prediction market outcomes     ║\n");
    printf("╚══════════════════════════════════╝\n\n");

    curl_global_init(CURL_GLOBAL_DEFAULT);
    db_init();
    if (!g_db) { fprintf(stderr, "Cannot open %s\n", OUTCOMES_DB); return 1; }

    int total = 0;
    total += scan_polymarket();
    total += scan_kalshi();

    print_summary();
    db_close();
    curl_global_cleanup();

    printf("\n  Done. %d total outcomes resolved.\n", total);
    return total > 0 ? 0 : 1;
}
