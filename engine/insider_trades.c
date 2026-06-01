/*
 * insider_trades.c — P65: Insider Trading / SEC Form 4 Feature Collector
 *
 * Fetches SEC Form 4 insider trading filing metadata from SEC EDGAR
 * full-text search API. Tracks filing volume trends.
 *
 * The SEC API is free, no API key required.
 *
 * Build: gcc insider_trades.c -o insider_trades -lcurl -ljansson -lsqlite3 -lm -O2
 * Cron: ./insider_trades fetch
 * Output: ~/.hermes/insider_cache/insider_features.json
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

#define DB_DIR      "/home/wubu2/.hermes/insider_cache"
#define API_BASE    "https://efts.sec.gov/LATEST/search-index"
#define CURL_TIMEOUT 30L

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
    headers = curl_slist_append(headers, "User-Agent: MoneyRoom/1.0 (research)");
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

// ── Fetch Form 4 filings for a date range ──
static int fetch_form4(const char *start_date, const char *end_date, int page,
                       json_t *out_array) {
    char url[1024];
    snprintf(url, sizeof(url),
        "%s?q=%%22form+4%%22&dateRange=custom&startdt=%s&enddt=%s&page=%d&counts=1",
        API_BASE, start_date, end_date, page);

    HttpBuf *buf = http_get(url);
    if (!buf) return -1;

    json_error_t err;
    json_t *root = json_loads(buf->data, 0, &err);
    if (!root) { free(buf->data); free(buf); return -1; }

    json_t *hits = json_object_get(root, "hits");
    json_t *hits_arr = hits ? json_object_get(hits, "hits") : NULL;
    json_t *total = hits ? json_object_get(hits, "total") : NULL;

    int total_count = 0;
    if (total) {
        json_t *val = json_object_get(total, "value");
        if (val) total_count = (int)json_number_value(val);
    }

    int count = 0;
    if (hits_arr && json_is_array(hits_arr)) {
        size_t n = json_array_size(hits_arr);
        for (size_t i = 0; i < n; i++) {
            json_t *hit = json_array_get(hits_arr, i);
            json_t *source = json_object_get(hit, "_source");
            if (!source) continue;

            json_t *display = json_object_get(source, "display_names");
            json_t *ciks = json_object_get(source, "ciks");
            json_t *file_date = json_object_get(source, "file_date");
            json_t *period = json_object_get(source, "period_ending");
            json_t *file_num = json_object_get(source, "file_num");

            // Extract filing detail URL from _id
            const char *id = json_string_value(json_object_get(hit, "_id"));
            if (!id) continue;

            // Build a record
            json_t *record = json_object();
            json_object_set_new(record, "id", json_string(id));
            if (file_date) json_object_set_new(record, "file_date", json_string(json_string_value(file_date)));
            if (period) json_object_set_new(record, "period_ending", json_string(json_string_value(period)));

            // Extract ticker from display_names
            if (display && json_is_array(display) && json_array_size(display) > 1) {
                const char *company = json_string_value(json_array_get(display, 0));
                if (company) {
                    // Format: "COMPANY NAME (CIK ##########)"
                    // Extract ticker not directly available — store company name
                    json_object_set_new(record, "company", json_string(company));
                }
            }

            // Extract CIKs
            if (ciks && json_is_array(ciks)) {
                json_t *ciks_arr = json_array();
                size_t cn = json_array_size(ciks);
                for (size_t j = 0; j < cn; j++) {
                    json_array_append_new(ciks_arr, json_string(json_string_value(json_array_get(ciks, j))));
                }
                json_object_set_new(record, "ciks", ciks_arr);
            }

            // Extract form root
            json_t *forms = json_object_get(source, "root_forms");
            if (forms && json_is_array(forms) && json_array_size(forms) > 0) {
                json_object_set_new(record, "form", json_string(json_string_value(json_array_get(forms, 0))));
            }

            json_array_append_new(out_array, record);
            count++;
        }
    }

    json_decref(root);
    free(buf->data);
    free(buf);
    return total_count > 0 ? total_count : count;
}

// ── Store filings in SQLite ──
static sqlite3 *open_db(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/insider_trades.db", DB_DIR);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return NULL;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    const char *sql =
        "CREATE TABLE IF NOT EXISTS filings ("
        "  doc_id TEXT PRIMARY KEY, file_date TEXT, period_ending TEXT,"
        "  company TEXT, form_type TEXT,"
        "  fetched_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS daily_stats ("
        "  date TEXT PRIMARY KEY, filing_count INTEGER,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");";
    char *err = NULL;
    sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) { sqlite3_free(err); sqlite3_close(db); return NULL; }
    return db;
}

static int fetch_and_store(sqlite3 *db, const char *start, const char *end) {
    json_t *all_filings = json_array();
    int total = fetch_form4(start, end, 1, all_filings);

    int stored = 0;
    size_t n = json_array_size(all_filings);
    for (size_t i = 0; i < n; i++) {
        json_t *rec = json_array_get(all_filings, i);
        const char *id = json_string_value(json_object_get(rec, "id"));
        const char *file_date = json_string_value(json_object_get(rec, "file_date"));
        const char *period = json_string_value(json_object_get(rec, "period_ending"));
        const char *company = json_string_value(json_object_get(rec, "company"));
        const char *form_type = json_string_value(json_object_get(rec, "form"));

        if (!id || !file_date) continue;

        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO filings (doc_id, file_date, period_ending, company, form_type) "
            "VALUES (?, ?, ?, ?, ?);", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, file_date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, period ? period : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, company ? company : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, form_type ? form_type : "", -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_DONE) stored++;
        sqlite3_finalize(stmt);
    }

    printf("Total in range: %d, stored: %d\n", total, stored);
    json_decref(all_filings);
    return stored;
}

// ── Compute features and write JSON ──
static int write_features(sqlite3 *db) {
    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/insider_features.json", DB_DIR);

    json_t *root = json_object();
    json_object_set_new(root, "fetch_time", json_integer((long long)time(NULL)));

    // Total filings in DB
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM filings;", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        json_object_set_new(root, "total_filings", json_integer(sqlite3_column_int(stmt, 0)));
    sqlite3_finalize(stmt);

    // Filings by month (last 3 months)
    sqlite3_prepare_v2(db,
        "SELECT SUBSTR(file_date,1,7) as month, COUNT(*) as cnt "
        "FROM filings WHERE file_date >= date('now', '-120 days') "
        "GROUP BY month ORDER BY month DESC;", -1, &stmt, NULL);
    json_t *monthly = json_object();
    int counts[3] = {0};
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < 3) {
        const char *m = (const char*)sqlite3_column_text(stmt, 0);
        int c = sqlite3_column_int(stmt, 1);
        json_object_set_new(monthly, m ? m : "unknown", json_integer(c));
        counts[idx++] = c;
    }
    sqlite3_finalize(stmt);
    json_object_set_new(root, "by_month", monthly);

    // Filing rate: avg per day in latest month
    int latest_month = counts[0];
    int prev_month = counts[1] > 0 ? counts[1] : counts[0];
    double daily_rate = latest_month / 30.0;

    // Normalized features [0,1]
    json_t *features = json_object();

    // Insider filing density: daily rate / 500 (max typical: ~500 Form 4/day)
    double density = fmin(daily_rate / 500.0, 1.0);
    json_object_set_new(features, "insider_density_norm", json_real(density));

    // Filing trend: MoM change signal
    double trend = prev_month > 0 ? (double)(latest_month - prev_month) / prev_month : 0;
    double trend_signal = fmax(fmin(trend / 0.5 + 0.5, 1.0), 0.0); // map [-0.5, +0.5] → [0,1]
    json_object_set_new(features, "insider_trend_norm", json_real(trend_signal));

    // Volume: filings count normalized
    json_object_set_new(features, "filing_volume_norm", json_real(fmin(latest_month / 10000.0, 1.0)));

    json_object_set_new(root, "features", features);
    json_object_set_new(root, "daily_rate", json_real(daily_rate));
    json_object_set_new(root, "latest_month_count", json_integer(latest_month));
    json_object_set_new(root, "prev_month_count", json_integer(prev_month));
    json_object_set_new(root, "mom_trend_pct", json_real(trend * 100.0));

    char *out = json_dumps(root, JSON_INDENT(2));
    FILE *f = fopen(out_path, "w");
    if (f) { fprintf(f, "%s\n", out); fclose(f); printf("Written to %s\n", out_path); }
    free(out);
    json_decref(root);
    return 0;
}

// ── Commands ──
static void cmd_fetch(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", DB_DIR);
    system(cmd);

    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "DB error\n"); return; }

    // Fetch last 3 months of Form 4 filings
    printf("Fetching Form 4 insider trading filings...\n");

    // Fetch month by month to stay within API limits
    const char *months[][2] = {
        {"2026-05-01", "2026-05-28"},
        {"2026-04-01", "2026-04-30"},
        {"2026-03-01", "2026-03-31"},
        {"2026-02-01", "2026-02-28"},
        {"2026-01-01", "2026-01-31"}
    };
    int n_months = sizeof(months) / sizeof(months[0]);
    int total = 0;
    for (int i = 0; i < n_months; i++) {
        printf("  Month %s to %s...\n", months[i][0], months[i][1]);
        int stored = fetch_and_store(db, months[i][0], months[i][1]);
        total += stored;
        struct timespec ts = {1, 0}; // 1s delay between months for rate limiting
        nanosleep(&ts, NULL);
    }

    printf("Total stored this run: %d\n", total);
    write_features(db);
    sqlite3_close(db);
}

static void cmd_db(int n) {
    char path[512];
    snprintf(path, sizeof(path), "%s/insider_trades.db", DB_DIR);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { printf("No DB.\n"); return; }
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT file_date, company, form_type, period_ending "
        "FROM filings ORDER BY file_date DESC LIMIT ?;", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, n);
    printf("\n=== Insider Form 4 Filings ===\n");
    printf("%-12s %-40s %-10s %s\n", "File Date", "Company", "Form", "Period");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%-12s %-40s %-10s %s\n",
               sqlite3_column_text(stmt, 0),
               sqlite3_column_text(stmt, 1),
               sqlite3_column_text(stmt, 2),
               sqlite3_column_text(stmt, 3));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s fetch|db [N]\n", argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "fetch")) cmd_fetch();
    else if (!strcmp(argv[1], "db")) cmd_db(argc >= 3 ? atoi(argv[2]) : 10);
    else printf("Unknown: %s\n", argv[1]);
    return 0;
}
