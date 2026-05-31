/**
 * paper_feature_bridge.c — Historical aux data for paper mode
 * 
 * Replaces hardcoded constants (vix=16, sp500=5000, etc.) with real
 * historical values from timeline.db. For each BTC candle timestamp,
 * looks up the nearest SP500, VIX, and computes BTC 30d stats from
 * bitstamp_1min data.
 * 
 * Compile: linked into paper engine only (PAPER_MODE)
 *   gcc -O3 -DPAPER_MODE -c paper_feature_bridge.c -lsqlite3
 *
 * Dependencies: libsqlite3, types.h ("MarketTick" definition)
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include "types.h"

#define TIMELINE_DB "/home/wubu2/.hermes/pm_logs/timeline.db"

// ── Cached SQLite handles ──
static sqlite3 *g_tl_db = NULL;
static sqlite3_stmt *g_sp500_stmt = NULL;
static sqlite3_stmt *g_vix_stmt = NULL;
static sqlite3_stmt *g_btc30d_stmt = NULL;
static int g_initialized = 0;

// ── Open timeline.db and prepare statements ──
static int paper_aux_init(void) {
    if (g_initialized) return 0;
    
    if (sqlite3_open(TIMELINE_DB, &g_tl_db) != SQLITE_OK) {
        fprintf(stderr, "[PAPER_AUX] Failed to open %s\n", TIMELINE_DB);
        return -1;
    }
    
    // SP500: nearest value <= candle_ts
    const char *sql_sp500 =
        "SELECT data FROM timeline WHERE source='fred_sp500' "
        "AND ts <= ?1 ORDER BY ts DESC LIMIT 1";
    if (sqlite3_prepare_v2(g_tl_db, sql_sp500, -1, &g_sp500_stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[PAPER_AUX] Failed to prepare sp500 query: %s\n", sqlite3_errmsg(g_tl_db));
        return -1;
    }
    
    // VIX: nearest value <= candle_ts
    const char *sql_vix =
        "SELECT data FROM timeline WHERE source='fred_vix' "
        "AND ts <= ?1 ORDER BY ts DESC LIMIT 1";
    if (sqlite3_prepare_v2(g_tl_db, sql_vix, -1, &g_vix_stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[PAPER_AUX] Failed to prepare vix query: %s\n", sqlite3_errmsg(g_tl_db));
        return -1;
    }
    
    // BTC 30d: closes from bitstamp_1min in range [ts-30d, ts]
    const char *sql_btc30d =
        "SELECT json_extract(data, '$.close') FROM timeline "
        "WHERE source='bitstamp_1min' AND json_extract(data, '$.pair')='BTC' "
        "AND ts > ?1 AND ts <= ?2 "
        "AND json_extract(data, '$.close') IS NOT NULL "
        "ORDER BY ts DESC";
    if (sqlite3_prepare_v2(g_tl_db, sql_btc30d, -1, &g_btc30d_stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[PAPER_AUX] Failed to prepare btc30d query: %s\n", sqlite3_errmsg(g_tl_db));
        return -1;
    }
    
    g_initialized = 1;
    fprintf(stderr, "[PAPER_AUX] Initialized: SP500, VIX, BTC-30d lookups\n");
    return 0;
}

// ── Extract a numeric value from a timeline JSON data field ──
// Looks for "close" then "value" then "open" key
static double json_extract_value(const char *json_str) {
    if (!json_str) return 0;
    
    // Try "close"
    const char *p = strstr(json_str, "\"close\"");
    if (p) {
        p += 7; // past "close"
        while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\"')) p++;
        if (*p) return strtod(p, NULL);
    }
    
    // Try "value"
    p = strstr(json_str, "\"value\"");
    if (p) {
        p += 6; // past "value"
        while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\"')) p++;
        if (*p) return strtod(p, NULL);
    }
    
    // Try "open"
    p = strstr(json_str, "\"open\"");
    if (p) {
        p += 5; // past "open"
        while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\"')) p++;
        if (*p) return strtod(p, NULL);
    }
    
    return 0;
}

// ── Lookup nearest SP500 value for a given timestamp ──
static double lookup_sp500(int64_t ts) {
    if (!g_sp500_stmt) return 0;
    
    sqlite3_reset(g_sp500_stmt);
    sqlite3_bind_int64(g_sp500_stmt, 1, ts);
    
    if (sqlite3_step(g_sp500_stmt) == SQLITE_ROW) {
        const char *data = (const char*)sqlite3_column_text(g_sp500_stmt, 0);
        if (data) return json_extract_value(data);
    }
    return 0;
}

// ── Lookup nearest VIX value for a given timestamp ──
static double lookup_vix(int64_t ts) {
    if (!g_vix_stmt) return 0;
    
    sqlite3_reset(g_vix_stmt);
    sqlite3_bind_int64(g_vix_stmt, 1, ts);
    
    if (sqlite3_step(g_vix_stmt) == SQLITE_ROW) {
        const char *data = (const char*)sqlite3_column_text(g_vix_stmt, 0);
        if (data) return json_extract_value(data);
    }
    return 0;
}

// ── Compute BTC 30d stats for a given timestamp ──
// Returns: vol_pct, mean, high, low in output params
static int compute_btc_30d_stats(int64_t ts, double *vol_pct, double *mean, double *high, double *low) {
    *vol_pct = 2.5;   // defaults
    *mean = 75000;
    *high = 82000;
    *low = 68000;
    
    if (!g_btc30d_stmt) return -1;
    
    int64_t ts_30d_ago = ts - 86400LL * 30;
    
    sqlite3_reset(g_btc30d_stmt);
    sqlite3_bind_int64(g_btc30d_stmt, 1, ts_30d_ago);
    sqlite3_bind_int64(g_btc30d_stmt, 2, ts);
    
    double sum = 0, sum_sq = 0;
    double h = -1e9, l = 1e9;
    int n = 0;
    
    while (sqlite3_step(g_btc30d_stmt) == SQLITE_ROW) {
        double c = sqlite3_column_double(g_btc30d_stmt, 0);
        if (c <= 0) continue;
        sum += c;
        sum_sq += c * c;
        if (c > h) h = c;
        if (c < l) l = c;
        n++;
    }
    
    if (n >= 60) { // Need at least 1 hour of data
        *mean = sum / n;
        *high = h;
        *low = l;
        double variance = sum_sq / n - (*mean) * (*mean);
        if (variance > 0) {
            *vol_pct = (sqrt(variance) / *mean) * 100.0;
        }
        return 0;
    }
    
    return -1; // Not enough data for reliable stats
}

// ── Main entry point: populate ALL aux fields in MarketTick ──
// Called from room_feeds.c PAPER_MODE after loading candle data
void paper_load_aux(MarketTick *tick) {
    if (!tick || tick->window_ts <= 0) {
        fprintf(stderr, "[PAPER_AUX] Invalid tick or timestamp\n");
        return;
    }
    
    if (paper_aux_init() != 0) {
        fprintf(stderr, "[PAPER_AUX] Init failed — using hardcoded fallbacks\n");
        return;
    }
    
    int64_t ts = tick->window_ts;
    
    // SP500
    double sp500 = lookup_sp500(ts);
    if (sp500 > 0) {
        tick->sp500 = (float)sp500;
    } else {
        fprintf(stderr, "[PAPER_AUX] No SP500 for ts=%ld, using fallback 5000\n", (long)ts);
        tick->sp500 = 5000.0f;
    }
    
    // VIX
    double vix = lookup_vix(ts);
    if (vix > 0) {
        tick->vix = (float)vix;
    } else {
        fprintf(stderr, "[PAPER_AUX] No VIX for ts=%ld, using fallback 16\n", (long)ts);
        tick->vix = 16.0f;
    }
    
    // BTC 30d stats
    double vol_pct, mean, high, low;
    if (compute_btc_30d_stats(ts, &vol_pct, &mean, &high, &low) == 0) {
        tick->btc_30d_volatility = (float)vol_pct;
        tick->btc_30d_mean = (float)mean;
        tick->btc_30d_high = (float)high;
        tick->btc_30d_low = (float)low;
    }
    // If compute fails, keep the existing defaults from room_feeds.c
    
    // BTC dominance: era-based estimate (real dominance varies ~40-70%)
    if (ts < 1451606400LL) {         tick->btc_dominance = 80.0f;
    } else if (ts < 1483228800LL) {  tick->btc_dominance = 75.0f;
    } else if (ts < 1514764800LL) {  tick->btc_dominance = 60.0f;
    } else if (ts < 1546300800LL) {  tick->btc_dominance = 45.0f;
    } else if (ts < 1577836800LL) {  tick->btc_dominance = 55.0f;
    } else if (ts < 1609459200LL) {  tick->btc_dominance = 62.0f;
    } else if (ts < 1640995200LL) {  tick->btc_dominance = 45.0f;
    } else if (ts < 1672531200LL) {  tick->btc_dominance = 40.0f;
    } else if (ts < 1704067200LL) {  tick->btc_dominance = 48.0f;
    } else if (ts < 1735689600LL) {  tick->btc_dominance = 52.0f;
    } else if (ts < 1767225600LL) {  tick->btc_dominance = 58.0f;
    } else {                          tick->btc_dominance = 62.0f; }
    
    // Pump score: neutral in paper mode (no historical news sentiment)
    tick->pump_score = 0.0f;
    
    // Set market_type for paper mode
    tick->market_type = MARKET_CRYPTO;
}
