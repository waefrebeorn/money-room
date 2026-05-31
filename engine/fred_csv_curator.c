/**
 * fred_csv_curator.c — FRED CSV Gateway curator (T1502-T1557)
 * Fetches 56 FRED economic series from ivo-welch.info cgi gateway
 * All free, no API key needed. Stores in SQLite.
 * 
 * Compile: gcc -O3 -Wall -Wextra -o fred_csv_curator fred_csv_curator.c -lcurl -lsqlite3 -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <time.h>
#include <math.h>

#define DB_PATH "/home/wubu2/money-room/engine/timeline.db"
#define BUFFER_SIZE 1048576  // 1MB per series

/* 56 FRED series from the battleship + extras */
typedef struct {
    const char *symbol;
    const char *name;
    const char *frequency;  /* D=Daily, W=Weekly, M=Monthly, Q=Quarterly */
} FredSeries;

FredSeries SERIES[] = {
    {"DGS10",   "10-Year Treasury Yield", "D"},
    {"DGS2",    "2-Year Treasury Yield", "D"},
    {"DGS30",   "30-Year Treasury Yield", "D"},
    {"DGS5",    "5-Year Treasury Yield", "D"},
    {"DGS3MO",  "3-Month Treasury Yield", "D"},
    {"T10Y2Y",  "10Y-2Y Yield Spread", "D"},
    {"T10Y3M",  "10Y-3M Yield Spread", "D"},
    {"CPIAUCSL","Consumer Price Index (All Urban)", "M"},
    {"CPILFESL","Core CPI (Ex Food & Energy)", "M"},
    {"PCEPILFE","Core PCE (Fed Target)", "M"},
    {"FEDFUNDS","Federal Funds Rate", "D"},
    {"UNRATE",  "Unemployment Rate", "M"},
    {"PAYEMS",  "Nonfarm Payrolls", "M"},
    {"GDP",     "Real Gross Domestic Product", "Q"},
    {"GDPPOT",  "Real Potential GDP", "Q"},
    {"M2SL",    "M2 Money Supply", "M"},
    {"VIXCLS",  "CBOE Volatility Index (VIX)", "D"},
    {"T5YIE",   "5-Year Breakeven Inflation", "D"},
    {"T10YIE",  "10-Year Breakeven Inflation", "D"},
    {"T5YIFR",  "5-Year Forward Inflation Expectation", "D"},
    {"BAMLH0A0HYM2","ICE BofA High Yield Spread", "D"},
    {"BAMLC0A4CBBBEY","ICE BofA BBB Corp Yield", "D"},
    {"SOFR",    "Secured Overnight Financing Rate", "D"},
    {"RRPONTSYD","Overnight Reverse Repo", "D"},
    {"TOTCI",   "Total Construction Spending", "M"},
    {"HOUST",   "Housing Starts", "M"},
    {"PERMIT",  "Building Permits", "M"},
    {"RSXFS",   "Retail Sales (Ex Food Services)", "M"},
    {"INDPRO",  "Industrial Production", "M"},
    {"TCU",     "Capacity Utilization", "M"},
    {"DGORDER", "Durable Goods Orders", "M"},
    {"BUSINV",  "Business Inventories", "M"},
    {"BOPTEXP", "Trade Balance: Exports", "M"},
    {"BOPTIMP", "Trade Balance: Imports", "M"},
    {"PCE",     "Personal Consumption Expenditures", "M"},
    {"DPI",     "Disposable Personal Income", "M"},
    {"PSAVERT", "Personal Saving Rate", "M"},
    {"IC4WSA",  "Initial Claims (4-Week Avg)", "W"},
    {"CC4WSA",  "Continuing Claims (4-Week Avg)", "W"},
    {"UEMPLT5", "Unemployed <5 Weeks", "M"},
    {"UEMP27OV","Unemployed 27+ Weeks", "M"},
    {"UEMPMEAN","Mean Duration of Unemployment", "M"},
    {"LNS14000003","Unemployment Rate 25-54 Years", "M"},
    {"CIVPART",  "Labor Force Participation Rate", "M"},
    {"CSUSHPISA","Case-Shiller Home Price Index", "M"},
    {"MORTGAGE30US","30-Year Mortgage Rate", "W"},
    {"REVOLSL", "Revolving Consumer Credit Owned/Securitized", "M"},
    {"NONREVSL", "Nonrevolving Consumer Credit Owned/Securitized", "M"},
    {"TOTALSLAR", "Total Consumer Credit (Adj for Inflation)", "M"},
    {"CP",      "Corporate Profits", "Q"},
    {"W875RX1", "Real Disposable Personal Income", "M"},
    {"USSLIND", "Leading Index for US", "M"},
    {"USREC",   "NBER Recession Indicator", "M"},
    {"GDPPOT",  "Real Potential GDP", "Q"},
    {"USCONS",  "Total Construction Put in Place", "M"},
};
const int N_SERIES = sizeof(SERIES) / sizeof(SERIES[0]);

