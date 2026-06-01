/**
 * dataset_ingestor.c — Free historical datasets → timeline.db
 * 6 sources: Polymarket, Yahoo Finance, SEC EDGAR, FRED, PredictIt, ESPN sports
 * All free APIs, no keys required.
 *
 * gcc -O3 dataset_ingestor.c -o dataset_ingestor -lcurl -lsqlite3 -ljansson -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <jansson.h>

#define HOME_DIR "/home/wubu2"
#define TIMELINE_DB HOME_DIR "/.hermes/timeline.db"
#define DATASET_DIR HOME_DIR "/.hermes/datasets"
#define USER_AGENT "DatasetIngestor/1.0 (research; money-room)"

typedef struct { char *data; size_t len; } buf_t;

static size_t write_cb(void *ptr, size_t sz, size_t nm, void *u) {
    buf_t *b = (buf_t*)u;
    size_t total = sz * nm;
    char *p = realloc(b->data, b->len + total + 1);
    if (!p) return 0;
    memcpy(p + b->len, ptr, total);
    b->data = p; b->len += total; p[b->len] = 0;
    return total;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    buf_t b = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) { free(b.data); return NULL; }
    return b.data;
}

static sqlite3 *db_open(void) {
    sqlite3 *db;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) return NULL;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;", 0,0,0);
    return db;
}

static void db_insert(sqlite3 *db, long ts, const char *src, const char *cat, const char *json_data) {
    sqlite3_stmt *s;
    char *sql = "INSERT INTO timeline (ts, source, category, data, collected_at) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &s, 0) != SQLITE_OK) return;
    sqlite3_bind_int64(s, 1, ts);
    sqlite3_bind_text(s, 2, src, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, cat, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, json_data, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 5, time(0));
    sqlite3_step(s);
    sqlite3_finalize(s);
}

// ── CSV row → JSON object ──
static char **csv_split(const char *line, int *n) {
    int cap = 32, i = 0;
    char **f = calloc(cap, sizeof(char*));
    const char *p = line;
    while (*p) {
        if (i >= cap) f = realloc(f, (cap*=2)*sizeof(char*));
        if (*p == '"') {
            p++; int len = 0; char buf[4096];
            while (*p && !(*p == '"' && *(p+1) == ',')) {
                if (*p == '"' && *(p+1) == '"') { buf[len++]='"'; p+=2; }
                else buf[len++]=*p++;
            }
            if (*p=='"') p++;
            buf[len]=0; f[i]=strdup(buf);
        } else {
            const char *start = p;
            while (*p && *p != ',') p++;
            f[i] = strndup(start, p-start);
        }
        i++;
        if (*p == ',') p++;
    }
    *n = i; return f;
}

// ── 1. Polymarket historical (manja316 GitHub CSV) ──
static int ingest_polymarket(sqlite3 *db) {
    char *data = http_get("https://raw.githubusercontent.com/manja316/polymarket-historical-data/main/markets.csv");
    if (!data) { printf("[FAIL] Polymarket CSV\n"); return 0; }
    int count = 0;
    char *line = data, *nl;
    int header = 1;
    while ((nl = strchr(line, '\n'))) {
        *nl = 0;
        if (!header) {
            int n; char **f = csv_split(line, &n);
            if (n >= 2) {
                long ts = (long)time(0);
                char *close_t = strdup(f[0]); // first col
                // try to parse ts
                char json[4096]; int pos=0;
                pos += snprintf(json+pos, sizeof(json)-pos, "{");
                for (int i = 0; i < n && i < 20; i++) {
                    if (i > 0) pos += snprintf(json+pos, sizeof(json)-pos, ",");
                    pos += snprintf(json+pos, sizeof(json)-pos, "\"col%d\":", i);
                    // escape JS
                    char *esc = json_dumps(json_string(f[i]?f[i]:""), 0);
                    if (esc) { pos += snprintf(json+pos, sizeof(json)-pos, "%s", esc); free(esc); }
                    else pos += snprintf(json+pos, sizeof(json)-pos, "\"\"");
                }
                pos += snprintf(json+pos, sizeof(json)-pos, "}");
                db_insert(db, ts, "polymarket_historical", "market_snapshot", json);
                count++;
                free(close_t);
            }
            for (int i=0; i<n; i++) free(f[i]); free(f);
        }
        header = 0;
        line = nl + 1;
    }
    free(data);
    printf("  Ingested %d Polymarket historical markets\n", count);
    return count;
}

// ── 2. Yahoo Finance — 20 tickers ──
static const char *TICKERS[] = {
    "SPY","QQQ","DIA","IWM","TLT","GLD","SLV","USO",
    "XLE","XLF","XLK","XLV","XLI","XLB","XLRE","XLP","XLU","XLY","VTI","VXUS",
    "BTC-USD","ETH-USD","SOL-USD","XRP-USD","DOGE-USD", 0
};

static int ingest_yfinance(sqlite3 *db) {
    int total = 0;
    long now = time(0);
    long p1 = now - 365*86400*3;
    for (int i = 0; TICKERS[i]; i++) {
        char url[512];
        snprintf(url, sizeof(url),
            "https://query1.finance.yahoo.com/v7/finance/download/%s?period1=%ld&period2=%ld&interval=1d&events=history",
            TICKERS[i], p1, now);
        char *data = http_get(url);
        int cnt = 0;
        if (data) {
            char *line = data, *nl;
            int hdr = 1;
            while ((nl = strchr(line, '\n'))) {
                *nl = 0;
                if (!hdr) {
                    int n; char **f = csv_split(line, &n);
                    if (n >= 7) {
                        // Date,Open,High,Low,Close,Adj Close,Volume
                        char json[512]; int p=0;
                        p += snprintf(json+p, sizeof(json)-p,
                            "{\"Date\":\"%s\",\"Open\":\"%s\",\"High\":\"%s\",\"Low\":\"%s\""
                            ",\"Close\":\"%s\",\"Adj Close\":\"%s\",\"Volume\":\"%s\"}",
                            f[0]?:"",f[1]?:"",f[2]?:"",f[3]?:"",f[4]?:"",f[5]?:"",f[6]?:"");
                        // parse date
                        long ts = now;
                        if (f[0]) {
                            struct tm tm = {0};
                            if (strptime(f[0], "%Y-%m-%d", &tm)) ts = (long)mktime(&tm);
                        }
                        db_insert(db, ts, TICKERS[i], "price_history", json);
                        cnt++;
                    }
                    for (int j=0; j<n; j++) free(f[j]); free(f);
                }
                hdr = 0;
                line = nl + 1;
            }
            free(data);
        }
        printf("  %s: %d days\n", TICKERS[i], cnt);
        total += cnt;
    }
    printf("  Total: %d rows ingested\n", total);
    return total;
}

// ── 3. SEC EDGAR company tickers ──
static int ingest_sec(sqlite3 *db) {
    char *data = http_get("https://www.sec.gov/files/company_tickers.json");
    if (!data) { printf("[FAIL] SEC EDGAR\n"); return 0; }
    int count = 0;
    json_error_t err;
    json_t *root = json_loads(data, 0, &err);
    free(data);
    if (!root || !json_is_object(root)) { printf("  SEC: parse error\n"); json_decref(root); return 0; }
    long ts = time(0);
    const char *key; json_t *val;
    json_object_foreach(root, key, val) {
        if (count >= 5000) break;
        if (json_is_object(val)) {
            json_t *cik = json_object_get(val, "cik_str");
            json_t *ticker = json_object_get(val, "ticker");
            json_t *title = json_object_get(val, "title");
            char json[512]; int p=0;
            p += snprintf(json+p, sizeof(json)-p, "{\"cik\":");
            if (cik && json_is_integer(cik)) p += snprintf(json+p, sizeof(json)-p, "%lld", (long long)json_integer_value(cik));
            else p += snprintf(json+p, sizeof(json)-p, "0");
            if (ticker && json_is_string(ticker))
                p += snprintf(json+p, sizeof(json)-p, ",\"ticker\":\"%s\"", json_string_value(ticker));
            if (title && json_is_string(title))
                p += snprintf(json+p, sizeof(json)-p, ",\"title\":\"%s\"", json_string_value(title));
            p += snprintf(json+p, sizeof(json)-p, "}");
            db_insert(db, ts, "sec_edgar", "company_ticker", json);
            count++;
        }
    }
    json_decref(root);
    printf("  %d SEC companies\n", count);
    return count;
}

// ── 4. FRED macro series ──
typedef struct { const char *id; const char *name; } fred_t;
static fred_t FRED[] = {
    {"HOUST","Housing Starts"},{"UMCSENT","Consumer Sentiment"},{"FEDFUNDS","Fed Funds"},
    {"DGS10","10Y Treasury"},{"DGS2","2Y Treasury"},{"DAAA","AAA Corp Bond"},
    {"DBAA","BAA Corp Bond"},{"T10YIE","10Y Breakeven Inflation"},{"T5YIE","5Y Breakeven Inflation"},
    {"T10Y2YM","T10Y2Y Spread"},{"UNRATE","Unemployment"},{"PAYEMS","Nonfarm Payrolls"},
    {"CPIAUCSL","CPI All Urban"},{"PCE","PCE Index"},{"PCEPILFE","Core PCE"},
    {"GDPC1","Real GDP"},{"INDPRO","Industrial Production"},{"M2SL","M2 Money Supply"},
    {"PSAVERT","Personal Saving Rate"},{"DTWEXBGS","Trade Weighted USD"},
    {"VIXCLS","CBOE Volatility Index"},{"SP500","S&P 500"},
    {0,0}
};

static int ingest_fred(sqlite3 *db) {
    int total = 0;
    for (int i = 0; FRED[i].id; i++) {
        char url[512];
        snprintf(url, sizeof(url),
            "https://fred.stlouisfed.org/graph/fredgraph.csv?id=%s&cosd=2000-01-01&coed=9999-12-31", FRED[i].id);
        char *data = http_get(url);
        int cnt = 0;
        if (data) {
            char *line = data, *nl;
            int hdr = 1;
            while ((nl = strchr(line, '\n'))) {
                *nl = 0;
                if (!hdr && *line) {
                    int n; char **f = csv_split(line, &n);
                    if (n >= 2 && f[0] && f[1]) {
                        long ts = (long)time(0);
                        struct tm tm = {0};
                        if (strptime(f[0], "%Y-%m-%d", &tm)) ts = (long)mktime(&tm);
                        char json[256];
                        snprintf(json, sizeof(json),
                            "{\"series\":\"%s\",\"series_id\":\"%s\",\"date\":\"%s\",\"value\":\"%s\"}",
                            FRED[i].name, FRED[i].id, f[0], f[1]);
                        db_insert(db, ts, FRED[i].id, "economic_series", json);
                        cnt++;
                    }
                    for (int j=0; j<n; j++) free(f[j]); free(f);
                }
                hdr = 0;
                line = nl + 1;
            }
            free(data);
        }
        printf("  %s (%s): %d points\n", FRED[i].id, FRED[i].name, cnt);
        total += cnt;
    }
    printf("  Total: %d FRED data points\n", total);
    return total;
}

// ── 5. PredictIt markets ──
static int ingest_predictit(sqlite3 *db) {
    char *data = http_get("https://www.predictit.org/api/marketdata/all/");
    if (!data) { printf("[FAIL] PredictIt\n"); return 0; }
    int count = 0;
    json_error_t err;
    json_t *root = json_loads(data, 0, &err);
    free(data);
    if (!root || !json_is_array(root)) { json_decref(root); return 0; }
    long ts = time(0);
    size_t idx; json_t *val;
    json_array_foreach(root, idx, val) {
        char *json_s = json_dumps(val, JSON_COMPACT);
        if (json_s) {
            db_insert(db, ts, "predictit_market", "market_snapshot", json_s);
            free(json_s);
            count++;
        }
    }
    json_decref(root);
    printf("  %d PredictIt markets\n", count);
    return count;
}

// ── 6. ESPN sports scoreboards ──
static const char *SPORTS[][2] = {
    {"nfl","http://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard"},
    {"nba","http://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard"},
    {"mlb","http://site.api.espn.com/apis/site/v2/sports/baseball/mlb/scoreboard"},
    {"nhl","http://site.api.espn.com/apis/site/v2/sports/hockey/nhl/scoreboard"},
    {0,0}
};

static int ingest_sports(sqlite3 *db) {
    int total = 0;
    long ts = time(0);
    for (int i = 0; SPORTS[i][0]; i++) {
        char *data = http_get(SPORTS[i][1]);
        if (!data) continue;
        json_error_t err;
        json_t *root = json_loads(data, 0, &err);
        free(data);
        if (!root) continue;
        json_t *events = json_object_get(root, "events");
        if (events && json_is_array(events)) {
            size_t idx; json_t *ev;
            json_array_foreach(events, idx, ev) {
                if (idx >= 20) break;
                char buf[32];
                json_t *id = json_object_get(ev, "id");
                json_t *name = json_object_get(ev, "name");
                json_t *date = json_object_get(ev, "date");
                json_t *status = json_object_get(json_object_get(ev, "status"), "type");
                json_t *desc = status ? json_object_get(status, "description") : 0;
                char json[1024]; int p=0;
                p += snprintf(json+p, sizeof(json)-p, "{\"league\":\"%s\"", SPORTS[i][0]);
                if (id && json_is_string(id)) p += snprintf(json+p, sizeof(json)-p, ",\"id\":\"%s\"", json_string_value(id));
                if (name && json_is_string(name)) p += snprintf(json+p, sizeof(json)-p, ",\"name\":\"%s\"", json_string_value(name));
                if (date && json_is_string(date)) p += snprintf(json+p, sizeof(json)-p, ",\"date\":\"%s\"", json_string_value(date));
                if (desc && json_is_string(desc)) p += snprintf(json+p, sizeof(json)-p, ",\"status\":\"%s\"", json_string_value(desc));
                p += snprintf(json+p, sizeof(json)-p, "}");
                db_insert(db, ts, SPORTS[i][0], "sports_scoreboard", json);
                total++;
            }
        }
        json_decref(root);
    }
    printf("  %d sports events\n", total);
    return total;
}

int main(void) {
    printf("============================================================\n");
    printf("📦 DATASET INGESTOR v1.0 (C)\n");
    printf("   Target: %s\n", TIMELINE_DB);
    printf("============================================================\n");

    curl_global_init(CURL_GLOBAL_ALL);
    sqlite3 *db = db_open();
    if (!db) { fprintf(stderr, "Cannot open %s\n", TIMELINE_DB); return 1; }

    int total = 0;

    printf("\n─── 1. Polymarket Historical (manja316) ───\n");
    total += ingest_polymarket(db);

    printf("\n─── 2. Yahoo Finance (20 tickers, 3yr) ───\n");
    total += ingest_yfinance(db);

    printf("\n─── 3. SEC EDGAR Company Tickers ───\n");
    total += ingest_sec(db);

    printf("\n─── 4. FRED Macro Series (22 series) ───\n");
    total += ingest_fred(db);

    printf("\n─── 5. PredictIt Markets ───\n");
    total += ingest_predictit(db);

    printf("\n─── 6. Sports Scoreboards (ESPN public) ───\n");
    total += ingest_sports(db);

    sqlite3_exec(db, "COMMIT", 0,0,0);
    sqlite3_close(db);
    curl_global_cleanup();

    printf("\n============================================================\n");
    printf("✅ INGEST COMPLETE — %d rows written\n", total);
    printf("============================================================\n");
    return 0;
}
