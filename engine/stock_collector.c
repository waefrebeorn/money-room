/*
 * stock_collector.c — CB-STOCK: Stock Fundamentals & Technicals Collector
 *
 * Fetches stock fundamentals from Finnhub free API (300 req/day).
 * Provides: name, sector, industry, market cap, PE ratio, EPS, dividend, beta
 *
 * Features:
 *   fetch <ticker>    — Fetch and store fundamentals for one ticker
 *   fetch-all         — Fetch all tracked tickers
 *   info <ticker>     — Show stored fundamentals
 *   list              — List all tickers with latest data
 *   gaps              — Show tickers with stale/missing data (>24h)
 *
 * Build: gcc stock_collector.c -o stock_collector -lcurl -ljansson -lsqlite3 -lm -O2 -Wall
 * Env:  FINNHUB_API_KEY must be set in ~/.hermes/secrets.env
 * Data: ~/.hermes/stocks_cache/stocks.db
 *
 * Finnhub Free API (300 req/day):
 *   stock/profile2   — Company profile: name, sector, mcap, shares, ipo
 *   stock/metric     — Key metrics: PE, EPS, dividend, beta
 *   quote            — Current price, change, open, high, low
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>

#define DB_DIR     "/home/wubu2/.hermes/stocks_cache"
#define DB_PATH    DB_DIR "/stocks.db"
#define TIMEOUT    30L
#define STALE_HRS  24

/* ── HTTP buffer ── */
typedef struct { char *data; size_t len; } HttpBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    HttpBuf *b = (HttpBuf*)user;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    memcpy(p + b->len, ptr, n);
    b->data = p; b->len += n; b->data[b->len] = '\0';
    return n;
}

static char *fetch_url(const char *url) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    HttpBuf b = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, TIMEOUT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0");
    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (res != CURLE_OK) { free(b.data); return NULL; }
    return b.data;
}