typedef struct {
    char *data;
    size_t size;
} MemBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    MemBuf *buf = (MemBuf*)userdata;
    size_t total = size * nmemb;
    size_t needed = buf->size + total + 1;
    char *new_data = realloc(buf->data, needed);
    if (!new_data) return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static int init_db(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS fred_data ("
        "  series_id TEXT NOT NULL,"
        "  obs_date TEXT NOT NULL,"
        "  value REAL,"
        "  name TEXT,"
        "  frequency TEXT,"
        "  updated_at TEXT DEFAULT (datetime('now')),"
        "  PRIMARY KEY (series_id, obs_date)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_fred_series ON fred_data(series_id);"
        "CREATE INDEX IF NOT EXISTS idx_fred_date ON fred_data(obs_date);";
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "DB init: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int fetch_series(const FredSeries *s, MemBuf *buf, const char *base_url) {
    char url[512];
    snprintf(url, sizeof(url), "%s?symbol=%s", base_url, s->symbol);
    
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    
    buf->size = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MoneyRoom/1.0 FRED Curator");
    
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        fprintf(stderr, "  [FAIL] %s: curl error %d\n", s->symbol, res);
        return -1;
    }
    if (http_code != 200) {
        fprintf(stderr, "  [FAIL] %s: HTTP %ld\n", s->symbol, http_code);
        return -1;
    }
    return 0;
}

static int insert_data(sqlite3 *db, const FredSeries *s, MemBuf *buf) {
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO fred_data (series_id, obs_date, value, name, frequency) "
                      "VALUES (?1, ?2, ?3, ?4, ?5)";
    
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    
    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);
    
    int n_rows = 0, n_skip = 0;
    char *line = buf->data;
    int line_num = 0;
    
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        
        line_num++;
        
        /* Skip header line */
        if (line_num == 1) {
            if (next) { line = next + 1; continue; } else break;
        }
        
        /* Parse: yyyymmdd,value */
        char date_str[16], val_str[64];
        if (sscanf(line, "%15[^,],%63s", date_str, val_str) >= 2) {
            double val;
            if (strcmp(val_str, ".") == 0 || strcmp(val_str, "") == 0) {
                n_skip++;
            } else {
                val = atof(val_str);
                sqlite3_bind_text(stmt, 1, s->symbol, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, date_str, -1, SQLITE_STATIC);
                sqlite3_bind_double(stmt, 3, val);
                sqlite3_bind_text(stmt, 4, s->name, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 5, s->frequency, -1, SQLITE_STATIC);
                
                rc = sqlite3_step(stmt);
                sqlite3_reset(stmt);
                if (rc == SQLITE_DONE) n_rows++;
            }
        }
        
        if (next) line = next + 1;
        else break;
    }
    
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_finalize(stmt);
    
    printf("  %s: %d rows, %d skipped\n", s->symbol, n_rows, n_skip);
    return n_rows;
}

