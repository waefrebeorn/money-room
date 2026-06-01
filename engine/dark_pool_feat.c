/*
 * dark_pool_feat.c — P63: Dark Pool / ATS Volume Feature Collector
 *
 * Fetches weekly ATS (Alternative Trading System / dark pool) volume data
 * from FINRA OTC Transparency API. Tracks dark pool share of total market
 * volume for major ETFs and indices.
 *
 * Data source: api.finra.org/data/group/otcMarket/name/weeklySummary
 * Free, no API key, delayed ~2-4 weeks (data as of mid-2023 in old API)
 *
 * Features collected per ticker:
 *   - dark_pool_volume: total shares traded in ATS that week
 *   - dark_pool_notional: total dollar value traded in ATS
 *   - dark_pool_trades: number of ATS trades
 *   - Aggregate: total market weekly volume for ratio computation
 *
 * Commands:
 *   fetch <ticker>       — Fetch latest data for a ticker, store in SQLite
 *   fetch-all            — Fetch for all tracked symbols
 *   latest <ticker>      — Show latest stored entry
 *   trend <ticker> [N]   — Show last N weekly snapshots
 *   db <ticker> [N]      — Show DB summary
 *
 * Build:
 *   gcc dark_pool_feat.c -o dark_pool_feat -lcurl -ljansson -lsqlite3 -lm -O2
 *
 * Cron (no_agent, every 1h):
 *   ./dark_pool_feat fetch SPY && ./dark_pool_feat fetch QQQ && ./dark_pool_feat fetch IWM
 *
 * Output: ~/.hermes/dark_pool_cache/SPY_darkpool.db
 */

#define _POSIX_CACHE_DIR "/home/wubu2/.hermes/dark_pool_cache"
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>

// ── Configuration ──
#define DB_DIR      "/home/wubu2/.hermes/dark_pool_cache"
#define CURL_TIMEOUT 30L
#define API_URL     "https://api.finra.org/data/group/otcMarket/name/weeklySummary"

// Tracked symbols for dark pool monitoring
static const char *TRACKED_SYMBOLS[] = {
    "SPY", "QQQ", "IWM", "DIA", "TLT", "XLF", "XLE", "GLD", "SLV", "USO",
    "AAPL", "MSFT", "AMZN", "GOOGL", "META", "NVDA", "TSLA", "JPM", "V", "JNJ"
};
#define N_SYMBOLS 20

// ── HTTP response buffer ──
typedef struct { char *data; size_t len; } HttpBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    HttpBuf *b = (HttpBuf*)user;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    memcpy(p + b->len, ptr, n);
    b->data = p;
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

// ── DB helpers ──
static sqlite3 *open_db(const char *ticker) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_darkpool.db", DB_DIR, ticker);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "DB error: %s\n", sqlite3_errmsg(db));
        return NULL;
    }
    // WAL mode for concurrent reads
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    // Create tables if not exist
    const char *sql =
        "CREATE TABLE IF NOT EXISTS snapshots ("
        "  week_start_date TEXT PRIMARY KEY,"
        "  last_update_date TEXT,"
        "  total_shares INTEGER,"
        "  total_notional REAL,"
        "  total_trades INTEGER,"
        "  tier TEXT,"
        "  product_type TEXT,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS aggregate ("
        "  week_start_date TEXT PRIMARY KEY,"
        "  total_market_shares INTEGER,"
        "  total_market_notional REAL,"
        "  total_market_trades INTEGER,"
        "  total_ats_shares INTEGER,"
        "  total_ats_notional REAL,"
        "  total_ats_trades INTEGER,"
        "  ats_share_pct REAL,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Create table error: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

// ── Fetch FINRA API ──
static HttpBuf *finra_post(const char *json_body) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    HttpBuf *buf = calloc(1, sizeof(HttpBuf));
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, API_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dark-pool-feat/1.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "HTTP error: %s\n", curl_easy_strerror(res));
        free(buf->data);
        free(buf);
        buf = NULL;
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return buf;
}

