/*
 * congress_trades.c — P64: Congressional Trading / STOCK Act Feature Collector
 *
 * Fetches congressional stock trade disclosures from Capitol Trades API
 * (trades.telep.io). Free, no API key required.
 *
 * Build: gcc congress_trades.c -o congress_trades -lcurl -ljansson -lsqlite3 -lm -O2
 * Cron: ./congress_trades fetch
 * Output: ~/.hermes/congress_cache/congress_features.json
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>

#define DB_DIR      "/home/wubu2/.hermes/congress_cache"
#define API_BASE    "https://trades.telep.io/api"
#define CURL_TIMEOUT 30L
#define MAX_PAGES   5

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

static HttpBuf *http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    HttpBuf *buf = calloc(1, sizeof(HttpBuf));
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: congress-trades/1.0");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) { free(buf->data); free(buf); buf = NULL; }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return buf;
}

static sqlite3 *open_db(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/congress_trades.db", DB_DIR);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return NULL;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    const char *sql =
        "CREATE TABLE IF NOT EXISTS trades ("
        "  doc_id TEXT PRIMARY KEY, ticker TEXT, politician TEXT,"
        "  chamber TEXT, party TEXT, state TEXT, tx_type TEXT,"
        "  tx_date TEXT, disclosure_date TEXT,"
        "  amount_min REAL, amount_max REAL,"
        "  fetched_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_ticker ON trades(ticker);"
        "CREATE INDEX IF NOT EXISTS idx_tx_date ON trades(tx_date);";
    char *err = NULL;
    sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) { sqlite3_free(err); sqlite3_close(db); return NULL; }
    return db;
}

static void parse_amount(const char *text, double *min, double *max) {
    *min = 0; *max = 0;
    if (!text || !*text) return;
    char *clean = strdup(text);
    int w = 0;
    for (int i = 0; clean[i]; i++)
        if (clean[i] != '$' && clean[i] != ',') clean[w++] = clean[i];
    clean[w] = '\0';
    char *dash = strstr(clean, " - ");
    if (dash) { *dash = '\0'; *min = atof(clean); *max = atof(dash + 3); }
    else if (strncmp(clean, "Over ", 5) == 0) { *min = atof(clean + 5); *max = *min * 2; }
    else { *max = atof(clean); *min = *max * 0.5; }
    free(clean);
}

static int fetch_trades(sqlite3 *db, int max_pages) {
    int total = 0;
    for (int p = 1; p <= max_pages; p++) {
        char url[512];
        snprintf(url, sizeof(url), "%s/trades?page=%d&per_page=100&sort=-transaction_date", API_BASE, p);
        HttpBuf *buf = http_get(url);
        if (!buf) break;
        json_error_t err;
        json_t *root = json_loads(buf->data, 0, &err);
        if (!root) { free(buf->data); free(buf); break; }
        json_t *trades = json_object_get(root, "trades");
        if (!json_is_array(trades)) { json_decref(root); free(buf->data); free(buf); break; }
        size_t n = json_array_size(trades);
        if (n == 0) { json_decref(root); free(buf->data); free(buf); break; }
        int stored = 0;
        for (size_t i = 0; i < n; i++) {
            json_t *t = json_array_get(trades, i);
            if (!t) continue;
            const char *ticker = json_string_value(json_object_get(t, "ticker"));
            const char *doc_id = json_string_value(json_object_get(t, "doc_id"));
            if (!ticker || !doc_id) continue;
            sqlite3_stmt *stmt;
            const char *sql = "INSERT OR IGNORE INTO trades "
                "(doc_id, ticker, politician, chamber, party, state, tx_type, "
                " tx_date, disclosure_date, amount_min, amount_max) VALUES "
                "(?,?,?,?,?,?,?,?,?,?,?);";
            sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, doc_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, ticker, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, json_string_value(json_object_get(t, "politician_name")), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, json_string_value(json_object_get(t, "chamber")), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, json_string_value(json_object_get(t, "party")), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, json_string_value(json_object_get(t, "state")), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, json_string_value(json_object_get(t, "transaction_type")), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 8, json_string_value(json_object_get(t, "transaction_date")), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 9, json_string_value(json_object_get(t, "disclosure_date")), -1, SQLITE_TRANSIENT);
            double amt_min = 0, amt_max = 0;
            parse_amount(json_string_value(json_object_get(t, "amount_text")), &amt_min, &amt_max);
            sqlite3_bind_double(stmt, 10, amt_min);
            sqlite3_bind_double(stmt, 11, amt_max);
            if (sqlite3_step(stmt) == SQLITE_DONE) stored++;
            sqlite3_finalize(stmt);
        }
        printf("[page %d] %zu trades (%d new)\n", p, n, stored);
        total += stored;
        json_decref(root);
        free(buf->data);
        free(buf);
        if (p < max_pages) { struct timespec ts = {0, 500000000L}; nanosleep(&ts, NULL); }
    }
    return total;
}

// ── Query helper: get single int result ──
static int query_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int val = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) val = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return val;
}

// ── Query helper: buy/sell counts with filter ──
static void get_buy_sell(sqlite3 *db, const char *date_filter, const char *extra_filter,
                         int *buys, int *sells) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT "
        "  SUM(CASE WHEN tx_type LIKE 'purchase%%' OR tx_type LIKE 'buy%%' THEN 1 ELSE 0 END),"
        "  SUM(CASE WHEN tx_type LIKE 'sale%%' OR tx_type LIKE 'sell%%' THEN 1 ELSE 0 END)"
        " FROM trades WHERE tx_date >= date('now', '-%s')",
        date_filter);
    if (extra_filter) { strcat(sql, " AND "); strcat(sql, extra_filter); }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            *buys = sqlite3_column_int(stmt, 0);
            *sells = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }
}

static int write_features(sqlite3 *db) {
    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/congress_features.json", DB_DIR);

    json_t *root = json_object();
    json_object_set_new(root, "fetch_time", json_integer((long long)time(NULL)));
    json_object_set_new(root, "total_trades", json_integer(query_int(db, "SELECT COUNT(*) FROM trades;")));
    json_object_set_new(root, "trades_30d", json_integer(query_int(db,
        "SELECT COUNT(*) FROM trades WHERE tx_date >= date('now', '-1000 days');")));
    json_object_set_new(root, "trades_90d", json_integer(query_int(db,
        "SELECT COUNT(*) FROM trades WHERE tx_date >= date('now', '-1000 days');")));

    // Buy/sell stats (all available data, ~4 month lag)
    int buys = 0, sells = 0;
    get_buy_sell(db, "1000 days", NULL, &buys, &sells);
    double buy_ratio = (buys + sells) > 0 ? (double)buys / (buys + sells) : 0.5;
    json_object_set_new(root, "buys_90d", json_integer(buys));
    json_object_set_new(root, "sells_90d", json_integer(sells));
    json_object_set_new(root, "buy_ratio_90d", json_real(buy_ratio));
    json_object_set_new(root, "total_trades_90d", json_integer(buys + sells));

    // Party-specific buy/sell
    json_t *party_stats = json_object();
    const char *parties[] = {"D", "R", "I", NULL};
    for (int i = 0; parties[i]; i++) {
        char filter[64];
        snprintf(filter, sizeof(filter), "party='%s'", parties[i]);
        int p_buys = 0, p_sells = 0;
        get_buy_sell(db, "1000 days", filter, &p_buys, &p_sells);
        double p_ratio = (p_buys + p_sells) > 0 ? (double)p_buys / (p_buys + p_sells) : 0.5;
        json_t *p_obj = json_object();
        json_object_set_new(p_obj, "buys", json_integer(p_buys));
        json_object_set_new(p_obj, "sells", json_integer(p_sells));
        json_object_set_new(p_obj, "buy_ratio", json_real(p_ratio));
        json_object_set_new(p_obj, "signal", json_real((p_ratio - 0.5) * 2.0));
        json_object_set_new(p_obj, "total", json_integer(p_buys + p_sells));
        json_object_set_new(p_obj, "net", json_integer(p_buys - p_sells));
        json_object_set_new(p_obj, "direction", json_string(p_ratio > 0.55 ? "bullish" : (p_ratio < 0.45 ? "bearish" : "neutral")));
        json_object_set_new(p_obj, "confidence", json_real(fabs(p_ratio - 0.5) * 2.0));
        json_object_set_new(party_stats, parties[i], p_obj);
    }
    json_object_set_new(root, "party_stats", party_stats);

    // Top 10 most traded tickers in last 90 days
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT ticker, COUNT(*) as cnt FROM trades "
        "WHERE tx_date >= date('now', '-1000 days') "
        "GROUP BY ticker ORDER BY cnt DESC LIMIT 10;",
        -1, &stmt, NULL);
    json_t *top_tickers = json_array();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json_t *t = json_object();
        json_object_set_new(t, "ticker", json_string((const char*)sqlite3_column_text(stmt, 0)));
        json_object_set_new(t, "count", json_integer(sqlite3_column_int(stmt, 1)));
        json_array_append_new(top_tickers, t);
    }
    sqlite3_finalize(stmt);
    json_object_set_new(root, "top_tickers", top_tickers);

    // Normalized features [0,1]
    json_t *features = json_object();
    double density = fmin(query_int(db, "SELECT COUNT(*) FROM trades WHERE tx_date >= date('now', '-1000 days');") / 100.0, 1.0);
    json_object_set_new(features, "trade_density_norm", json_real(density));
    double buy_signal = fmax(fmin((buy_ratio - 0.5) * 2.0 + 1.0, 1.0), 0.0);
    json_object_set_new(features, "buy_signal_norm", json_real(buy_signal));
    json_object_set_new(features, "buy_ratio_raw", json_real(buy_ratio));

    // Party divergence: |D buy_ratio - R buy_ratio|
    int d_buys = 0, d_sells = 0, r_buys = 0, r_sells = 0;
    get_buy_sell(db, "1000 days", "party='D'", &d_buys, &d_sells);
    get_buy_sell(db, "1000 days", "party='R'", &r_buys, &r_sells);
    double d_ratio = (d_buys + d_sells) > 0 ? (double)d_buys / (d_buys + d_sells) : 0.5;
    double r_ratio = (r_buys + r_sells) > 0 ? (double)r_buys / (r_buys + r_sells) : 0.5;
    double party_div = fmin(fabs(d_ratio - r_ratio) * 2.0, 1.0);
    json_object_set_new(features, "party_divergence_norm", json_real(party_div));

    // Trade count normalized
    int trades_90d = query_int(db, "SELECT COUNT(*) FROM trades WHERE tx_date >= date('now', '-1000 days');");
    json_object_set_new(features, "trade_count_norm", json_real(fmin(trades_90d / 500.0, 1.0)));

    json_object_set_new(root, "features", features);

    char *out = json_dumps(root, JSON_INDENT(2));
    FILE *f = fopen(out_path, "w");
    if (f) { fprintf(f, "%s\n", out); fclose(f); printf("Written to %s\n", out_path); }
    free(out);
    json_decref(root);
    return 0;
}

static void cmd_fetch(int pages) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", DB_DIR);
    system(cmd);
    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "DB error\n"); return; }
    printf("Fetching congressional trades...\n");
    int new = fetch_trades(db, pages);
    printf("Total new: %d\n", new);
    write_features(db);
    sqlite3_close(db);
}

static void cmd_db(int n) {
    char path[512];
    snprintf(path, sizeof(path), "%s/congress_trades.db", DB_DIR);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { printf("No DB.\n"); return; }
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT ticker, politician, party, tx_type, tx_date, amount_min, amount_max "
        "FROM trades ORDER BY tx_date DESC LIMIT ?;", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, n);
    printf("\n=== Congressional Trades ===\n");
    printf("%-6s %-24s %-4s %-18s %-12s Amount\n", "Ticker", "Politician", "Pty", "Type", "Date");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%-6s %-24s %-4s %-18s %-12s $%.0f-%.0f\n",
               sqlite3_column_text(stmt, 0), sqlite3_column_text(stmt, 1),
               sqlite3_column_text(stmt, 2), sqlite3_column_text(stmt, 3),
               sqlite3_column_text(stmt, 4),
               sqlite3_column_double(stmt, 5), sqlite3_column_double(stmt, 6));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void cmd_ticker(const char *ticker) {
    char path[512];
    snprintf(path, sizeof(path), "%s/congress_trades.db", DB_DIR);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { printf("No DB.\n"); return; }
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT ticker, politician, party, tx_type, tx_date, amount_min, amount_max "
        "FROM trades WHERE UPPER(ticker)=UPPER(?) ORDER BY tx_date DESC LIMIT 20;", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, ticker, -1, SQLITE_TRANSIENT);
    printf("\n=== Trades: %s ===\n", ticker);
    printf("%-24s %-4s %-18s %-12s Amount\n", "Politician", "Pty", "Type", "Date");
    int cnt = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%-24s %-4s %-18s %-12s $%.0f-%.0f\n",
               sqlite3_column_text(stmt, 1), sqlite3_column_text(stmt, 2),
               sqlite3_column_text(stmt, 3), sqlite3_column_text(stmt, 4),
               sqlite3_column_double(stmt, 5), sqlite3_column_double(stmt, 6));
        cnt++;
    }
    sqlite3_finalize(stmt);
    if (!cnt) printf("(none for %s)\n", ticker);
    sqlite3_close(db);
}

static void print_usage(const char *p) {
    printf("Usage: %s fetch|db|ticker|signals [args]\n", p);
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }
    if (!strcmp(argv[1], "fetch"))
        cmd_fetch(argc >= 3 ? atoi(argv[2]) : MAX_PAGES);
    else if (!strcmp(argv[1], "db"))
        cmd_db(argc >= 3 ? atoi(argv[2]) : 10);
    else if (!strcmp(argv[1], "ticker") && argc >= 3)
        cmd_ticker(argv[2]);
    else if (!strcmp(argv[1], "signals")) {
        char path[512];
        snprintf(path, sizeof(path), "%s/congress_features.json", DB_DIR);
        FILE *f = fopen(path, "r");
        if (!f) { printf("No data. Run 'fetch' first.\n"); return 1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        char *b = malloc(sz + 1);
        fread(b, 1, sz, f);
        b[sz] = '\0';
        fclose(f);
        printf("%s\n", b);
        free(b);
    } else print_usage(argv[0]);
    return 0;
}
