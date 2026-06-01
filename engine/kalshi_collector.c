/**
 * kalshi_collector.c — Kalshi Prediction Market Data Collector
 *
 * Pulls all available Kalshi market data (public API, no auth needed).
 * Writes to timeline.db with source='kalshi_<ticker>'.
 *
 * Markets are organized:
 *   Events → Series → Markets (each market = yes/no binary)
 *   Each market has: ticker, title, current_price, volume, 
 *   open_interest, close_time, status
 *
 * Also collects Kalshi historical data for backtesting.
 *
 * NOT FINANCIAL ADVICE. All data is public market data.
 *
 * Build: make kalshi  (see Makefile)
 * Usage: ./kalshi_collector [mode]
 *   mode=scan    — scan all markets (default)
 *   mode=history — fetch historical data for settled markets
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <jansson.h>

#define KALSHI_API "https://api.elections.kalshi.com/trade-api/v2"
#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define MAX_URL 4096
#define MAX_JSON 2097152  /* 2MB response buffer */

/* ── HTTP response buffer ── */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} HttpResponse;

static size_t write_cb(void *ptr, size_t sz, size_t nmemb, void *user) {
    size_t total = sz * nmemb;
    HttpResponse *r = (HttpResponse *)user;
    if (r->len + total >= r->cap) {
        r->cap = r->len + total + 65536;
        r->data = realloc(r->data, r->cap);
    }
    memcpy(r->data + r->len, ptr, total);
    r->len += total;
    r->data[r->len] = '\0';
    return total;
}

static HttpResponse http_get(const char *url) {
    HttpResponse r = {calloc(1, 65536), 0, 65536};
    CURL *curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "kalshi-collector/1.0");
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    return r;
}

/* ── DB helpers ── */
static sqlite3 *g_db = NULL;

static void db_init(void) {
    sqlite3_open(DB_PATH, &g_db);
    char *err = NULL;
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;",0,0,&err);
}

static void db_insert(const char *source, long long ts, const char *category, const char *data_json) {
    if (!g_db) return;
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO timeline (ts, source, category, data, collected_at) "
                       "VALUES (?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, data_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (long long)time(NULL));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void db_close(void) {
    if (g_db) sqlite3_close(g_db);
}

/* ── Timestamp helpers ── */
static long long parse_kalshi_ts(const char *iso) {
    if (!iso || !*iso) return 0;
    struct tm tm = {0};
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", 
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = 0;
        return (long long)mktime(&tm);
    }
    return 0;
}

static char *sanitize_ticker(const char *ticker) {
    /* Create a safe source name from ticker */
    static char buf[128];
    int j = 0;
    for (int i = 0; ticker[i] && j < 120; i++) {
        char c = ticker[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
            (c >= '0' && c <= '9') || c == '_') buf[j++] = c;
        else if (c == '-' || c == '.') buf[j++] = '_';
        /* skip other chars */
    }
    buf[j] = '\0';
    return buf;
}

/* ── Scan all markets ── */
static void scan_markets(void) {
    printf("  Scanning Kalshi markets...\n");
    
    char url[MAX_URL];
    int page = 0, total = 0;
    const char *cursor = NULL;
    
    do {
        if (cursor) {
            snprintf(url, sizeof(url), "%s/markets?limit=100&cursor=%s", 
                     KALSHI_API, cursor);
        } else {
            snprintf(url, sizeof(url), "%s/markets?limit=100", KALSHI_API);
        }
        
        /* Rate limit: 200ms delay between pages */
        struct timespec d = {0, 200000000L};
        nanosleep(&d, NULL);
        
        HttpResponse resp = http_get(url);
        json_error_t err;
        json_t *root = json_loads(resp.data, 0, &err);
        free(resp.data);
        
        if (!root) {
            printf("    JSON error at page %d\n", page);
            break;
        }
        
        json_t *markets = json_object_get(root, "markets");
        if (!markets || !json_is_array(markets)) {
            json_decref(root);
            break;
        }
        
        int n = (int)json_array_size(markets);
        for (int i = 0; i < n; i++) {
            json_t *m = json_array_get(markets, i);
            const char *ticker = json_string_value(json_object_get(m, "ticker"));
            const char *title = json_string_value(json_object_get(m, "title"));
            const char *status = json_string_value(json_object_get(m, "status"));
            const char *close_time = json_string_value(json_object_get(m, "close_time"));
            double yes_bid = json_number_value(json_object_get(m, "yes_bid"));
            double yes_ask = json_number_value(json_object_get(m, "yes_ask"));
            double volume = json_number_value(json_object_get(m, "volume"));
            double open_interest = json_number_value(json_object_get(m, "open_interest"));
            
            if (!ticker) continue;
            
            /* Insert as timeline data */
            long long ts = parse_kalshi_ts(close_time);
            if (ts == 0) ts = time(NULL);
            
            char source[128];
            snprintf(source, sizeof(source), "kalshi_%s", sanitize_ticker(ticker));
            
            char category[64];
            if (strstr(ticker, "SPORTS")) snprintf(category, 64, "kalshi_sports");
            else if (strstr(ticker, "ELECTION")) snprintf(category, 64, "kalshi_elections");
            else if (strstr(ticker, "ECONOMY")) snprintf(category, 64, "kalshi_economy");
            else if (strstr(ticker, "CRYPTO")) snprintf(category, 64, "kalshi_crypto");
            else snprintf(category, 64, "kalshi_other");
            
            char data_json[1024];
            snprintf(data_json, sizeof(data_json),
                "{\"ticker\":\"%s\",\"title\":\"%s\",\"status\":\"%s\","
                "\"yes_bid\":%.4f,\"yes_ask\":%.4f,\"volume\":%.0f,"
                "\"open_interest\":%.0f,\"price\":%.4f}",
                ticker, title ? title : "", status ? status : "",
                yes_bid, yes_ask, volume, open_interest,
                (yes_bid + yes_ask) / 2.0);
            
            db_insert(source, ts, category, data_json);
            total++;
        }
        
        /* Get cursor for next page — MUST strdup, memory freed by json_decref */
        json_t *cursor_obj = json_object_get(root, "cursor");
        static char cursor_buf[256];
        if (cursor_obj && json_is_string(cursor_obj)) {
            const char *c = json_string_value(cursor_obj);
            if (c) { strncpy(cursor_buf, c, sizeof(cursor_buf)-1); cursor_buf[sizeof(cursor_buf)-1] = 0; cursor = cursor_buf; }
            else { cursor = NULL; }
        } else {
            cursor = NULL;
        }
        
        json_decref(root);
        page++;
        
        if (page % 5 == 0) printf("    Page %d: %d markets so far...\n", page, total);
        
    } while (cursor && page < 1000); /* Safety limit (1000 pages × 100 = 100K markets max) */
    
    printf("  ✅ Total: %d Kalshi markets collected\n", total);
}

/* ── Main ── */
int main(int argc, char *argv[]) {
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════╗\n");
    printf("  ║   KALSHI COLLECTOR — Public Market Data             ║\n");
    printf("  ║   No auth needed · All markets · timeline.db        ║\n");
    printf("  ╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  NOT FINANCIAL ADVICE. Public market data collection.\n");
    printf("\n");
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    db_init();
    scan_markets();
    db_close();
    curl_global_cleanup();
    
    printf("\n  Done.\n");
    return 0;
}
