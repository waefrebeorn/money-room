/**
 * economic_collector.c — Economic & Election Event Data Collector
 *
 * Fetches Kalshi economic event markets (CPI, NFP, FOMC, GDP, etc.)
 * and election markets (Senate, House, Governor races).
 * Writes to timeline.db with source='kalshi_economic_{ticker}'.
 *
 * Kalshi API is public for market data. No auth needed for reads.
 * US-legal, CFTC-regulated. Zero fees on most markets.
 *
 * Categories: economy (CPI, NFP, FOMC, GDP, UNEMPLOYMENT, PPI, RETAIL, SENTIMENT)
 *             elections (SENATE, HOUSE, GOVERNOR, PRESIDENTIAL)
 *             crypto (BTC, ETH price targets)
 *
 * Build: gcc -O2 -o economic_collector economic_collector.c -lcurl -ljansson -lsqlite3 -lm
 * Usage: ./economic_collector [mode=all|economy|elections|crypto]
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
#define MAX_JSON 2097152

typedef struct { char *data; size_t len; size_t cap; } HttpResponse;

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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "economic-collector/1.0");
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    return r;
}

static sqlite3 *g_db = NULL;
static void db_init(void) {
    sqlite3_open(DB_PATH, &g_db);
    char *err = NULL;
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;",0,0,&err);
}

static void db_insert(const char *source, long long ts, const char *category, const char *data_json) {
    if (!g_db) return;
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO timeline (ts, source, category, data, collected_at) VALUES (?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, data_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (long long)time(NULL));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void db_close(void) { if (g_db) sqlite3_close(g_db); }

static long long parse_ts(const char *iso) {
    if (!iso || !*iso) return 0;
    struct tm tm = {0};
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6) {
        tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = 0;
        return (long long)mktime(&tm);
    }
    return 0;
}

static char *sanitize(const char *ticker) {
    static char buf[128];
    int j = 0;
    for (int i = 0; ticker[i] && j < 120; i++) {
        char c = ticker[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') buf[j++] = c;
        else if (c == '-' || c == '.') buf[j++] = '_';
    }
    buf[j] = '\0';
    return buf;
}

// Category keywords
static const char *ECON_KEYWORDS[] = {"INFLATION", "CPI", "NFP", "PAYROLL", "FOMC", "FED", "RATE",
    "GDP", "UNEMPLOYMENT", "JOBLESS", "PPI", "PRODUCER", "RETAIL", "SENTIMENT", "CONSUMER", NULL};
static const char *ELECTION_KEYWORDS[] = {"ELECTION", "SENATE", "HOUSE", "GOVERNOR", "PRESIDENT", 
    "DEMOCRAT", "REPUBLICAN", "NOMINEE", "PRIMARY", NULL};
static const char *CRYPTO_KEYWORDS[] = {"CRYPTO", "BITCOIN", "BTC", "ETHEREUM", "ETH", "SOLANA", "SOL", NULL};

static int matches_any(const char *ticker, const char *title, const char **keywords) {
    for (int i = 0; keywords[i]; i++) {
        if (strstr(ticker, keywords[i]) || (title && strstr(title, keywords[i]))) return 1;
    }
    return 0;
}

static void scan_category(const char *category_name, const char **keywords, const char *category_tag) {
    printf("  Scanning %s markets...\n", category_name);
    
    char url[MAX_URL];
    int page = 0, total = 0;
    const char *cursor = NULL;
    
    do {
        if (cursor) snprintf(url, sizeof(url), "%s/markets?limit=100&cursor=%s", KALSHI_API, cursor);
        else snprintf(url, sizeof(url), "%s/markets?limit=100", KALSHI_API);
        
        HttpResponse resp = http_get(url);
        json_error_t err;
        json_t *root = json_loads(resp.data, 0, &err);
        free(resp.data);
        
        if (!root) break;
        
        json_t *markets = json_object_get(root, "markets");
        if (!markets || !json_is_array(markets)) { json_decref(root); break; }
        
        int n = (int)json_array_size(markets);
        for (int i = 0; i < n; i++) {
            json_t *m = json_array_get(markets, i);
            const char *ticker = json_string_value(json_object_get(m, "ticker"));
            const char *title = json_string_value(json_object_get(m, "title"));
            if (!ticker) continue;
            
            // Filter by keyword match if keywords provided
            if (keywords && !matches_any(ticker, title, keywords)) continue;
            
            const char *status = json_string_value(json_object_get(m, "status"));
            const char *close_time = json_string_value(json_object_get(m, "close_time"));
            double yes_bid = json_number_value(json_object_get(m, "yes_bid"));
            double yes_ask = json_number_value(json_object_get(m, "yes_ask"));
            double volume = json_number_value(json_object_get(m, "volume"));
            double open_interest = json_number_value(json_object_get(m, "open_interest"));
            
            long long ts = parse_ts(close_time);
            if (ts == 0) ts = time(NULL);
            
            char source[256];
            snprintf(source, sizeof(source), "kalshi_%s", sanitize(ticker));
            
            char data_json[1024];
            snprintf(data_json, sizeof(data_json),
                "{\"ticker\":\"%s\",\"title\":\"%s\",\"status\":\"%s\","
                "\"yes_bid\":%.4f,\"yes_ask\":%.4f,\"volume\":%.0f,"
                "\"open_interest\":%.0f,\"price\":%.4f}",
                ticker, title ? title : "", status ? status : "",
                yes_bid, yes_ask, volume, open_interest,
                (yes_bid + yes_ask) / 2.0);
            
            db_insert(source, ts, category_tag, data_json);
            total++;
        }
        
        json_t *pagination = json_object_get(root, "pagination");
        cursor = pagination ? json_string_value(json_object_get(pagination, "next_cursor")) : NULL;
        json_decref(root);
        page++;
    } while (cursor && page < 50);
    
    printf("    ✅ %d %s markets\n", total, category_name);
}

int main(int argc, char *argv[]) {
    const char *mode = (argc > 1) ? argv[1] : "all";
    
    printf("  ECONOMIC & ELECTION COLLECTOR\n");
    printf("  Source: Kalshi API | CFTC-regulated\n\n");
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    db_init();
    
    if (strcmp(mode, "all") == 0 || strcmp(mode, "economy") == 0)
        scan_category("Economic", ECON_KEYWORDS, "kalshi_economy");
    
    if (strcmp(mode, "all") == 0 || strcmp(mode, "elections") == 0)
        scan_category("Election", ELECTION_KEYWORDS, "kalshi_elections");
    
    if (strcmp(mode, "all") == 0 || strcmp(mode, "crypto") == 0)
        scan_category("Crypto", CRYPTO_KEYWORDS, "kalshi_crypto");
    
    if (strcmp(mode, "all") == 0 || strcmp(mode, "all-markets") == 0)
        scan_category("All Markets", NULL, "kalshi_other");
    
    db_close();
    curl_global_cleanup();
    
    printf("\n  Done.\n");
    return 0;
}