// ── Fetch per-symbol ATS data ──
static int fetch_symbol_ats(sqlite3 *db, const char *ticker) {
    char body[4096];
    snprintf(body, sizeof(body),
        "{"
        "\"compareFilters\":["
        "{\"compareType\":\"EQUAL\",\"fieldName\":\"summaryTypeCode\",\"fieldValue\":\"ATS_W_SMBL\"},"
        "{\"compareType\":\"EQUAL\",\"fieldName\":\"issueSymbolIdentifier\",\"fieldValue\":\"%s\"}"
        "],"
        "\"limit\":10"
        "}", ticker);

    HttpBuf *buf = finra_post(body);
    if (!buf) return -1;

    json_error_t err;
    json_t *root = json_loads(buf->data, 0, &err);
    if (!root || !json_is_array(root)) {
        fprintf(stderr, "JSON parse error for %s: %s\n", ticker, err.text);
        free(buf->data);
        free(buf);
        return -1;
    }

    size_t n = json_array_size(root);
    printf("[%s] %zu records received\n", ticker, n);

    int count = 0;
    for (size_t i = 0; i < n; i++) {
        json_t *rec = json_array_get(root, i);
        if (!rec) continue;

        json_t *j_week = json_object_get(rec, "weekStartDate");
        json_t *j_update = json_object_get(rec, "lastUpdateDate");
        json_t *j_shares = json_object_get(rec, "totalWeeklyShareQuantity");
        json_t *j_notional = json_object_get(rec, "totalNotionalSum");
        json_t *j_trades = json_object_get(rec, "totalWeeklyTradeCount");
        json_t *j_tier = json_object_get(rec, "tierDescription");
        json_t *j_product = json_object_get(rec, "productTypeCode");

        if (!j_week || !j_shares || !j_notional) continue;

        const char *week = json_string_value(j_week);
        const char *update_date = j_update ? json_string_value(j_update) : "";
        const char *tier = j_tier ? json_string_value(j_tier) : "";
        const char *prod = j_product ? json_string_value(j_product) : "";

        // UPSERT
        sqlite3_stmt *stmt;
        const char *sql =
            "INSERT OR REPLACE INTO snapshots "
            "(week_start_date, last_update_date, total_shares, total_notional, total_trades, tier, product_type) "
            "VALUES (?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "Prepare error: %s\n", sqlite3_errmsg(db));
            continue;
        }
        sqlite3_bind_text(stmt, 1, week, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, update_date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)json_integer_value(j_shares));
        sqlite3_bind_double(stmt, 4, json_number_value(j_notional));
        sqlite3_bind_int(stmt, 5, (int)json_integer_value(j_trades));
        sqlite3_bind_text(stmt, 6, tier, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, prod, -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            fprintf(stderr, "Insert error: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
        count++;
    }

    json_decref(root);
    free(buf->data);
    free(buf);
    printf("[%s] %d records stored\n", ticker, count);
    return count;
}

// ── Fetch aggregate ATS volume stats ──
static int fetch_aggregate_ats(sqlite3 *db) {
    const char *body =
        "{\"compareFilters\":["
        "{\"compareType\":\"EQUAL\",\"fieldName\":\"summaryTypeCode\",\"fieldValue\":\"ATS_W_VOL_STATS\"}"
        "],\"limit\":20}";

    HttpBuf *buf = finra_post(body);
    if (!buf) return -1;

    json_error_t err;
    json_t *root = json_loads(buf->data, 0, &err);
    if (!root || !json_is_array(root)) {
        fprintf(stderr, "JSON parse error for aggregate: %s\n", err.text);
        free(buf->data);
        free(buf);
        return -1;
    }

    size_t n = json_array_size(root);
    printf("[AGGREGATE] %zu records received\n", n);

    int count = 0;
    for (size_t i = 0; i < n; i++) {
        json_t *rec = json_array_get(root, i);
        if (!rec) continue;
        json_t *j_week = json_object_get(rec, "weekStartDate");
        json_t *j_shares = json_object_get(rec, "totalWeeklyShareQuantity");
        json_t *j_notional = json_object_get(rec, "totalNotionalSum");
        json_t *j_trades = json_object_get(rec, "totalWeeklyTradeCount");
        if (!j_week || !j_shares) continue;

        const char *week = json_string_value(j_week);
        sqlite3_int64 shares = (sqlite3_int64)json_integer_value(j_shares);
        double notional = json_number_value(j_notional);
        int trades = (int)json_integer_value(j_trades);

        // For aggregate, we don't have total market volume directly.
        // totalWeeklyShareQuantity IS the total ATS volume aggregated.
        // Market total would need separate data source.
        // Store what we have — ats_share_pct computed later.

        sqlite3_stmt *stmt;
        const char *sql =
            "INSERT OR REPLACE INTO aggregate "
            "(week_start_date, total_ats_shares, total_ats_notional, total_ats_trades, ats_share_pct) "
            "VALUES (?, ?, ?, ?, NULL);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "Prepare error: %s\n", sqlite3_errmsg(db));
            continue;
        }
        sqlite3_bind_text(stmt, 1, week, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, shares);
        sqlite3_bind_double(stmt, 3, notional);
        sqlite3_bind_int(stmt, 4, trades);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            fprintf(stderr, "Insert error: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
        count++;
    }

    json_decref(root);
    free(buf->data);
    free(buf);
    printf("[AGGREGATE] %d records stored\n", count);
    return count;
}