/* ── SQLite ── */
static sqlite3 *open_db(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", DB_DIR);
    int _s = system(cmd); (void)_s;
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "DB: %s\n", sqlite3_errmsg(db));
        return NULL;
    }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS stock_profile ("
        "  ticker TEXT PRIMARY KEY,"
        "  name TEXT, sector TEXT, industry TEXT, country TEXT,"
        "  market_cap REAL, shares_outstanding REAL,"
        "  ipo_year TEXT, currency TEXT,"
        "  updated_at INTEGER"
        ");"
        "CREATE TABLE IF NOT EXISTS stock_metrics ("
        "  ticker TEXT PRIMARY KEY,"
        "  pe_ratio REAL, eps REAL, dividend_yield REAL,"
        "  dividend_rate REAL, beta REAL,"
        "  high_52w REAL, low_52w REAL, avg_volume REAL,"
        "  updated_at INTEGER"
        ");"
        "CREATE TABLE IF NOT EXISTS stock_quote ("
        "  ticker TEXT PRIMARY KEY,"
        "  price REAL, change_pct REAL, open REAL, high REAL, low REAL,"
        "  volume REAL, prev_close REAL,"
        "  updated_at INTEGER"
        ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Schema: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static int64_t now_ts(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

/* ── Tracked tickers ── */
static const char *TICKERS[] = {
    "SPY","QQQ","IWM","DIA","GLD","SLV","USO","TLT","IEF","EEM",
    "VNQ","XLF","XLK","XLE","XLV","XLI","XLB","XLU","XLP","XLY",
    "AAPL","MSFT","GOOGL","AMZN","NVDA","META","TSLA","JPM","V","JNJ",
    "WMT","PG","UNH","HD","DIS","NFLX","ADBE","CRM","INTC","AMD",
    "BAC","PFE","MRK","KO","PEP","XOM","CVX","BA","CAT","GE",
    NULL
};

static const char *get_key(void) {
    const char *k = getenv("FINNHUB_API_KEY");
    if (!k) fprintf(stderr, "Set FINNHUB_API_KEY in env\n");
    return k;
}

/* ── Fetch profile + metrics + quote for one ticker ── */
static int fetch_ticker(sqlite3 *db, const char *tkr) {
    const char *key = get_key();
    if (!key) return 0;

    int64_t ts = now_ts();

    // Fetch profile2
    char url[512];
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/stock/profile2?symbol=%s&token=%s", tkr, key);
    char *body = fetch_url(url);
    if (body) {
        json_t *j = json_loads(body, 0, NULL);
        free(body);
        if (j && json_object_get(j, "name") && !json_is_null(json_object_get(j, "name"))) {
            const char *name = json_string_value(json_object_get(j, "name"));
            const char *sector = json_string_value(json_object_get(j, "sector"));
            const char *industry = json_string_value(json_object_get(j, "industry"));
            const char *country = json_string_value(json_object_get(j, "country"));
            const char *currency = json_string_value(json_object_get(j, "currency"));
            const char *ipo = json_string_value(json_object_get(j, "ipo"));
            double mcap = json_number_value(json_object_get(j, "marketCapitalization"));
            double shares = json_number_value(json_object_get(j, "shareOutstanding"));

            sqlite3_stmt *st;
            sqlite3_prepare_v2(db,
                "INSERT OR REPLACE INTO stock_profile "
                "(ticker,name,sector,industry,country,market_cap,shares_outstanding,ipo_year,currency,updated_at) "
                "VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL);
            sqlite3_bind_text(st, 1, tkr, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, name ? name : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, sector ? sector : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 4, industry ? industry : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 5, country ? country : "", -1, SQLITE_STATIC);
            sqlite3_bind_double(st, 6, mcap);
            sqlite3_bind_double(st, 7, shares);
            sqlite3_bind_text(st, 8, ipo ? ipo : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 9, currency ? currency : "", -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 10, ts);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        if (j) json_decref(j);
    }

    // Fetch basic financials (PE, EPS, dividend, beta)
    // Note: Finnhub free tier stock/metric endpoint returns limited data.
    // Full metrics require Finnhub Pro ($). Using profile2 + quote as primary sources.
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/stock/metric?symbol=%s&metric=all&token=%s", tkr, key);
    body = fetch_url(url);
    if (body) {
        json_t *j = json_loads(body, 0, NULL);
        free(body);
        if (j) {
            json_t *m = json_object_get(j, "metric");
            if (m) {
                double pe = json_number_value(json_object_get(m, "peBasicExclExtraTTM"));
                double eps = json_number_value(json_object_get(m, "epsBasicExclExtraItemsTTM"));
                double div_y = json_number_value(json_object_get(m, "dividendYieldIndicatedAnnual"));
                double div_r = json_number_value(json_object_get(m, "dividendPerShareAnnual"));
                double beta = json_number_value(json_object_get(m, "beta"));

                // Validate: reject obviously bogus values
                if (pe < 0 || pe > 5000) pe = 0;
                if (eps < -5000 || eps > 5000) eps = 0;
                if (div_y < 0 || div_y > 5) div_y = 0;   // >500% yield is wrong
                if (beta < -5 || beta > 10) beta = 0;     // beta 315 is wrong
                double h52 = json_number_value(json_object_get(m, "52WeekHigh"));
                double l52 = json_number_value(json_object_get(m, "52WeekLow"));
                double avgv = json_number_value(json_object_get(m, "shrsOut"));

                sqlite3_stmt *st;
                sqlite3_prepare_v2(db,
                    "INSERT OR REPLACE INTO stock_metrics "
                    "(ticker,pe_ratio,eps,dividend_yield,dividend_rate,beta,high_52w,low_52w,avg_volume,updated_at) "
                    "VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL);
                sqlite3_bind_text(st, 1, tkr, -1, SQLITE_STATIC);
                sqlite3_bind_double(st, 2, pe);
                sqlite3_bind_double(st, 3, eps);
                sqlite3_bind_double(st, 4, div_y);
                sqlite3_bind_double(st, 5, div_r);
                sqlite3_bind_double(st, 6, beta);
                sqlite3_bind_double(st, 7, h52);
                sqlite3_bind_double(st, 8, l52);
                sqlite3_bind_double(st, 9, avgv);
                sqlite3_bind_int64(st, 10, ts);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            json_decref(j);
        }
    }

    // Fetch quote (current price, change)
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/quote?symbol=%s&token=%s", tkr, key);
    body = fetch_url(url);
    if (body) {
        json_t *j = json_loads(body, 0, NULL);
        free(body);
        if (j) {
            double price = json_number_value(json_object_get(j, "c"));
            double chg = json_number_value(json_object_get(j, "dp"));
            double open = json_number_value(json_object_get(j, "o"));
            double high = json_number_value(json_object_get(j, "h"));
            double low = json_number_value(json_object_get(j, "l"));
            double vol = json_number_value(json_object_get(j, "v"));
            double prev = json_number_value(json_object_get(j, "pc"));

            sqlite3_stmt *st;
            sqlite3_prepare_v2(db,
                "INSERT OR REPLACE INTO stock_quote "
                "(ticker,price,change_pct,open,high,low,volume,prev_close,updated_at) "
                "VALUES (?,?,?,?,?,?,?,?,?)", -1, &st, NULL);
            sqlite3_bind_text(st, 1, tkr, -1, SQLITE_STATIC);
            sqlite3_bind_double(st, 2, price);
            sqlite3_bind_double(st, 3, chg);
            sqlite3_bind_double(st, 4, open);
            sqlite3_bind_double(st, 5, high);
            sqlite3_bind_double(st, 6, low);
            sqlite3_bind_double(st, 7, vol);
            sqlite3_bind_double(st, 8, prev);
            sqlite3_bind_int64(st, 9, ts);
            sqlite3_step(st);
            sqlite3_finalize(st);
            json_decref(j);
        }
    }

    printf("  %s: done\n", tkr);
    return 1;
}

/* ── Show stored info for a ticker ── */
static int cmd_info(sqlite3 *db, const char *tkr) {
    sqlite3_stmt *st;
    const char *sql = "SELECT p.*,m.pe_ratio,m.eps,m.dividend_yield,m.beta,m.high_52w,m.low_52w,"
        "q.price,q.change_pct,q.volume "
        "FROM stock_profile p "
        "LEFT JOIN stock_metrics m ON p.ticker=m.ticker "
        "LEFT JOIN stock_quote q ON p.ticker=q.ticker "
        "WHERE p.ticker=?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, tkr, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_ROW) {
        printf("No data for %s. Run 'fetch' first.\n", tkr);
        sqlite3_finalize(st);
        return 0;
    }
    printf("=== %s ===\n", sqlite3_column_text(st, 0));
    printf("Name:     %s\n", sqlite3_column_text(st, 1));
    printf("Sector:   %s\n", sqlite3_column_text(st, 2));
    printf("Industry: %s\n", sqlite3_column_text(st, 3));
    printf("Country:  %s\n", sqlite3_column_text(st, 4));
    printf("Mkt Cap:  $%.0fM\n", sqlite3_column_double(st, 5));
    printf("Shares:   %.0f\n", sqlite3_column_double(st, 6) * 1000000);
    printf("IPO:      %s\n", sqlite3_column_text(st, 7));
    printf("Currency: %s\n", sqlite3_column_text(st, 8));
    double price = sqlite3_column_double(st, 16);
    double pct = sqlite3_column_double(st, 17);
    printf("Price:    $%.2f (%.2f%%)\n", price, pct);
    printf("PE:       %.2f\n", sqlite3_column_double(st, 10));
    printf("EPS:      $%.2f\n", sqlite3_column_double(st, 11));
    printf("Div Yld:  %.2f%%\n", sqlite3_column_double(st, 12));
    printf("Beta:     %.2f\n", sqlite3_column_double(st, 13));
    printf("52W Hi:   %.2f  Lo: %.2f\n", sqlite3_column_double(st, 14), sqlite3_column_double(st, 15));
    int64_t ts = sqlite3_column_int64(st, 9);
    printf("Updated:  %dh ago\n", (int)((now_ts() - ts) / 3600));
    sqlite3_finalize(st);
    return 1;
}

/* ── List all ── */
static int cmd_list(sqlite3 *db) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT p.ticker,p.name,q.price,m.pe_ratio,p.market_cap,"
        "m.dividend_yield,m.beta,m.high_52w,p.updated_at "
        "FROM stock_profile p "
        "LEFT JOIN stock_quote q ON p.ticker=q.ticker "
        "LEFT JOIN stock_metrics m ON p.ticker=m.ticker "
        "ORDER BY p.ticker", -1, &st, NULL) != SQLITE_OK) return 0;
    printf("%-8s %-20s %8s %6s %12s %5s %5s %8s %s\n",
        "Ticker","Name","Price","PE","MktCap","Div%","Beta","52WHi","Age");
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t ts = sqlite3_column_int64(st, 8);
        printf("%-8s %-20.20s %7.2f %6.1f %11.0f %4.2f%% %4.2f %7.2f %dh\n",
            sqlite3_column_text(st, 0),
            sqlite3_column_text(st, 1),
            sqlite3_column_double(st, 2),
            sqlite3_column_double(st, 3),
            sqlite3_column_double(st, 4),
            sqlite3_column_double(st, 5),
            sqlite3_column_double(st, 6),
            sqlite3_column_double(st, 7),
            (int)((now_ts() - ts) / 3600));
    }
    sqlite3_finalize(st);
    return 1;
}

/* ── Show stale ── */
static int cmd_gaps(sqlite3 *db) {
    int64_t cutoff = now_ts() - (STALE_HRS * 3600);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT ticker,updated_at FROM stock_profile WHERE updated_at < ?", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, cutoff);
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int h = (int)((now_ts() - sqlite3_column_int64(st, 1)) / 3600);
        printf("  %s: %dh ago\n", sqlite3_column_text(st, 0), h);
        n++;
    }
    sqlite3_finalize(st);
    printf("%d stale tickers\n", n);
    return 1;
}

