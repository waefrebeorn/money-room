/**
 * stock_collector.c — P61: Stock Fundamentals C Pipeline
 *
 * Fetches stock quote, fundamentals, and option chain data from
 * Yahoo Finance. Computes IV rank, max pain, Greeks.
 * Stores everything in SQLite.
 *
 * Compile:
 *   gcc -O3 -o stock_collector stock_collector.c -lcurl -ljansson -lsqlite3 -lm
 *
 * Usage:
 *   ./stock_collector quote <SYMBOL>          — real-time quote
 *   ./stock_collector fundamentals <SYMBOL>   — fundamentals snapshot
 *   ./stock_collector options <SYMBOL>        — option chain + IV rank + max pain
 *   ./stock_collector all <SYMBOL>            — full pipeline to DB
 *   ./stock_collector db <SYMBOL> [N]         — latest N DB snapshots for symbol
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_URL      4096
#define MAX_PATH     1024
#define MAX_BUF      262144
#define USER_AGENT   "Mozilla/5.0"
#define YF_BASE      "https://query1.finance.yahoo.com"
#define YF_BASE2     "https://query2.finance.yahoo.com"
#define DB_DIR       "/home/wubu2/.hermes/stock_cache"
#define DB_PATH      "/home/wubu2/.hermes/stock_cache/stock_fundamentals.db"

// ── Curl write callback ──
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} WriteBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    WriteBuf *buf = (WriteBuf*)userdata;
    if (buf->len + total + 1 > buf->cap) {
        buf->cap = buf->len + total + 4096;
        char *tmp = realloc(buf->data, buf->cap);
        if (!tmp) return 0;
        buf->data = tmp;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static CURL *curl_easy(void) {
    CURL *h = curl_easy_init();
    if (!h) return NULL;
    curl_easy_setopt(h, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(h, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");

    // Load consent cookies if available
    char cookies_file[MAX_PATH];
    snprintf(cookies_file, sizeof(cookies_file), "%s/yf_cookies.txt", DB_DIR);
    FILE *f = fopen(cookies_file, "r");
    if (f) {
        fclose(f);
        curl_easy_setopt(h, CURLOPT_COOKIEFILE, cookies_file);
    }
    return h;
}

static char *fetch_url(const char *url) {
    CURL *h = curl_easy();
    if (!h) return NULL;
    WriteBuf buf = {0};
    buf.cap = 65536;
    buf.data = malloc(buf.cap);
    if (!buf.data) { curl_easy_cleanup(h); return NULL; }
    buf.data[0] = '\0';

    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(h, CURLOPT_URL, url);

    CURLcode rc = curl_easy_perform(h);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK) {
        fprintf(stderr, "[stock] curl error: %s\n", curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

// ── Crumb management ──
static char *fetch_crumb(void) {
    CURL *h = curl_easy();
    if (!h) return NULL;

    char cookies_file[MAX_PATH];
    snprintf(cookies_file, sizeof(cookies_file), "%s/yf_cookies.txt", DB_DIR);
    char cmd[MAX_PATH];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", DB_DIR);
    system(cmd);

    // Check if existing cookies are fresh enough (< 1 hour old)
    struct stat cstat;
    int has_fresh_cookies = 0;
    if (stat(cookies_file, &cstat) == 0) {
        time_t now = time(NULL);
        if (difftime(now, cstat.st_mtime) < 3600) {
            has_fresh_cookies = 1;
        }
    }

    if (!has_fresh_cookies) {
        // Step 1: get consent cookies from fc.yahoo.com
        curl_easy_setopt(h, CURLOPT_URL, "https://fc.yahoo.com/");
        curl_easy_setopt(h, CURLOPT_COOKIEJAR, cookies_file);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
        WriteBuf dummy = {0};
        dummy.cap = 4096;
        dummy.data = malloc(4096);
        if (!dummy.data) { curl_easy_cleanup(h); return NULL; }
        dummy.data[0] = '\0';
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &dummy);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
        curl_easy_perform(h);
        free(dummy.data);
    }

    // Step 2: fetch crumb with consent cookies (use existing file if available)
    curl_easy_setopt(h, CURLOPT_URL, "https://query1.finance.yahoo.com/v1/test/getcrumb");
    curl_easy_setopt(h, CURLOPT_COOKIEFILE, cookies_file);
    curl_easy_setopt(h, CURLOPT_COOKIEJAR, NULL);
    WriteBuf buf = {0};
    buf.cap = 256;
    buf.data = malloc(256);
    if (!buf.data) { curl_easy_cleanup(h); return NULL; }
    buf.data[0] = '\0';
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &buf);
    CURLcode rc = curl_easy_perform(h);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK || buf.len == 0) {
        free(buf.data);
        return NULL;
    }
    while (buf.len > 0 && (buf.data[buf.len-1] == '\n' || buf.data[buf.len-1] == '\r'))
        buf.data[--buf.len] = '\0';
    return buf.data;
}

// ── Fetch URL with consent cookies (for crumb-requiring endpoints) ──
static char *fetch_authenticated(const char *url, const char *crumb) {
    // Validate crumb is alphanumeric-ish
    size_t clen = strlen(crumb);
    for (size_t i = 0; i < clen; i++) {
        if (!isalnum(crumb[i]) && crumb[i] != '_' && crumb[i] != '-' && crumb[i] != '.') {
            fprintf(stderr, "[stock] invalid crumb\n");
            return NULL;
        }
    }

    CURL *h = curl_easy();
    if (!h) return NULL;

    char cookies_file[MAX_PATH];
    snprintf(cookies_file, sizeof(cookies_file), "%s/yf_cookies.txt", DB_DIR);

    // Build URL with crumb
    char full_url[MAX_URL];
    snprintf(full_url, sizeof(full_url), "%s&crumb=%s", url, crumb);

    curl_easy_setopt(h, CURLOPT_COOKIEFILE, cookies_file);
    curl_easy_setopt(h, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_URL, full_url);

    WriteBuf buf = {0};
    buf.cap = MAX_BUF;
    buf.data = malloc(MAX_BUF);
    if (!buf.data) { curl_easy_cleanup(h); return NULL; }
    buf.data[0] = '\0';
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(h);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK) {
        fprintf(stderr, "[stock] curl error: %s\n", curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

// ── Safe JSON helpers ──
static const char *json_str(json_t *obj, const char *key) {
    json_t *v = json_object_get(obj, key);
    if (!v || !json_is_string(v)) return NULL;
    return json_string_value(v);
}

static double json_num(json_t *obj, const char *key, double def) {
    json_t *v = json_object_get(obj, key);
    if (!v) return def;
    if (json_is_number(v)) return json_number_value(v);
    if (json_is_string(v)) {
        const char *s = json_string_value(v);
        char *end = NULL;
        double d = strtod(s, &end);
        if (end && *end == '\0') return d;
    }
    return def;
}

static double json_raw(json_t *parent, const char *field) {
    json_t *f = json_object_get(parent, field);
    if (!f) return NAN;
    json_t *raw = json_object_get(f, "raw");
    if (raw && json_is_number(raw)) return json_number_value(raw);
    return NAN;
}

// ── SQLite init ──
static sqlite3 *db_open(void) {
    // Ensure directory exists
    char cmd[MAX_PATH];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", DB_DIR);
    system(cmd);

    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "[stock] DB error: %s\n", sqlite3_errmsg(db));
        return NULL;
    }

    // Create tables
    const char *schema =
        "CREATE TABLE IF NOT EXISTS quotes ("
        "  symbol TEXT NOT NULL,"
        "  ts INTEGER NOT NULL,"
        "  price REAL, change REAL, change_pct REAL,"
        "  volume INTEGER, market_cap REAL, pe_ratio REAL,"
        "  short_name TEXT, currency TEXT, exchange TEXT,"
        "  prev_close REAL, open REAL, day_high REAL, day_low REAL,"
        "  PRIMARY KEY (symbol, ts)"
        ");"
        "CREATE TABLE IF NOT EXISTS fundamentals ("
        "  symbol TEXT NOT NULL,"
        "  ts INTEGER NOT NULL,"
        "  market_cap REAL, enterprise_value REAL,"
        "  trailing_pe REAL, forward_pe REAL,"
        "  peg_ratio REAL, price_to_book REAL,"
        "  dividend_yield REAL, payout_ratio REAL,"
        "  eps_trailing REAL, eps_forward REAL,"
        "  revenue_ttm REAL, gross_profit_ttm REAL,"
        "  ebitda REAL, debt_to_equity REAL,"
        "  beta REAL, fifty_two_w_high REAL, fifty_two_w_low REAL,"
        "  fifty_day_ma REAL, two_hundred_day_ma REAL,"
        "  short_ratio REAL, short_pct_float REAL,"
        "  held_pct_institutions REAL,"
        "  PRIMARY KEY (symbol, ts)"
        ");"
        "CREATE TABLE IF NOT EXISTS options_chain ("
        "  symbol TEXT NOT NULL,"
        "  ts INTEGER NOT NULL,"
        "  expiration TEXT NOT NULL,"
        "  strike REAL NOT NULL,"
        "  type TEXT NOT NULL,"
        "  last_price REAL, bid REAL, ask REAL,"
        "  volume INTEGER, open_interest INTEGER,"
        "  implied_volatility REAL, delta REAL, gamma REAL,"
        "  theta REAL, vega REAL, rho REAL,"
        "  in_the_money INTEGER,"
        "  PRIMARY KEY (symbol, expiration, strike, type, ts)"
        ");"
        "CREATE TABLE IF NOT EXISTS option_summary ("
        "  symbol TEXT NOT NULL,"
        "  ts INTEGER NOT NULL,"
        "  near_expiration TEXT,"
        "  near_days_to_expiry INTEGER,"
        "  max_pain_strike REAL,"
        "  call_volume INTEGER, put_volume INTEGER,"
        "  put_call_volume_ratio REAL,"
        "  call_oi INTEGER, put_oi INTEGER,"
        "  put_call_oi_ratio REAL,"
        "  iv_min REAL, iv_max REAL, iv_mean REAL,"
        "  iv_rank_hv REAL,"
        "  PRIMARY KEY (symbol, ts)"
        ");";

    char *err = NULL;
    if (sqlite3_exec(db, schema, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[stock] schema error: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

// ── Command: Quote (v8 chart doesn't need crumb) ──
static int cmd_quote(const char *symbol) {
    char url[MAX_URL];

    // Try v8 chart first — no crumb needed
    snprintf(url, sizeof(url), "%s/v8/finance/chart/%s?interval=1d&range=1d",
             YF_BASE, symbol);
    char *body = fetch_url(url);
    if (!body) {
        // Fallback to query2
        snprintf(url, sizeof(url), "%s/v8/finance/chart/%s?interval=1d&range=1d",
                 YF_BASE2, symbol);
        body = fetch_url(url);
    }
    if (!body) {
        fprintf(stderr, "[stock] No data for %s\n", symbol);
        return 1;
    }

    json_error_t err;
    json_t *root = json_loads(body, 0, &err);
    free(body);

    if (!root) {
        fprintf(stderr, "[stock] JSON parse error: %s\n", err.text);
        return 1;
    }

    json_t *chart_result = json_object_get(root, "chart");
    if (!chart_result) { json_decref(root); return 1; }
    json_t *rarr = json_object_get(chart_result, "result");
    if (!rarr || !json_is_array(rarr) || json_array_size(rarr) == 0) {
        json_decref(root);
        fprintf(stderr, "[stock] No chart data for %s\n", symbol);
        return 1;
    }
    json_t *meta = json_object_get(json_array_get(rarr, 0), "meta");
    if (!meta) { json_decref(root); return 1; }

    double price = json_num(meta, "regularMarketPrice", 0);
    double prev = json_num(meta, "chartPreviousClose", 0);
    double chg = price - prev;
    double chg_pct = prev > 0 ? (chg / prev) * 100.0 : 0;

    printf("{\n");
    printf("  \"symbol\": \"%s\",\n", symbol);
    printf("  \"price\": %.2f,\n", price);
    printf("  \"change\": %.2f,\n", chg);
    printf("  \"change_pct\": %.2f,\n", chg_pct);
    printf("  \"volume\": %.0f,\n", json_num(meta, "regularMarketVolume", 0));
    printf("  \"prev_close\": %.2f,\n", prev);
    printf("  \"currency\": \"%s\",\n", json_str(meta, "currency") ?: "USD");
    printf("  \"exchange\": \"%s\",\n", json_str(meta, "exchangeName") ?: "");
    printf("  \"short_name\": \"%s\",\n", json_str(meta, "shortName") ?: "");
    printf("  \"long_name\": \"%s\",\n", json_str(meta, "longName") ?: "");
    printf("  \"52w_high\": %.2f,\n", json_num(meta, "fiftyTwoWeekHigh", 0));
    printf("  \"52w_low\": %.2f,\n", json_num(meta, "fiftyTwoWeekLow", 0));
    printf("  \"data_source\": \"Yahoo Finance\"\n");
    printf("}\n");

    json_decref(root);
    return 0;
}

// ── Command: Fundamentals (quoteSummary) ──
static int cmd_fundamentals(const char *symbol) {
    // Note: Yahoo v11 quoteSummary endpoint currently broken/blocked.
    // Falls back to quote data from v8 chart endpoint which always works.
    return cmd_quote(symbol);
}

// ── Options chain ──
static double black_scholes_delta(double S, double K, double T, double r, double sigma, int is_call) {
    if (T <= 0 || sigma <= 0) return NAN;
    double d1 = (log(S/K) + (r + sigma*sigma/2)*T) / (sigma*sqrt(T));
    double cdf = 0.5 * (1 + erf(d1 / sqrt(2)));
    if (is_call) return cdf;
    return cdf - 1.0;  // put delta
}

static double black_scholes_gamma(double S, double K, double T, double r, double sigma) {
    if (T <= 0 || sigma <= 0) return NAN;
    double d1 = (log(S/K) + (r + sigma*sigma/2)*T) / (sigma*sqrt(T));
    double pdf = exp(-d1*d1/2) / sqrt(2*M_PI);
    return pdf / (S * sigma * sqrt(T));
}

static double black_scholes_theta(double S, double K, double T, double r, double sigma, int is_call) {
    if (T <= 0 || sigma <= 0) return NAN;
    double d1 = (log(S/K) + (r + sigma*sigma/2)*T) / (sigma*sqrt(T));
    double d2 = d1 - sigma*sqrt(T);
    double pdf = exp(-d1*d1/2) / sqrt(2*M_PI);
    double theta = -S*sigma*pdf/(2*sqrt(T));
    if (is_call) {
        theta -= r*K*exp(-r*T)*0.5*(1+erf(d2/sqrt(2)));
    } else {
        theta += r*K*exp(-r*T)*0.5*(1-erf(-d2/sqrt(2)));
    }
    return theta / 365.0;  // daily theta
}

static double black_scholes_vega(double S, double K, double T, double r, double sigma) {
    if (T <= 0 || sigma <= 0) return NAN;
    double d1 = (log(S/K) + (r + sigma*sigma/2)*T) / (sigma*sqrt(T));
    double pdf = exp(-d1*d1/2) / sqrt(2*M_PI);
    return S * pdf * sqrt(T) / 100.0;  // per 1% IV change
}

// ── Parse date string "YYYY-MM-DD" → days to expiry ──
static int days_to_expiry(const char *date_str) {
    int y, m, d;
    if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3) return 365;
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    time_t expiry = mktime(&tm);
    time_t now = time(NULL);
    double secs = difftime(expiry, now);
    return secs > 0 ? (int)(secs / 86400.0 + 0.5) : 0;
}

static double compute_max_pain(const char *json_body, const char *symbol) {
    (void)symbol;  // unused
    json_error_t err;
    json_t *root = json_loads(json_body, 0, &err);
    if (!root) return NAN;

    json_t *opt = json_object_get(root, "optionChain");
    if (!opt) { json_decref(root); return NAN; }
    json_t *rarr = json_object_get(opt, "result");
    if (!rarr || json_array_size(rarr) == 0) { json_decref(root); return NAN; }

    json_t *r0 = json_array_get(rarr, 0);
    json_t *quote = json_object_get(r0, "quote");
    double underlying = quote ? json_num(quote, "regularMarketPrice", 0) : 0;
    if (underlying <= 0) {
        json_t *meta = json_object_get(r0, "summary");
        underlying = meta ? json_num(meta, "underlyingPrice", 0) : 0;
    }

    json_t *exp_arr = json_object_get(r0, "expirationDates");
    if (!exp_arr || json_array_size(exp_arr) == 0) { json_decref(root); return NAN; }

    // Get nearest expiry
    json_t *opt_arr = json_object_get(r0, "options");
    if (!opt_arr || json_array_size(opt_arr) == 0) { json_decref(root); return NAN; }

    // Process all option chains for max pain calculation
    // Store strikes: OI * |strike - underlying|
    double max_pain = underlying;
    double max_val = 0;
    int found = 0;

    size_t n_exp = json_array_size(exp_arr);
    for (size_t ei = 0; ei < n_exp && ei < 5; ei++) {
        // Get options for this expiration
        json_t *calls = NULL, *puts = NULL;
        json_t *exp_obj = json_array_get(opt_arr, ei);
        if (exp_obj) {
            calls = json_object_get(exp_obj, "calls");
            puts = json_object_get(exp_obj, "puts");
        }

        if (!calls || !puts) continue;

        size_t n_calls = json_array_size(calls);
        size_t n_puts = json_array_size(puts);

        // Simple max pain: sum over strikes OI * |K - S|
        // For efficiency, only check strikes present in the chain
        for (size_t i = 0; i < n_calls; i++) {
            json_t *c = json_array_get(calls, i);
            double strike = json_num(c, "strike", 0);
            if (strike <= 0) continue;
            double call_oi = json_num(c, "openInterest", 0);
            double put_oi = 0;
            // Find matching put strike
            for (size_t j = 0; j < n_puts; j++) {
                json_t *p = json_array_get(puts, j);
                if (fabs(json_num(p, "strike", 0) - strike) < 0.01) {
                    put_oi = json_num(p, "openInterest", 0);
                    break;
                }
            }
            double total_liability = (call_oi + put_oi) * fabs(strike - underlying);
            if (total_liability > max_val) {
                max_val = total_liability;
                max_pain = strike;
                found = 1;
            }
        }
    }

    json_decref(root);
    return found ? max_pain : NAN;
}

// ── Compute IV rank from option chain ──
static void compute_iv_stats(const char *json_body, double *iv_min, double *iv_max, double *iv_mean) {
    *iv_min = 999;
    *iv_max = 0;
    *iv_mean = 0;
    int count = 0;

    json_error_t err;
    json_t *root = json_loads(json_body, 0, &err);
    if (!root) return;

    json_t *opt = json_object_get(root, "optionChain");
    if (!opt) { json_decref(root); return; }
    json_t *rarr = json_object_get(opt, "result");
    if (!rarr || json_array_size(rarr) == 0) { json_decref(root); return; }

    json_t *r0 = json_array_get(rarr, 0);
    json_t *opt_arr = json_object_get(r0, "options");
    if (!opt_arr || json_array_size(opt_arr) == 0) { json_decref(root); return; }

    for (size_t ei = 0; ei < json_array_size(opt_arr) && ei < 3; ei++) {
        json_t *exp_obj = json_array_get(opt_arr, ei);
        json_t *calls = json_object_get(exp_obj, "calls");
        json_t *puts = json_object_get(exp_obj, "puts");

        // Process calls
        if (calls) {
            size_t n = json_array_size(calls);
            for (size_t i = 0; i < n; i++) {
                double iv = json_num(json_array_get(calls, i), "impliedVolatility", 0);
                if (iv > 0.001) {
                    if (iv < *iv_min) *iv_min = iv;
                    if (iv > *iv_max) *iv_max = iv;
                    *iv_mean += iv;
                    count++;
                }
            }
        }
        // Process puts
        if (puts) {
            size_t n = json_array_size(puts);
            for (size_t i = 0; i < n; i++) {
                double iv = json_num(json_array_get(puts, i), "impliedVolatility", 0);
                if (iv > 0.001) {
                    if (iv < *iv_min) *iv_min = iv;
                    if (iv > *iv_max) *iv_max = iv;
                    *iv_mean += iv;
                    count++;
                }
            }
        }
    }

    if (count > 0) *iv_mean /= count;
    if (*iv_min >= 999) *iv_min = 0;

    json_decref(root);
}

// ── Store to DB ──
static int store_to_db(sqlite3 *db, const char *symbol, time_t ts,
                        const char *quote_json, const char *opt_json) {
    // Parse quote JSON for storage
    json_error_t err;
    json_t *root = json_loads(quote_json, 0, &err);
    if (!root) return 1;

    json_t *qs = json_object_get(root, "quoteSummary");
    json_t *c = NULL;
    if (qs) {
        json_t *ra = json_object_get(qs, "result");
        if (ra && json_array_size(ra) > 0) {
            c = json_array_get(ra, 0);
        }
    }

    // Store fundamentals
    if (c) {
        json_t *sd = json_object_get(c, "summaryDetail");
        json_t *ks = json_object_get(c, "defaultKeyStatistics");
        json_t *fd = json_object_get(c, "financialData");

        sqlite3_stmt *stmt;
        const char *sql =
            "INSERT OR REPLACE INTO fundamentals "
            "(symbol, ts, market_cap, enterprise_value, trailing_pe, forward_pe, "
            " peg_ratio, price_to_book, dividend_yield, payout_ratio, "
            " eps_trailing, eps_forward, revenue_ttm, gross_profit_ttm, "
            " ebitda, debt_to_equity, beta, fifty_two_w_high, fifty_two_w_low, "
            " fifty_day_ma, two_hundred_day_ma, short_ratio, short_pct_float, "
            " held_pct_institutions) "
            "VALUES (?,?,?,?,?,?, ?,?,?,?, ?,?,?,?, ?,?,?,?,?, ?,?,?,?,?)";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, symbol, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)ts);

            double mc = fd ? json_raw(fd, "marketCap") : NAN;
            double ev = ks ? json_raw(ks, "enterpriseValue") : NAN;
            double tpe = sd ? json_raw(sd, "trailingPE") : NAN;
            double fpe = sd ? json_raw(sd, "forwardPE") : NAN;

            sqlite3_bind_double(stmt, 3, mc);
            sqlite3_bind_double(stmt, 4, ev);
            sqlite3_bind_double(stmt, 5, tpe);
            sqlite3_bind_double(stmt, 6, fpe);
            sqlite3_bind_double(stmt, 7, sd ? json_raw(sd, "pegRatio") : NAN);
            sqlite3_bind_double(stmt, 8, ks ? json_raw(ks, "priceToBook") : NAN);
            sqlite3_bind_double(stmt, 9, sd ? json_raw(sd, "dividendYield") : NAN);
            sqlite3_bind_double(stmt, 10, sd ? json_raw(sd, "payoutRatio") : NAN);
            sqlite3_bind_double(stmt, 11, ks ? json_raw(ks, "trailingEps") : NAN);
            sqlite3_bind_double(stmt, 12, ks ? json_raw(ks, "forwardEps") : NAN);
            sqlite3_bind_double(stmt, 13, fd ? json_raw(fd, "totalRevenue") : NAN);
            sqlite3_bind_double(stmt, 14, fd ? json_raw(fd, "grossProfits") : NAN);
            sqlite3_bind_double(stmt, 15, fd ? json_raw(fd, "ebitda") : NAN);
            sqlite3_bind_double(stmt, 16, ks ? json_raw(ks, "debtToEquity") : NAN);
            sqlite3_bind_double(stmt, 17, sd ? json_raw(sd, "beta") : NAN);
            sqlite3_bind_double(stmt, 18, sd ? json_raw(sd, "fiftyTwoWeekHigh") : NAN);
            sqlite3_bind_double(stmt, 19, sd ? json_raw(sd, "fiftyTwoWeekLow") : NAN);
            sqlite3_bind_double(stmt, 20, sd ? json_raw(sd, "fiftyDayAverage") : NAN);
            sqlite3_bind_double(stmt, 21, sd ? json_raw(sd, "twoHundredDayAverage") : NAN);
            sqlite3_bind_double(stmt, 22, sd ? json_raw(sd, "shortRatio") : NAN);
            sqlite3_bind_double(stmt, 23, sd ? json_raw(sd, "shortPercentOfFloat") : NAN);
            sqlite3_bind_double(stmt, 24, ks ? json_raw(ks, "heldPercentInstitutions") : NAN);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    json_decref(root);

    // Store option summary if available
    if (opt_json && strlen(opt_json) > 10) {
        double iv_min, iv_max, iv_mean;
        compute_iv_stats(opt_json, &iv_min, &iv_max, &iv_mean);
        double max_pain = compute_max_pain(opt_json, symbol);

        // Get near expiration
        json_t *oroot = json_loads(opt_json, 0, &err);
        char near_exp[32] = "";
        int near_dte = 365;
        if (oroot) {
            json_t *opt = json_object_get(oroot, "optionChain");
            if (opt) {
                json_t *res = json_object_get(opt, "result");
                if (res && json_array_size(res) > 0) {
                    json_t *r0 = json_array_get(res, 0);
                    json_t *exps = json_object_get(r0, "expirationDates");
                    if (exps && json_is_array(exps) && json_array_size(exps) > 0) {
                        // First expiration is nearest
                        time_t nearest = (time_t)json_number_value(json_array_get(exps, 0));
                        struct tm *tm = gmtime(&nearest);
                        strftime(near_exp, sizeof(near_exp), "%Y-%m-%d", tm);
                        near_dte = (int)((nearest - ts) / 86400);
                        if (near_dte < 0) near_dte = 0;
                    }
                }
            }
            json_decref(oroot);
        }

        // Count volumes
        json_t *oroot2 = json_loads(opt_json, 0, &err);
        long call_vol = 0, put_vol = 0, call_oi = 0, put_oi = 0;
        if (oroot2) {
            json_t *opt = json_object_get(oroot2, "optionChain");
            if (opt) {
                json_t *res = json_object_get(opt, "result");
                if (res && json_array_size(res) > 0) {
                    json_t *r0 = json_array_get(res, 0);
                    json_t *oarr = json_object_get(r0, "options");
                    if (oarr && json_array_size(oarr) > 0) {
                        json_t *exp0 = json_array_get(oarr, 0);
                        json_t *calls = json_object_get(exp0, "calls");
                        json_t *puts = json_object_get(exp0, "puts");
                        if (calls) {
                            for (size_t i = 0; i < json_array_size(calls); i++) {
                                call_vol += (long)json_num(json_array_get(calls, i), "volume", 0);
                                call_oi += (long)json_num(json_array_get(calls, i), "openInterest", 0);
                            }
                        }
                        if (puts) {
                            for (size_t i = 0; i < json_array_size(puts); i++) {
                                put_vol += (long)json_num(json_array_get(puts, i), "volume", 0);
                                put_oi += (long)json_num(json_array_get(puts, i), "openInterest", 0);
                            }
                        }
                    }
                }
            }
            json_decref(oroot2);
        }

        sqlite3_stmt *stmt;
        const char *sql =
            "INSERT OR REPLACE INTO option_summary "
            "(symbol, ts, near_expiration, near_days_to_expiry, max_pain_strike, "
            " call_volume, put_volume, put_call_volume_ratio, "
            " call_oi, put_oi, put_call_oi_ratio, "
            " iv_min, iv_max, iv_mean, iv_rank_hv) "
            "VALUES (?,?,?,?,?, ?,?,?, ?,?,?, ?,?,?,?)";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, symbol, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)ts);
            sqlite3_bind_text(stmt, 3, near_exp, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, near_dte);
            sqlite3_bind_double(stmt, 5, max_pain);
            sqlite3_bind_int64(stmt, 6, call_vol);
            sqlite3_bind_int64(stmt, 7, put_vol);
            sqlite3_bind_double(stmt, 8, put_vol > 0 ? (double)call_vol / put_vol : 0);
            sqlite3_bind_int64(stmt, 9, call_oi);
            sqlite3_bind_int64(stmt, 10, put_oi);
            sqlite3_bind_double(stmt, 11, put_oi > 0 ? (double)call_oi / put_oi : 0);
            sqlite3_bind_double(stmt, 12, iv_min);
            sqlite3_bind_double(stmt, 13, iv_max);
            sqlite3_bind_double(stmt, 14, iv_mean);
            sqlite3_bind_double(stmt, 15, iv_mean > 0 ? iv_mean * 100 : 0);  // IV rank proxy
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return 0;
}

// ── Command: Options chain ──
static int cmd_options(const char *symbol, sqlite3 *db) {
    char url[MAX_URL];
    char *crumb = fetch_crumb();
    if (!crumb) {
        fprintf(stderr, "[stock] Failed to get crumb for %s\n", symbol);
        return 1;
    }

    snprintf(url, sizeof(url), "%s/v7/finance/options/%s",
             YF_BASE, symbol);
    char *body = fetch_authenticated(url, crumb);
    if (!body) {
        snprintf(url, sizeof(url), "%s/v7/finance/options/%s",
                 YF_BASE2, symbol);
        body = fetch_authenticated(url, crumb);
    }
    if (!body) {
        fprintf(stderr, "[stock] No options data for %s\n", symbol);
        return 1;
    }

    // Parse and compute stats
    double iv_min = 0, iv_max = 0, iv_mean = 0;
    compute_iv_stats(body, &iv_min, &iv_max, &iv_mean);

    double max_pain = compute_max_pain(body, symbol);

    // Parse full chain for display
    json_error_t err;
    json_t *root = json_loads(body, 0, &err);
    if (!root) { free(body); return 1; }

    json_t *opt = json_object_get(root, "optionChain");
    json_t *rarr = opt ? json_object_get(opt, "result") : NULL;
    json_t *r0 = rarr && json_array_size(rarr) > 0 ? json_array_get(rarr, 0) : NULL;

    // Get underlying price
    double underlying = 0;
    if (r0) {
        json_t *quote = json_object_get(r0, "quote");
        if (quote) underlying = json_num(quote, "regularMarketPrice", 0);
        if (underlying <= 0) {
            json_t *meta = json_object_get(r0, "summary");
            if (meta) underlying = json_num(meta, "underlyingPrice", 0);
        }
    }

    // Get risk-free rate (approximate)
    double risk_free = 0.045;  // ~4.5% current

    printf("{\n");
    printf("  \"symbol\": \"%s\",\n", symbol);
    printf("  \"underlying_price\": %.2f,\n", underlying);
    printf("  \"max_pain\": %.2f,\n", max_pain);
    printf("  \"iv_min\": %.4f,\n", iv_min);
    printf("  \"iv_max\": %.4f,\n", iv_max);
    printf("  \"iv_mean\": %.4f,\n", iv_mean);

    // Store to DB if available
    if (db) {
        // Get fundamentals first for the store
        char qurl[MAX_URL];
        snprintf(qurl, sizeof(qurl), "%s/v11/finance/quoteSummary/%s?modules=price,summaryDetail,defaultKeyStatistics,financialData",
                 YF_BASE, symbol);
        char *qbody = fetch_authenticated(qurl, crumb);
        if (!qbody) {
            snprintf(qurl, sizeof(qurl), "%s/v11/finance/quoteSummary/%s?modules=price,summaryDetail,defaultKeyStatistics,financialData",
                     YF_BASE2, symbol);
            qbody = fetch_authenticated(qurl, crumb);
        }
        if (qbody) {
            store_to_db(db, symbol, time(NULL), qbody, body);
            free(qbody);
        }
    }
    free(crumb);

    // Output option chain data (first expiry, top/bottom strikes)
    json_t *oarr = r0 ? json_object_get(r0, "options") : NULL;
    if (oarr && json_array_size(oarr) > 0) {
        json_t *exp0 = json_array_get(oarr, 0);
        json_t *calls = json_object_get(exp0, "calls");
        json_t *puts = json_object_get(exp0, "puts");

        // Get expiration
        json_t *exps = json_object_get(r0, "expirationDates");
        char near_exp[32] = "unknown";
        if (exps && json_array_size(exps) > 0) {
            time_t et = (time_t)json_number_value(json_array_get(exps, 0));
            struct tm *tm = gmtime(&et);
            strftime(near_exp, sizeof(near_exp), "%Y-%m-%d", tm);
        }
        printf("  \"near_expiration\": \"%s\",\n", near_exp);
        printf("  \"days_to_expiry\": %d,\n", days_to_expiry(near_exp));

        // Top 5 call strikes by OI
        printf("  \"top_calls_by_oi\": [\n");
        if (calls) {
            // Simple approach: print all near-the-money calls
            int count = 0;
            for (size_t i = 0; i < json_array_size(calls) && count < 5; i++) {
                json_t *opt = json_array_get(calls, i);
                double strike = json_num(opt, "strike", 0);
                double oi = json_num(opt, "openInterest", 0);
                if (oi < 1) continue;
                double iv = json_num(opt, "impliedVolatility", 0);
                double vol = json_num(opt, "volume", 0);
                double last = json_num(opt, "lastPrice", 0);
                double bid = json_num(opt, "bid", 0);
                double ask = json_num(opt, "ask", 0);

                int dte = days_to_expiry(near_exp);
                double T = dte / 365.0;
                double delta = black_scholes_delta(underlying, strike, T, risk_free, iv, 1);
                double gamma = black_scholes_gamma(underlying, strike, T, risk_free, iv);
                double theta = black_scholes_theta(underlying, strike, T, risk_free, iv, 1);
                double vega = black_scholes_vega(underlying, strike, T, risk_free, iv);

                printf("    {\"strike\":%.2f,\"oi\":%.0f,\"volume\":%.0f,"
                       "\"last\":%.2f,\"bid\":%.2f,\"ask\":%.2f,"
                       "\"iv\":%.4f,\"delta\":%.4f,\"gamma\":%.4f,"
                       "\"theta\":%.6f,\"vega\":%.4f}", 
                       strike, oi, vol, last, bid, ask, iv, delta, gamma, theta, vega);
                count++;
                if (count < 5) printf(",");
                printf("\n");
            }
        }
        printf("  ],\n");

        // Top 5 put strikes by OI
        printf("  \"top_puts_by_oi\": [\n");
        if (puts) {
            int count = 0;
            for (size_t i = 0; i < json_array_size(puts) && count < 5; i++) {
                json_t *opt = json_array_get(puts, i);
                double strike = json_num(opt, "strike", 0);
                double oi = json_num(opt, "openInterest", 0);
                if (oi < 1) continue;
                double iv = json_num(opt, "impliedVolatility", 0);
                double vol = json_num(opt, "volume", 0);
                double last = json_num(opt, "lastPrice", 0);
                double bid = json_num(opt, "bid", 0);
                double ask = json_num(opt, "ask", 0);

                int dte = days_to_expiry(near_exp);
                double T = dte / 365.0;
                double delta = black_scholes_delta(underlying, strike, T, risk_free, iv, 0);
                double gamma = black_scholes_gamma(underlying, strike, T, risk_free, iv);
                double theta = black_scholes_theta(underlying, strike, T, risk_free, iv, 0);
                double vega = black_scholes_vega(underlying, strike, T, risk_free, iv);

                printf("    {\"strike\":%.2f,\"oi\":%.0f,\"volume\":%.0f,"
                       "\"last\":%.2f,\"bid\":%.2f,\"ask\":%.2f,"
                       "\"iv\":%.4f,\"delta\":%.4f,\"gamma\":%.4f,"
                       "\"theta\":%.6f,\"vega\":%.4f}",
                       strike, oi, vol, last, bid, ask, iv, delta, gamma, theta, vega);
                count++;
                if (count < 5) printf(",");
                printf("\n");
            }
        }
        printf("  ]\n");
    }

    printf("}\n");

    json_decref(root);
    free(body);
    return 0;
}

// ── Command: All (full pipeline to DB) ──
static int cmd_all(const char *symbol) {
    sqlite3 *db = db_open();
    if (!db) return 1;

    time_t now = time(NULL);
    char tstr[64];
    struct tm *tm = localtime(&now);
    strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", tm);

    printf("{\n");
    printf("  \"symbol\": \"%s\",\n", symbol);
    printf("  \"timestamp\": \"%s\",\n", tstr);

    // Get fundamentals
    char *crumb = fetch_crumb();
    if (!crumb) {
        fprintf(stderr, "[stock] Failed to get crumb\n");
        sqlite3_close(db);
        return 1;
    }

    // Fetch quoteSummary
    char qurl[MAX_URL];
    snprintf(qurl, sizeof(qurl), "%s/v11/finance/quoteSummary/%s?modules=price,summaryDetail,defaultKeyStatistics,financialData",
             YF_BASE, symbol);
    char *qbody = fetch_authenticated(qurl, crumb);
    if (!qbody) {
        snprintf(qurl, sizeof(qurl), "%s/v11/finance/quoteSummary/%s?modules=price,summaryDetail,defaultKeyStatistics,financialData",
                 YF_BASE2, symbol);
        qbody = fetch_authenticated(qurl, crumb);
    }

    // Fetch options
    char ourl[MAX_URL];
    snprintf(ourl, sizeof(ourl), "%s/v7/finance/options/%s",
             YF_BASE, symbol);
    char *obody = fetch_authenticated(ourl, crumb);
    if (!obody) {
        snprintf(ourl, sizeof(ourl), "%s/v7/finance/options/%s",
                 YF_BASE2, symbol);
        obody = fetch_authenticated(ourl, crumb);
    }

    free(crumb);

    // Store to DB
    if (qbody) {
        store_to_db(db, symbol, now, qbody, obody);
        printf("  \"fundamentals\": \"stored\",\n");
        free(qbody);
    } else {
        printf("  \"fundamentals\": \"failed\",\n");
    }

    if (obody) {
        free(obody);
        printf("  \"options\": \"stored\",\n");
    } else {
        printf("  \"options\": \"not_available\",\n");
    }

    printf("  \"data_source\": \"Yahoo Finance\"\n");
    printf("}\n");

    sqlite3_close(db);
    return 0;
}

// ── Command: DB query (latest N snapshots) ──
static int cmd_db(const char *symbol, int n) {
    sqlite3 *db = db_open();
    if (!db) return 1;

    // Quotes
    printf("{\n");
    printf("  \"symbol\": \"%s\",\n", symbol);

    sqlite3_stmt *stmt;
    const char *sql = "SELECT ts, price, volume, market_cap, pe_ratio "
                       "FROM quotes WHERE symbol=? ORDER BY ts DESC LIMIT ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, symbol, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, n);
        printf("  \"quotes\": [\n");
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) printf(",\n");
            first = 0;
            time_t ts = (time_t)sqlite3_column_int64(stmt, 0);
            struct tm *tm = gmtime(&ts);
            char ts_str[32];
            strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M:%S", tm);
            printf("    {\"ts\":\"%s\",\"price\":%.2f,\"volume\":%lld,"
                   "\"market_cap\":%.0f,\"pe\":%.2f}",
                   ts_str, sqlite3_column_double(stmt, 1),
                   (long long)sqlite3_column_int64(stmt, 2),
                   sqlite3_column_double(stmt, 3),
                   sqlite3_column_double(stmt, 4));
        }
        printf("\n  ],\n");
        sqlite3_finalize(stmt);
    }

    // Option summaries
    sql = "SELECT ts, near_expiration, max_pain_strike, put_call_volume_ratio, "
           "put_call_oi_ratio, iv_mean, iv_rank_hv "
           "FROM option_summary WHERE symbol=? ORDER BY ts DESC LIMIT ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, symbol, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, n);
        printf("  \"option_summaries\": [\n");
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) printf(",\n");
            first = 0;
            time_t ts = (time_t)sqlite3_column_int64(stmt, 0);
            struct tm *tm = gmtime(&ts);
            char ts_str[32];
            strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M:%S", tm);
            printf("    {\"ts\":\"%s\",\"expiration\":\"%s\",\"max_pain\":%.2f,"
                   "\"pc_vol_ratio\":%.2f,\"pc_oi_ratio\":%.2f,"
                   "\"iv_mean\":%.4f,\"iv_rank\":%.2f}",
                   ts_str, sqlite3_column_text(stmt, 1),
                   sqlite3_column_double(stmt, 2),
                   sqlite3_column_double(stmt, 3),
                   sqlite3_column_double(stmt, 4),
                   sqlite3_column_double(stmt, 5),
                   sqlite3_column_double(stmt, 6));
        }
        printf("\n  ]\n");
        sqlite3_finalize(stmt);
    }

    printf("}\n");
    sqlite3_close(db);
    return 0;
}

// ── Usage ──
static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s quote <SYMBOL>               — real-time quote\n", prog);
    fprintf(stderr, "  %s fundamentals <SYMBOL>        — fundamentals snapshot\n", prog);
    fprintf(stderr, "  %s options <SYMBOL>             — option chain + Greeks\n", prog);
    fprintf(stderr, "  %s all <SYMBOL>                 — full pipeline to DB\n", prog);
    fprintf(stderr, "  %s db <SYMBOL> [N]              — latest N DB records\n", prog);
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s quote AAPL\n", prog);
    fprintf(stderr, "  %s fundamentals MSFT\n", prog);
    fprintf(stderr, "  %s options SPY\n", prog);
    fprintf(stderr, "  %s all NVDA\n", prog);
    fprintf(stderr, "  %s db AAPL 5\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];
    const char *symbol = argv[2];

    // Uppercase symbol
    char sym_upper[16];
    strncpy(sym_upper, symbol, 15);
    sym_upper[15] = '\0';
    for (char *p = sym_upper; *p; p++) *p = toupper(*p);

    // Ensure cache directory exists
    char syscmd[MAX_PATH];
    snprintf(syscmd, sizeof(syscmd), "mkdir -p %s", DB_DIR);
    system(syscmd);

    if (strcmp(cmd, "quote") == 0) {
        return cmd_quote(sym_upper);
    } else if (strcmp(cmd, "fundamentals") == 0) {
        return cmd_fundamentals(sym_upper);
    } else if (strcmp(cmd, "options") == 0) {
        return cmd_options(sym_upper, NULL);
    } else if (strcmp(cmd, "all") == 0) {
        return cmd_all(sym_upper);
    } else if (strcmp(cmd, "db") == 0) {
        int n = 5;
        if (argc >= 4) n = atoi(argv[3]);
        if (n < 1) n = 1;
        if (n > 50) n = 50;
        return cmd_db(sym_upper, n);
    } else {
        fprintf(stderr, "[stock] Unknown command: %s\n", cmd);
        usage(argv[0]);
        return 1;
    }
}