// ── Output JSON for feed_bridge ──
static int write_json_output(const char *ticker) {
    char db_path[512], out_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s_darkpool.db", DB_DIR, ticker);
    snprintf(out_path, sizeof(out_path), "%s/%s_darkpool_feat.json", DB_DIR, ticker);

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "DB error: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    // Get latest snapshot
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT week_start_date, total_shares, total_notional, total_trades, "
        "       tier, product_type "
        "FROM snapshots ORDER BY week_start_date DESC LIMIT 5;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Query error: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    json_t *snapshots = json_array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json_t *s = json_object();
        json_object_set_new(s, "week", json_string((const char*)sqlite3_column_text(stmt, 0)));
        json_object_set_new(s, "shares", json_integer(sqlite3_column_int64(stmt, 1)));
        json_object_set_new(s, "notional", json_real(sqlite3_column_double(stmt, 2)));
        json_object_set_new(s, "trades", json_integer(sqlite3_column_int(stmt, 3)));
        json_object_set_new(s, "tier", json_string((const char*)sqlite3_column_text(stmt, 4)));
        json_object_set_new(s, "product", json_string((const char*)sqlite3_column_text(stmt, 5)));
        json_array_append_new(snapshots, s);
    }
    sqlite3_finalize(stmt);

    // Get latest aggregate
    const char *agg_sql =
        "SELECT week_start_date, total_ats_shares, total_ats_notional, "
        "       total_ats_trades, ats_share_pct "
        "FROM aggregate ORDER BY week_start_date DESC LIMIT 5;";
    json_t *aggregate = json_array();
    if (sqlite3_prepare_v2(db, agg_sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json_t *a = json_object();
            json_object_set_new(a, "week", json_string((const char*)sqlite3_column_text(stmt, 0)));
            json_object_set_new(a, "ats_shares", json_integer(sqlite3_column_int64(stmt, 1)));
            json_object_set_new(a, "ats_notional", json_real(sqlite3_column_double(stmt, 2)));
            json_object_set_new(a, "ats_trades", json_integer(sqlite3_column_int(stmt, 3)));
            if (sqlite3_column_type(stmt, 4) == SQLITE_FLOAT)
                json_object_set_new(a, "share_pct", json_real(sqlite3_column_double(stmt, 4)));
            else
                json_object_set_new(a, "share_pct", json_null());
            json_array_append_new(aggregate, a);
        }
        sqlite3_finalize(stmt);
    }

    // Build final JSON
    json_t *root = json_object();
    json_object_set_new(root, "ticker", json_string(ticker));
    json_object_set_new(root, "fetch_time", json_integer((long long)time(NULL)));
    json_object_set_new(root, "snapshots", snapshots);
    json_object_set_new(root, "aggregate_ats", aggregate);

    // Compute features for latest snapshot
    if (json_array_size(snapshots) > 0) {
        json_t *latest = json_array_get(snapshots, 0);
        json_t *prev = json_array_size(snapshots) > 1 ? json_array_get(snapshots, 1) : NULL;
        json_t *features = json_object();

        double shares_now = json_number_value(json_object_get(latest, "shares"));
        double notional_now = json_number_value(json_object_get(latest, "notional"));
        double trades_now = json_number_value(json_object_get(latest, "trades"));

        json_object_set_new(features, "latest_shares", json_real(shares_now));
        json_object_set_new(features, "latest_notional", json_real(notional_now));
        json_object_set_new(features, "latest_trades", json_real(trades_now));

        // Week-over-week change
        if (prev) {
            double shares_prev = json_number_value(json_object_get(prev, "shares"));
            double notional_prev = json_number_value(json_object_get(prev, "notional"));
            if (shares_prev > 0) {
                json_object_set_new(features, "wow_change_pct", json_real((shares_now - shares_prev) / shares_prev * 100.0));
            }
            if (notional_prev > 0) {
                json_object_set_new(features, "notional_wow_pct", json_real((notional_now - notional_prev) / notional_prev * 100.0));
            }
        } else {
            json_object_set_new(features, "wow_change_pct", json_null());
            json_object_set_new(features, "notional_wow_pct", json_null());
        }

        // 4-week trend
        if (json_array_size(snapshots) >= 4) {
            json_t *w1 = json_array_get(snapshots, 0);
            json_t *w4 = json_array_get(snapshots, 3);
            double shares_1 = json_number_value(json_object_get(w1, "shares"));
            double shares_4 = json_number_value(json_object_get(w4, "shares"));
            if (shares_4 > 0) {
                json_object_set_new(features, "month_trend_pct", json_real((shares_1 - shares_4) / shares_4 * 100.0));
            }
        }

        // Normalized features [0,1] range
        // Dark pool ratio: shares / 200M (SPY typical weekly volume ~80M, max ~200M)
        double dp_ratio = fmin(shares_now / 200000000.0, 1.0);
        json_object_set_new(features, "dark_pool_ratio_norm", json_real(dp_ratio));

        // Dark pool volume signal: WoW change normalized [-10%, +10%] → [0,1]
        if (json_object_get(features, "wow_change_pct") != NULL && !json_is_null(json_object_get(features, "wow_change_pct"))) {
            double wow = json_number_value(json_object_get(features, "wow_change_pct"));
            double wow_signal = fmin(fmax(wow / 20.0 + 0.5, 0.0), 1.0);
            json_object_set_new(features, "wow_signal_norm", json_real(wow_signal));
        }

        json_object_set_new(root, "features", features);
    }

    // Write to file
    char *out = json_dumps(root, JSON_INDENT(2));
    FILE *f = fopen(out_path, "w");
    if (f) {
        fprintf(f, "%s\n", out);
        fclose(f);
        printf("[%s] Written to %s\n", ticker, out_path);
    } else {
        fprintf(stderr, "Cannot write %s\n", out_path);
    }
    free(out);
    json_decref(root);
    sqlite3_close(db);
    return 0;
}

