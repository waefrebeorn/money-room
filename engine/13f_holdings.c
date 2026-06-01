/*
 * 13f_holdings.c — P66: 13F Institutional Holdings Feature Collector
 *
 * Fetches 13F-HR filing metadata from SEC EDGAR full-text search API.
 * Tracks institutional filing volume: density (filings/day) and
 * trend (rising/falling filing rate).
 *
 * 13F filings are quarterly reports by institutional investment managers
 * with >$100M AUM. Filing volume correlates with institutional market
 * engagement.
 *
 * SEC EDGAR API is free, no API key required. Rate limit: ~10 req/sec.
 *
 * Build: gcc 13f_holdings.c -o 13f_holdings -lcurl -ljansson -lsqlite3 -lm -O2
 * Cron: ./13f_holdings fetch
 * Output: ~/.hermes/13f_cache/inst_features.json
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

#define DB_DIR      "/home/wubu2/.hermes/13f_cache"
#define OUTPUT_DIR  "/home/wubu2/.hermes/13f_cache"
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

// ── SQLite helpers ──
static sqlite3 *db_open(const char *db_path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "DB open failed: %s\n", sqlite3_errmsg(db));
        return NULL;
    }
    return db;
}

static int db_exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\nSQL: %s\n", err, sql);
        sqlite3_free(err);
    }
    return rc;
}

// ── Fetch 13F-HR filings for a date range ──
static int fetch_13f(const char *start_date, const char *end_date, int page,
                     json_t *out_array) {
    char url[1024];
    snprintf(url, sizeof(url),
        "%s?q=%%2213F-HR%%22&dateRange=custom&startdt=%s&enddt=%s&page=%d&counts=1",
        API_BASE, start_date, end_date, page);

    HttpBuf *buf = http_get(url);
    if (!buf) return -1;

    json_error_t err;
    json_t *root = json_loads(buf->data, 0, &err);
    if (!root) { free(buf->data); free(buf); return -1; }

    json_t *hits = json_object_get(root, "hits");
    json_t *hits_arr = hits ? json_object_get(hits, "hits") : NULL;
    json_t *total = hits ? json_object_get(hits, "total") : NULL;

    long total_count = 0;
    if (total && json_is_object(total)) {
        json_t *v = json_object_get(total, "value");
        if (v && json_is_integer(v)) total_count = json_integer_value(v);
    }

    if (hits_arr && json_is_array(hits_arr)) {
        size_t idx;
        json_t *hit;
        json_array_foreach(hits_arr, idx, hit) {
            json_t *src = json_object_get(hit, "_source");
            if (!src) continue;

            json_t *display_names = json_object_get(src, "display_names");
            json_t *file_type = json_object_get(src, "file_type");
            json_t *file_date = json_object_get(src, "file_date");
            json_t *file_num = json_object_get(src, "file_num");
            json_t *period_ending = json_object_get(src, "period_ending");
            json_t *ciks = json_object_get(src, "ciks");

            const char *name = (display_names && json_array_size(display_names) > 0)
                ? json_string_value(json_array_get(display_names, 0)) : "unknown";
            const char *ftype = file_type ? json_string_value(file_type) : "";
            const char *fdate = file_date ? json_string_value(file_date) : "";
            const char *fnum = (file_num && json_array_size(file_num) > 0)
                ? json_string_value(json_array_get(file_num, 0)) : "";
            const char *period = period_ending ? json_string_value(period_ending) : "";

            // Filter for 13F-HR only (not /A amendments)
            if (strstr(ftype, "13F-HR") && !strstr(ftype, "/A")) {
                json_t *entry = json_object();
                json_object_set_new(entry, "name", json_string(name));
                json_object_set_new(entry, "file_type", json_string(ftype));
                json_object_set_new(entry, "filing_date", json_string(fdate));
                json_object_set_new(entry, "file_num", json_string(fnum));
                json_object_set_new(entry, "period_ending", json_string(period));

                if (ciks && json_array_size(ciks) > 0)
                    json_object_set(entry, "cik", json_array_get(ciks, 0));

                json_array_append_new(out_array, entry);
            }
        }
    }

    json_decref(root);
    free(buf->data);
    free(buf);
    return (int)total_count;
}

// ── Main fetch ──
static int fetch_and_store(sqlite3 *db) {
    json_t *all_filings = json_array();

    // Fetch filings for last 90 days (2-3 quarters)
    time_t now = time(NULL);
    char end_date[16], start_date[16];
    struct tm *tm = gmtime(&now);
    strftime(end_date, sizeof(end_date), "%Y-%m-%d", tm);

    // 90 days back
    time_t start = now - 90 * 86400;
    tm = gmtime(&start);
    strftime(start_date, sizeof(start_date), "%Y-%m-%d", tm);

    int total = 0;
    for (int page = 0; page < 3; page++) { // 3 pages max (~300 filings)
        int count = fetch_13f(start_date, end_date, page, all_filings);
        if (count <= 0 || count < 100) break; // last page
        total = count;
    }

    printf("Fetched %zu 13F-HR filings (total reported: %d)\n",
           json_array_size(all_filings), total);

    // Store in SQLite
    db_exec(db, "CREATE TABLE IF NOT EXISTS filings ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "filing_date TEXT NOT NULL,"
                "filer_name TEXT,"
                "form_type TEXT,"
                "file_num TEXT,"
                "period_ending TEXT,"
                "cik TEXT,"
                "UNIQUE(file_num, filing_date)"
                ")");

    // Get today's date for insertion
    char today[16];
    tm = gmtime(&now);
    strftime(today, sizeof(today), "%Y-%m-%d", tm);

    size_t idx;
    json_t *entry;
    int inserted = 0;
    json_array_foreach(all_filings, idx, entry) {
        const char *name = json_string_value(json_object_get(entry, "name"));
        const char *ftype = json_string_value(json_object_get(entry, "file_type"));
        const char *fdate = json_string_value(json_object_get(entry, "filing_date"));
        const char *fnum = json_string_value(json_object_get(entry, "file_num"));
        const char *period = json_string_value(json_object_get(entry, "period_ending"));
        json_t *cik_json = json_object_get(entry, "cik");
        const char *cik = cik_json ? json_string_value(cik_json) : "";

        // Use sqlite3_mprintf for proper escaping
        char *sql = sqlite3_mprintf(
            "INSERT OR IGNORE INTO filings (filing_date, filer_name, form_type, file_num, period_ending, cik) "
            "VALUES ('%s', '%q', '%q', '%q', '%q', '%q')",
            fdate ? fdate : "", name ? name : "", ftype ? ftype : "",
            fnum ? fnum : "", period ? period : "", cik ? cik : "");
        int rc2 = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc2 == SQLITE_OK) inserted++;
    }

    printf("Inserted %d new 13F-HR records\n", inserted);

    // ── Compute features ──
    // Track filing dates for density computation
    db_exec(db, "CREATE TABLE IF NOT EXISTS daily_counts ("
                "date TEXT PRIMARY KEY,"
                "filing_count INTEGER DEFAULT 0"
                ")");

    // Upsert today's count
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO daily_counts (date, filing_count) VALUES ('%s', %zu) "
        "ON CONFLICT(date) DO UPDATE SET filing_count = excluded.filing_count",
        today, json_array_size(all_filings));
    db_exec(db, sql);

    // Compute density: avg filings/day over last 30 days
    double density_30d = 0.0;
    sqlite3_stmt *stmt;
    int rc_prep = sqlite3_prepare_v2(db,
        "SELECT COALESCE(AVG(filing_count), 0) FROM daily_counts "
        "WHERE date >= date('now', '-30 days')", -1, &stmt, NULL);
    if (rc_prep == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        density_30d = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // Compute trend: last 7 days vs prior 23 days
    double density_7d = 0.0, density_23d_prior = 0.0;
    rc_prep = sqlite3_prepare_v2(db,
        "SELECT COALESCE(AVG(filing_count), 0) FROM daily_counts "
        "WHERE date >= date('now', '-7 days')", -1, &stmt, NULL);
    if (rc_prep == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        density_7d = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    rc_prep = sqlite3_prepare_v2(db,
        "SELECT COALESCE(AVG(filing_count), 0) FROM daily_counts "
        "WHERE date >= date('now', '-30 days') AND date < date('now', '-7 days')",
        -1, &stmt, NULL);
    if (rc_prep == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        density_23d_prior = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // ── Output features as JSON ──
    // F63: inst_filing_density — filing density normalized to [0,1]
    //    Baseline: ~100-300 13F filings/day is normal (thousands of managers × quarterly)
    //    Cap at 500/day, floor at 0
    double density_feat = density_30d;
    if (density_feat > 500.0) density_feat = 500.0;
    double density_norm = density_feat / 500.0;

    // F64: inst_filing_trend — change in filing rate [0,1]
    //    0.5 = no change, >0.5 = rising institutional activity
    double trend_raw = (density_23d_prior > 0.001)
        ? (density_7d - density_23d_prior) / density_23d_prior
        : 0.0;
    // Clamp to [-0.5, 0.5] then map to [0,1]
    if (trend_raw > 0.5) trend_raw = 0.5;
    if (trend_raw < -0.5) trend_raw = -0.5;
    double trend_norm = (trend_raw + 0.5); // 0 = sharply falling, 0.5 = flat, 1 = rising

    // Save output
    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/inst_features.json", OUTPUT_DIR);
    json_t *out = json_object();
    json_object_set_new(out, "inst_filing_density", json_real(density_norm));
    json_object_set_new(out, "inst_filing_trend", json_real(trend_norm));
    json_object_set_new(out, "density_30d_avg", json_real(density_30d));
    json_object_set_new(out, "density_7d_avg", json_real(density_7d));
    json_object_set_new(out, "filing_count_today", json_real(json_array_size(all_filings)));
    json_object_set_new(out, "trend_raw", json_real(trend_raw));
    json_object_set_new(out, "timestamp", json_string(today));

    json_dump_file(out, out_path, JSON_INDENT(2));
    printf("Features written to %s\n", out_path);
    printf("  inst_filing_density: %.3f\n", density_norm);
    printf("  inst_filing_trend:   %.3f\n", trend_norm);
    printf("  density_30d_avg:     %.1f/day\n", density_30d);
    printf("  density_7d_avg:      %.1f/day\n", density_7d);

    json_decref(out);
    json_decref(all_filings);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s fetch\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "fetch") != 0) {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }

    // Ensure directories exist
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", DB_DIR);
    system(cmd);

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/13f.db", DB_DIR);
    sqlite3 *db = db_open(db_path);
    if (!db) return 1;

    int ret = fetch_and_store(db);

    sqlite3_close(db);
    return ret;
}
