/*
 * options_flow.c — P62: Options Flow Alert Pipeline
 *
 * Fetches SPY/QQQ options chain from CBOE CDN API, stores snapshots
 * in SQLite, detects volume surges and large premium flow.
 *
 * Data source: cdn.cboe.com/api/global/delayed_quotes/options/{TICKER}.json
 * Free, no API key, delayed ~15min (as permitted for non-commercial use)
 *
 * Features:
 *   fetch <ticker>        — Fetch and store current options chain
 *   diff <ticker>         — Compare with latest snapshot, show flow alerts
 *   db <ticker> [N]       — Show last N snapshots summary
 *   monitor <ticker>      — Fetch + diff (one-shot, use cron for periodic)
 *
 * Build: gcc options_flow.c -o options_flow -lcurl -ljansson -lsqlite3 -lm -O2
 * Cron: hermes cron create --every 15m "./options_flow spy"
 *
 * CBOE option name format: TICKERYYMMDDC/PSTRIKE
 *   e.g., SPY260527C00500000 = SPY, 2026-05-27, Call, $500.000
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>

// ── Configuration ──
#define DB_DIR      "/home/wubu2/.hermes/options_cache"
#define CURL_TIMEOUT 30L
#define VOL_SURGE_MULT 3.0f   // Alert when volume > 3x avg
#define PREMIUM_THRESHOLD 100000.0f  // $100K premium = alert

// ── Contract type ──
typedef enum { CALL, PUT, UNKNOWN } OptionType;

// ── Per-option data from CBOE ──
typedef struct {
    char     name[64];       // SPY260527C00500000
    OptionType type;
    int      expiry_year, expiry_month, expiry_day;
    float    strike;
    float    bid, ask;
    int      bid_size, ask_size;
    float    volume;
    float    open_interest;
    float    iv;              // Implied volatility
    float    delta, gamma, theta, vega, rho;
    float    last_price;
    float    prev_close;
    float    change;
    float    theo;            // Theoretical price
    int64_t  last_trade_time; // Unix timestamp
} OptionRecord;

// ── Snapshot metadata ──
typedef struct {
    int64_t  ts;              // Unix timestamp
    char     ticker[8];
    float    underlying_price;
    float    underlying_bid, underlying_ask;
    int      n_options;
    int      n_calls, n_puts;
    float    total_call_volume, total_put_volume;
    float    total_call_oi, total_put_oi;
} SnapshotMeta;

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

// ── Parse option name ──
// Format: TICKERYYMMDDC/PSTRIKE (e.g., SPY260527C00500000)
// Strike: last 9 digits? Let's check: 00500000 = 500.000
// Actually: SPY260527C00500000 — C after date, then 8 digits of strike
// Strike: 00500000 = divide by 1000 = $500.000
static OptionRecord parse_option_name(const char *name) {
    OptionRecord r = {0};
    strncpy(r.name, name, 63); r.name[63] = '\0';
    
    // Find call/put indicator — search BACKWARDS from end to avoid
    // matching 'P' in "SPY" (position 1). The last C/P is always the type.
    int len = strlen(name);
    if (len < 15) { r.type = UNKNOWN; return r; }
    
    int call_pos = -1;
    for (int i = len - 1; i >= 4; i--) {
        if (name[i] == 'C' || name[i] == 'P') {
            call_pos = i;
            break;
        }
    }
    if (call_pos < 4) { r.type = UNKNOWN; return r; }
    
    // Date is between ticker end and C/P
    // Ticker = first 3-4 chars (SPY = 3, QQQ = 3)
    int ticker_len = 3; // default for SPY/QQQ
    int date_start = ticker_len;
    int date_len = call_pos - date_start;
    
    if (date_len >= 6) {
        char date_str[16];
        strncpy(date_str, name + date_start, 6);
        date_str[6] = '\0';
        r.expiry_year = 2000 + (date_str[0]-'0')*10 + (date_str[1]-'0');
        r.expiry_month = (date_str[2]-'0')*10 + (date_str[3]-'0');
        r.expiry_day = (date_str[4]-'0')*10 + (date_str[5]-'0');
    }
    
    r.type = (name[call_pos] == 'C') ? CALL : PUT;
    
    // Strike is everything after C/P — 9 digits, last 3 are decimals
    int strike_start = call_pos + 1;
    int strike_len = len - strike_start;
    if (strike_len >= 5) {
        char strike_str[32];
        strncpy(strike_str, name + strike_start, strike_len);
        strike_str[strike_len] = '\0';
        r.strike = atof(strike_str) / 1000.0f;
    }
    
    return r;
}

// ── SQLite helpers ──
static sqlite3 *db_open(const char *ticker) {
    char path[256];
    // Ensure directory exists
    char mkdir_cmd[256];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", DB_DIR);
    system(mkdir_cmd);
    
    snprintf(path, sizeof(path), "%s/%s_flows.db", DB_DIR, ticker);
    sqlite3 *db;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Can't open DB: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    
    // Enable WAL mode
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    
    // Create tables
    const char *schema =
        "CREATE TABLE IF NOT EXISTS snapshots ("
        "  ts INTEGER PRIMARY KEY,"
        "  ticker TEXT,"
        "  underlying REAL,"
        "  n_calls INTEGER, n_puts INTEGER,"
        "  call_vol REAL, put_vol REAL,"
        "  call_oi REAL, put_oi REAL,"
        "  pcr_vol REAL, pcr_oi REAL"
        ");"
        "CREATE TABLE IF NOT EXISTS options ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  ts INTEGER NOT NULL,"
        "  type TEXT,"
        "  expiry TEXT,"
        "  strike REAL,"
        "  bid REAL, ask REAL,"
        "  volume REAL, open_interest REAL,"
        "  iv REAL, delta REAL, gamma REAL,"
        "  theta REAL, vega REAL, rho REAL,"
        "  last_price REAL, prev_close REAL,"
        "  premium REAL,"
        "  UNIQUE(name, ts)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_options_ts ON options(ts);"
        "CREATE INDEX IF NOT EXISTS idx_options_name ON options(name);"
        "CREATE INDEX IF NOT EXISTS idx_snapshots_ts ON snapshots(ts DESC);";
    
    char *err = NULL;
    rc = sqlite3_exec(db, schema, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Schema error: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    
    return db;
}

// ── Fetch options chain from CBOE ──
static json_t *fetch_options(const char *ticker) {
    char url[128];
    snprintf(url, sizeof(url), "https://cdn.cboe.com/api/global/delayed_quotes/options/%s.json",
             ticker);
    
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    
    HttpBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/8.0");
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        fprintf(stderr, "HTTP error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    
    json_error_t err;
    json_t *root = json_loads(buf.data, 0, &err);
    free(buf.data);
    
    if (!root) {
        fprintf(stderr, "JSON parse error at %d: %s\n", err.line, err.text);
        return NULL;
    }
    
    return root;
}

// ── Store snapshot in DB ──
static int store_snapshot(sqlite3 *db, json_t *root, const char *ticker) {
    json_t *data = json_object_get(root, "data");
    if (!data) { fprintf(stderr, "No 'data' in response\n"); return 1; }
    
    json_t *options = json_object_get(data, "options");
    if (!options || !json_is_array(options)) {
        fprintf(stderr, "No 'options' array\n"); return 1;
    }
    
    int64_t ts = (int64_t)time(NULL);
    float underlying = (float)json_number_value(json_object_get(data, "current_price"));
    float bid = (float)json_number_value(json_object_get(data, "bid"));
    float ask = (float)json_number_value(json_object_get(data, "ask"));
    
    int n_opts = json_array_size(options);
    int n_calls = 0, n_puts = 0;
    float call_vol = 0, put_vol = 0;
    float call_oi = 0, put_oi = 0;
    float call_prem = 0, put_prem = 0;
    
    // Begin transaction
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR IGNORE INTO options "
        "(name, ts, type, expiry, strike, bid, ask, volume, open_interest, "
        " iv, delta, gamma, theta, vega, rho, last_price, prev_close, premium) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL prepare: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return 1;
    }
    
    for (int i = 0; i < n_opts; i++) {
        json_t *opt = json_array_get(options, i);
        const char *opt_name = json_string_value(json_object_get(opt, "option"));
        if (!opt_name) continue;
        
        OptionRecord r = parse_option_name(opt_name);
        
        r.bid = (float)json_number_value(json_object_get(opt, "bid"));
        r.ask = (float)json_number_value(json_object_get(opt, "ask"));
        r.bid_size = (int)json_number_value(json_object_get(opt, "bid_size"));
        r.ask_size = (int)json_number_value(json_object_get(opt, "ask_size"));
        r.volume = (float)json_number_value(json_object_get(opt, "volume"));
        r.open_interest = (float)json_number_value(json_object_get(opt, "open_interest"));
        r.iv = (float)json_number_value(json_object_get(opt, "iv"));
        r.delta = (float)json_number_value(json_object_get(opt, "delta"));
        r.gamma = (float)json_number_value(json_object_get(opt, "gamma"));
        r.theta = (float)json_number_value(json_object_get(opt, "theta"));
        r.vega = (float)json_number_value(json_object_get(opt, "vega"));
        r.rho = (float)json_number_value(json_object_get(opt, "rho"));
        r.last_price = (float)json_number_value(json_object_get(opt, "last_trade_price"));
        r.prev_close = (float)json_number_value(json_object_get(opt, "prev_day_close"));
        
        double premium = r.volume * ((r.bid + r.ask) / 2.0) * 100.0;
        
        char expiry_str[16];
        snprintf(expiry_str, sizeof(expiry_str), "%04d-%02d-%02d",
                 r.expiry_year, r.expiry_month, r.expiry_day);
        
        sqlite3_bind_text(stmt, 1, opt_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, ts);
        sqlite3_bind_text(stmt, 3, r.type == CALL ? "call" : "put", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, expiry_str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 5, r.strike);
        sqlite3_bind_double(stmt, 6, r.bid);
        sqlite3_bind_double(stmt, 7, r.ask);
        sqlite3_bind_double(stmt, 8, r.volume);
        sqlite3_bind_double(stmt, 9, r.open_interest);
        sqlite3_bind_double(stmt, 10, r.iv);
        sqlite3_bind_double(stmt, 11, r.delta);
        sqlite3_bind_double(stmt, 12, r.gamma);
        sqlite3_bind_double(stmt, 13, r.theta);
        sqlite3_bind_double(stmt, 14, r.vega);
        sqlite3_bind_double(stmt, 15, r.rho);
        sqlite3_bind_double(stmt, 16, r.last_price);
        sqlite3_bind_double(stmt, 17, r.prev_close);
        sqlite3_bind_double(stmt, 18, premium);
        
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
        
        if (r.type == CALL) {
            n_calls++; call_vol += r.volume; call_oi += r.open_interest; call_prem += premium;
        } else if (r.type == PUT) {
            n_puts++; put_vol += r.volume; put_oi += r.open_interest; put_prem += premium;
        }
    }
    
    sqlite3_finalize(stmt);
    
    // Insert snapshot metadata
    sqlite3_stmt *meta;
    const char *meta_sql = "INSERT INTO snapshots "
        "(ts, ticker, underlying, n_calls, n_puts, call_vol, put_vol, call_oi, put_oi, pcr_vol, pcr_oi) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    
    sqlite3_prepare_v2(db, meta_sql, -1, &meta, NULL);
    sqlite3_bind_int64(meta, 1, ts);
    sqlite3_bind_text(meta, 2, ticker, -1, SQLITE_STATIC);
    sqlite3_bind_double(meta, 3, underlying);
    sqlite3_bind_int(meta, 4, n_calls);
    sqlite3_bind_int(meta, 5, n_puts);
    sqlite3_bind_double(meta, 6, call_vol);
    sqlite3_bind_double(meta, 7, put_vol);
    sqlite3_bind_double(meta, 8, call_oi);
    sqlite3_bind_double(meta, 9, put_oi);
    sqlite3_bind_double(meta, 10, put_vol > 0 ? call_vol / put_vol : 999);
    sqlite3_bind_double(meta, 11, put_oi > 0 ? call_oi / put_oi : 999);
    sqlite3_step(meta);
    sqlite3_finalize(meta);
    
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    
    printf("  Stored: %d calls + %d puts = %d options\n", n_calls, n_puts, n_opts);
    printf("  Call vol: %.0f | Put vol: %.0f | PCR vol: %.2f\n",
           call_vol, put_vol, put_vol > 0 ? call_vol/put_vol : 999);
    printf("  Call OI:  %.0f | Put OI:  %.0f | PCR OI:  %.2f\n",
           call_oi, put_oi, put_oi > 0 ? call_oi/put_oi : 999);
    printf("  Call prem: $%.0f | Put prem: $%.0f\n", call_prem, put_prem);
    printf("  Underlying: $%.2f | Bid/Ask: %.2f/%.2f\n", underlying, bid, ask);
    
    return 0;
}

// ── Diff: compare last two snapshots for flow alerts ──
static int diff_snapshots(sqlite3 *db, const char *ticker) {
    // Get last two snapshot timestamps
    sqlite3_stmt *stmt;
    const char *sql = "SELECT ts FROM snapshots ORDER BY ts DESC LIMIT 2;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    
    int64_t ts_curr = 0, ts_prev = 0;
    int row = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (row == 0) ts_curr = sqlite3_column_int64(stmt, 0);
        if (row == 1) ts_prev = sqlite3_column_int64(stmt, 0);
        row++;
    }
    sqlite3_finalize(stmt);
    
    if (ts_prev == 0) {
        printf("  Need 2+ snapshots for diff. Fetch again later.\n");
        return 0;
    }
    
    // Get previous snapshot's options
    sqlite3_stmt *curr, *prev;
    const char *opt_sql = "SELECT name, volume FROM options WHERE ts = ?;";
    sqlite3_prepare_v2(db, opt_sql, -1, &curr, NULL);
    sqlite3_bind_int64(curr, 1, ts_curr);
    sqlite3_prepare_v2(db, opt_sql, -1, &prev, NULL);
    sqlite3_bind_int64(prev, 1, ts_prev);
    
    // Find volume surges
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  OPTIONS FLOW ALERTS — %s                      ║\n", ticker);
    printf("╠══════════════════════════════════════════════════╣\n");
    
    // Build hash table for prev volumes
    #define MAX_ALERTS 100
    typedef struct { char name[64]; float vol; } VolEntry;
    int n_prev = 0;
    VolEntry prev_vols[14678];
    
    while (sqlite3_step(prev) == SQLITE_ROW) {
        const char *name = (const char*)sqlite3_column_text(prev, 0);
        float vol = (float)sqlite3_column_double(prev, 1);
        strncpy(prev_vols[n_prev].name, name, 63);
        prev_vols[n_prev].name[63] = '\0';
        prev_vols[n_prev].vol = vol;
        n_prev++;
    }
    sqlite3_finalize(prev);
    
    // Compare current vs previous
    int alert_count = 0;
    int rich_alert_count = 0;
    float total_call_prem = 0, total_put_prem = 0;
    int surge_alerts = 0;
    
    while (sqlite3_step(curr) == SQLITE_ROW) {
        const char *name = (const char*)sqlite3_column_text(curr, 0);
        float curr_vol = (float)sqlite3_column_double(curr, 1);
        
        // Find prev vol
        float prev_vol = 0;
        for (int i = 0; i < n_prev; i++) {
            if (strcmp(prev_vols[i].name, name) == 0) {
                prev_vol = prev_vols[i].vol;
                break;
            }
        }
        
        // Volume surge detection
        if (prev_vol > 0 && curr_vol > prev_vol * VOL_SURGE_MULT && curr_vol > 50) {
            // Get full option info from current snapshot
            sqlite3_stmt *info;
            const char *info_sql = "SELECT type, strike, bid, ask, iv, delta, premium, expiry "
                "FROM options WHERE name = ? AND ts = ?";
            sqlite3_prepare_v2(db, info_sql, -1, &info, NULL);
            sqlite3_bind_text(info, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(info, 2, ts_curr);
            
            if (sqlite3_step(info) == SQLITE_ROW) {
                const char *type = (const char*)sqlite3_column_text(info, 0);
                float strike = (float)sqlite3_column_double(info, 1);
                float bid = (float)sqlite3_column_double(info, 2);
                float ask = (float)sqlite3_column_double(info, 3);
                float iv = (float)sqlite3_column_double(info, 4);
                float delta = (float)sqlite3_column_double(info, 5);
                float premium = (float)sqlite3_column_double(info, 6);
                const char *expiry = (const char*)sqlite3_column_text(info, 7);
                
                printf("  ⚡ SURGE: %s %s $%.2f %s\n", type, name, strike, expiry);
                printf("    Vol: %.0f → %.0f (%.0fx) | Prem: $%.0f\n",
                       prev_vol, curr_vol, curr_vol/prev_vol, premium);
                printf("    Bid/Ask: %.2f/%.2f | IV: %.1f%% | Delta: %.2f\n",
                       bid, ask, iv*100, delta);
                
                surge_alerts++;
                if (type[0] == 'c') total_call_prem += premium;
                else total_put_prem += premium;
            }
            sqlite3_finalize(info);
            alert_count++;
            if (alert_count >= MAX_ALERTS) break;
        }
        
        // Rich premium detection (any single option > $100K premium)
        if (curr_vol > 0) {
            sqlite3_stmt *info2;
            const char *info_sql2 = "SELECT premium, type, strike, expiry FROM options "
                "WHERE name = ? AND ts = ? AND premium > ?";
            sqlite3_prepare_v2(db, info_sql2, -1, &info2, NULL);
            sqlite3_bind_text(info2, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(info2, 2, ts_curr);
            sqlite3_bind_double(info2, 3, PREMIUM_THRESHOLD);
            
            if (sqlite3_step(info2) == SQLITE_ROW && rich_alert_count < MAX_ALERTS) {
                float prem = (float)sqlite3_column_double(info2, 0);
                const char *type = (const char*)sqlite3_column_text(info2, 1);
                printf("  💰 RICH: %s %s — Premium $%.0f | Vol %.0f\n",
                       type, name, prem, curr_vol);
                rich_alert_count++;
            }
            sqlite3_finalize(info2);
        }
    }
    sqlite3_finalize(curr);
    
    // Get snapshot summaries
    printf("╠══════════════════════════════════════════════════╣\n");
    
    sqlite3_stmt *ss;
    const char *ss_sql = "SELECT ts, underlying, n_calls, n_puts, call_vol, put_vol, "
        "call_oi, put_oi, pcr_vol, pcr_oi FROM snapshots "
        "WHERE ts IN (?, ?) ORDER BY ts ASC;";
    sqlite3_prepare_v2(db, ss_sql, -1, &ss, NULL);
    sqlite3_bind_int64(ss, 1, ts_prev);
    sqlite3_bind_int64(ss, 2, ts_curr);
    
    while (sqlite3_step(ss) == SQLITE_ROW) {
        int64_t sts = sqlite3_column_int64(ss, 0);
        float under = (float)sqlite3_column_double(ss, 1);
        int nc = sqlite3_column_int(ss, 2);
        int np = sqlite3_column_int(ss, 3);
        float cv = (float)sqlite3_column_double(ss, 4);
        float pv = (float)sqlite3_column_double(ss, 5);
        float co = (float)sqlite3_column_double(ss, 6);
        float po = (float)sqlite3_column_double(ss, 7);
        float pcr = (float)sqlite3_column_double(ss, 8);
        float pcr_oi = (float)sqlite3_column_double(ss, 9);
        
        char ts_str[32];
        struct tm *tm = localtime((time_t*)&sts);
        strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M", tm);
        
        printf("  %-16s | SPY $%.2f | C/P %d/%d | PCR vol: %.2f OI: %.2f\n",
               ts_str, under, nc, np, pcr, pcr_oi);
        printf("  %16s | Call vol: %.0f | Put vol: %.0f | Call OI: %.0f | Put OI: %.0f\n",
               "", cv, pv, co, po);
    }
    sqlite3_finalize(ss);
    
    if (surge_alerts == 0 && rich_alert_count == 0) {
        printf("  ✅ No alerts — normal flow.\n");
    } else {
        printf("  └─ %d surge alerts + %d rich premium signals\n", surge_alerts, rich_alert_count);
        printf("  Total call premium: $%.0f | Total put premium: $%.0f\n",
               total_call_prem, total_put_prem);
    }
    
    printf("╚══════════════════════════════════════════════════╝\n");
    
    return 0;
}

// ── Show recent snapshots ──
static int cmd_db(const char *ticker, int n) {
    sqlite3 *db = db_open(ticker);
    if (!db) return 1;
    
    sqlite3_stmt *stmt;
    const char *sql = "SELECT ts, underlying, n_calls, n_puts, call_vol, put_vol, "
        "pcr_vol, pcr_oi FROM snapshots ORDER BY ts DESC LIMIT ?;";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, n);
    
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  %s OPTIONS SNAPSHOTS (last %d)                    ║\n", ticker, n);
    printf("╠══════════════════════════════════════════════════╣\n");
    
    int rows = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t sts = sqlite3_column_int64(stmt, 0);
        float under = (float)sqlite3_column_double(stmt, 1);
        int nc = sqlite3_column_int(stmt, 2);
        int np = sqlite3_column_int(stmt, 3);
        float cv = (float)sqlite3_column_double(stmt, 4);
        float pv = (float)sqlite3_column_double(stmt, 5);
        float pcr_vol = (float)sqlite3_column_double(stmt, 6);
        float pcr_oi = (float)sqlite3_column_double(stmt, 7);
        
        char ts_str[32];
        struct tm *tm = localtime((time_t*)&sts);
        strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M", tm);
        
        printf("  %s | $%.2f | %d opts | C vol: %.0f P vol: %.0f | PCR v:%.2f oi:%.2f\n",
               ts_str, under, nc+np, cv, pv, pcr_vol, pcr_oi);
        rows++;
    }
    sqlite3_finalize(stmt);
    
    if (rows == 0) printf("  No snapshots yet. Run 'fetch' first.\n");
    printf("╚══════════════════════════════════════════════════╝\n");
    
    sqlite3_close(db);
    return 0;
}

// ── Fetch + store ──
static int cmd_fetch(const char *ticker) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  FETCHING OPTIONS CHAIN — %s                    ║\n", ticker);
    printf("╠══════════════════════════════════════════════════╣\n");
    
    json_t *root = fetch_options(ticker);
    if (!root) { printf("║  ❌ Fetch failed!                           ║\n╚══════════════════════════════════════════════════╝\n"); return 1; }
    
    sqlite3 *db = db_open(ticker);
    if (!db) { json_decref(root); return 1; }
    
    store_snapshot(db, root, ticker);
    json_decref(root);
    sqlite3_close(db);
    
    printf("╚══════════════════════════════════════════════════╝\n");
    return 0;
}

// ── Fetch + diff (monitor mode) ──
static int cmd_monitor(const char *ticker) {
    json_t *root = fetch_options(ticker);
    if (!root) return 1;
    
    sqlite3 *db = db_open(ticker);
    if (!db) { json_decref(root); return 1; }
    
    store_snapshot(db, root, ticker);
    diff_snapshots(db, ticker);
    
    json_decref(root);
    sqlite3_close(db);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s fetch   <ticker>   — Fetch and store options chain\n", argv[0]);
        fprintf(stderr, "  %s diff    <ticker>   — Compare last 2 snapshots for alerts\n", argv[0]);
        fprintf(stderr, "  %s monitor <ticker>   — Fetch + diff (1-shot, use with cron)\n", argv[0]);
        fprintf(stderr, "  %s db      <ticker> [N] — Show last N snapshots\n", argv[0]);
        fprintf(stderr, "Ticker examples: SPY, QQQ, IWM, GLD\n");
        return 1;
    }
    
    const char *cmd = argv[1];
    const char *ticker = argv[2];
    
    // Uppercase ticker
    char ticker_upper[16];
    strncpy(ticker_upper, ticker, 15); ticker_upper[15] = '\0';
    for (int i = 0; ticker_upper[i]; i++)
        ticker_upper[i] = toupper((unsigned char)ticker_upper[i]);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    int rc = 0;
    if (strcmp(cmd, "fetch") == 0) {
        rc = cmd_fetch(ticker_upper);
    } else if (strcmp(cmd, "diff") == 0) {
        sqlite3 *db = db_open(ticker_upper);
        if (db) { rc = diff_snapshots(db, ticker_upper); sqlite3_close(db); }
        else rc = 1;
    } else if (strcmp(cmd, "monitor") == 0) {
        rc = cmd_monitor(ticker_upper);
    } else if (strcmp(cmd, "db") == 0) {
        int n = argc > 3 ? atoi(argv[3]) : 5;
        rc = cmd_db(ticker_upper, n);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        rc = 1;
    }
    
    curl_global_cleanup();
    return rc;
}