/* ── Main ── */
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s fetch <ticker>     — Fetch one ticker\n", argv[0]);
        printf("  %s fetch-all          — Fetch all %zu tickers\n", argv[0], sizeof(TICKERS)/sizeof(TICKERS[0])-1);
        printf("  %s info <ticker>      — Show fundamentals\n", argv[0]);
        printf("  %s list               — List all\n", argv[0]);
        printf("  %s gaps               — Show stale\n", argv[0]);
        return 1;
    }

    sqlite3 *db = open_db();
    if (!db) return 1;

    int ret = 0;
    if (strcmp(argv[1], "fetch") == 0 && argc >= 3) {
        ret = fetch_ticker(db, argv[2]);
    } else if (strcmp(argv[1], "fetch-all") == 0) {
        int ok = 0, fail = 0, n = 0;
        while (TICKERS[n]) n++;
        for (int i = 0; TICKERS[i]; i++) {
            printf("[%d/%d] %s ... ", i+1, n, TICKERS[i]);
            if (fetch_ticker(db, TICKERS[i])) ok++; else fail++;
            struct timespec ts = {0, 100000000L}; // 100ms rate limit
            nanosleep(&ts, NULL);
        }
        printf("Done: %d ok, %d failed\n", ok, fail);
    } else if (strcmp(argv[1], "info") == 0 && argc >= 3) {
        ret = cmd_info(db, argv[2]);
    } else if (strcmp(argv[1], "list") == 0) {
        ret = cmd_list(db);
    } else if (strcmp(argv[1], "gaps") == 0) {
        ret = cmd_gaps(db);
    } else {
        printf("Unknown: %s\n", argv[1]);
        ret = 1;
    }

    sqlite3_close(db);
    return ret ? 0 : 1;
}