// ── Commands ──
static void cmd_fetch(const char *ticker) {
    sqlite3 *db = open_db(ticker);
    if (!db) return;
    fetch_symbol_ats(db, ticker);
    // Also write aggregate stats (only when first symbol is fetched to avoid duplicates)
    if (strcmp(ticker, TRACKED_SYMBOLS[0]) == 0) {
        fetch_aggregate_ats(db);
    }
    sqlite3_close(db);
    write_json_output(ticker);
}

static void cmd_fetch_all(void) {
    // First pass: fetch aggregate once
    sqlite3 *first_db = open_db(TRACKED_SYMBOLS[0]);
    if (first_db) {
        fetch_aggregate_ats(first_db);
        sqlite3_close(first_db);
    }
    // Then fetch each symbol
    for (int i = 0; i < N_SYMBOLS; i++) {
        sqlite3 *db = open_db(TRACKED_SYMBOLS[i]);
        if (!db) continue;
        printf("--- %s (%d/%d) ---\n", TRACKED_SYMBOLS[i], i+1, N_SYMBOLS);
        fetch_symbol_ats(db, TRACKED_SYMBOLS[i]);
        sqlite3_close(db);
        write_json_output(TRACKED_SYMBOLS[i]);
    }
}

static void cmd_latest(const char *ticker) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_darkpool_feat.json", DB_DIR, ticker);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("No data for %s (run 'fetch %s' first)\n", ticker, ticker);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    printf("%s\n", buf);
    free(buf);
}

static void cmd_db(const char *ticker, int n) {
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s_darkpool.db", DB_DIR, ticker);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        printf("No DB for %s\n", ticker);
        return;
    }
    sqlite3_stmt *stmt;
    const char *sql = "SELECT week_start_date, total_shares, total_notional, total_trades "
                      "FROM snapshots ORDER BY week_start_date DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Query error\n");
        sqlite3_close(db);
        return;
    }
    sqlite3_bind_int(stmt, 1, n);
    printf("\n=== %s Dark Pool Weekly Data (last %d) ===\n", ticker, n);
    printf("%-14s %12s %18s %10s\n", "Week", "Shares", "Notional ($)", "Trades");
    printf("%-14s %12s %18s %10s\n", "------", "------", "-------------", "------");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%-14s %12lld %'18.0f %10d\n",
               sqlite3_column_text(stmt, 0),
               (long long)sqlite3_column_int64(stmt, 1),
               sqlite3_column_double(stmt, 2),
               sqlite3_column_int(stmt, 3));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s fetch <TICKER>     — Fetch and store data for a symbol\n", prog);
    printf("  %s fetch-all          — Fetch data for all %d tracked symbols\n", prog, N_SYMBOLS);
    printf("  %s latest <TICKER>    — Show latest JSON output\n", prog);
    printf("  %s db <TICKER> [N]    — Show last N weekly records (default: 5)\n", prog);
    printf("\nTracked symbols: ");
    for (int i = 0; i < N_SYMBOLS; i++) printf("%s ", TRACKED_SYMBOLS[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    // Ensure cache directory exists
    char mkdir_cmd[256];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", DB_DIR);
    system(mkdir_cmd);

    if (strcmp(argv[1], "fetch") == 0 && argc >= 3) {
        cmd_fetch(argv[2]);
    } else if (strcmp(argv[1], "fetch-all") == 0) {
        cmd_fetch_all();
    } else if (strcmp(argv[1], "latest") == 0 && argc >= 3) {
        cmd_latest(argv[2]);
    } else if (strcmp(argv[1], "db") == 0 && argc >= 3) {
        int n = (argc >= 4) ? atoi(argv[3]) : 5;
        if (n < 1) n = 1;
        cmd_db(argv[2], n);
    } else {
        print_usage(argv[0]);
        return 1;
    }
    return 0;
}