static int update_all(const char *base_url) {
    sqlite3 *db;
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Can't open DB: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    
    init_db(db);
    
    MemBuf buf = {NULL, 0};
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    int total_ok = 0, total_fail = 0, total_rows = 0;
    
    for (int i = 0; i < N_SERIES; i++) {
        printf("[%d/%d] %-15s %s\n", i+1, N_SERIES, SERIES[i].symbol, SERIES[i].name);
        
        if (buf.data) { free(buf.data); buf.data = NULL; buf.size = 0; }
        
        if (fetch_series(&SERIES[i], &buf, base_url) == 0) {
            int rows = insert_data(db, &SERIES[i], &buf);
            if (rows > 0) {
                total_ok++;
                total_rows += rows;
            } else {
                total_fail++;
            }
        } else {
            total_fail++;
        }
    }
    
    free(buf.data);
    sqlite3_close(db);
    curl_global_cleanup();
    
    printf("\n=== FRED CURATOR RESULT ===\n");
    printf("Series OK:  %d\n", total_ok);
    printf("Series FAIL: %d\n", total_fail);
    printf("Total rows: %d\n", total_rows);
    printf("===========================\n");
    
    return total_fail;
}

static void print_stats(sqlite3 *db) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT series_id, COUNT(*), MAX(obs_date), "
                      "ROUND(AVG(value), 2), name "
                      "FROM fred_data GROUP BY series_id ORDER BY series_id";
    
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return;
    
    printf("\n%-15s %8s %14s %12s  %s\n", "SERIES", "ROWS", "LATEST", "MEAN", "NAME");
    printf("--------------- -------- -------------- ------------  --------------------------------\n");
    
    int total = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *sid = (const char*)sqlite3_column_text(stmt, 0);
        int cnt = sqlite3_column_int(stmt, 1);
        const char *latest = (const char*)sqlite3_column_text(stmt, 2);
        double avg = sqlite3_column_double(stmt, 3);
        const char *name = (const char*)sqlite3_column_text(stmt, 4);
        printf("%-15s %8d %14s %11.2f  %s\n", sid, cnt, latest ? latest : "-", avg, name ? name : "");
        total += cnt;
    }
    printf("--------------- -------- -------------- ------------  --------------------------------\n");
    printf("%-15s %8d\n", "TOTAL", total);
    sqlite3_finalize(stmt);
}

int main(int argc, char **argv) {
    const char *base_url = "https://www.ivo-welch.info/cgi-bin/fredwrap";
    
    if (argc > 1) {
        if (strcmp(argv[1], "stats") == 0 || strcmp(argv[1], "status") == 0) {
            sqlite3 *db;
            if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
                print_stats(db);
                sqlite3_close(db);
            }
            return 0;
        } else if (strcmp(argv[1], "update") == 0 || strcmp(argv[1], "fetch") == 0) {
            return update_all(base_url);
        } else if (strcmp(argv[1], "single") == 0 && argc > 2) {
            /* Fetch single series */
            sqlite3 *db;
            if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return 1;
            init_db(db);
            MemBuf buf = {NULL, 0};
            curl_global_init(CURL_GLOBAL_DEFAULT);
            for (int i = 0; i < N_SERIES; i++) {
                if (strcmp(SERIES[i].symbol, argv[2]) == 0) {
                    if (buf.data) { free(buf.data); buf.data = NULL; buf.size = 0; }
                    if (fetch_series(&SERIES[i], &buf, base_url) == 0) {
                        insert_data(db, &SERIES[i], &buf);
                    }
                    break;
                }
            }
            free(buf.data);
            sqlite3_close(db);
            curl_global_cleanup();
            return 0;
        } else if (strcmp(argv[1], "list") == 0) {
            printf("FRED CSV Curator - %d series available:\n", N_SERIES);
            for (int i = 0; i < N_SERIES; i++) {
                printf("  %-15s %s\n", SERIES[i].symbol, SERIES[i].name);
            }
            return 0;
        }
    }
    
    printf("Usage: fred_csv_curator <command>\n");
    printf("  update  - Fetch all 56 FRED series into SQLite\n");
    printf("  stats   - Show summary statistics\n");
    printf("  list    - List all available series\n");
    printf("  single <SYMBOL> - Fetch single series\n");
    return 0;
}
