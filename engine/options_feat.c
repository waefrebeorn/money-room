/*
 * options_feat.c — P31: Options-Implied Features
 *
 * Reads SPY options chain SQLite DB from options_flow.c and computes:
 *   - IV term structure slope (front/back month IV ratio → contango/backwardation)
 *   - IV skew (OTM put IV - OTM call IV)
 *   - ATM implied move (straddle mid / underlying)
 *   - 25-delta risk reversal
 *
 * Output: JSON file read by feed_bridge.c → engine features.
 *
 * Dependencies: libjansson, libsqlite3
 * Build: gcc -O3 -march=native options_feat.c -o options_feat -ljansson -lsqlite3 -lm
 * Cron: hermes cron create --every 15m "./options_feat SPY"
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <jansson.h>
#include <sqlite3.h>

#define DB_DIR      "/home/wubu2/.hermes/options_cache"
#define OUT_FILE    DB_DIR "/latest_features.json"

// Compute term structure slope from a sequence of (expiry, atm_iv) pairs
// Returns: (back_month_iv - front_month_iv) / front_month_iv
// Positive = contango, Negative = backwardation
static double compute_term_structure(sqlite3 *db, int64_t ts) {
    double front_iv = 0, back_iv = 0;
    int n = 0;
    
    time_t now = time(NULL);
    char today_str[16];
    struct tm *tm = gmtime(&now);
    strftime(today_str, sizeof(today_str), "%Y-%m-%d", tm);
    
    // Get ATM IV per expiry, ordered by date, skipping past expiries
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT expiry, "
        "  AVG(CASE WHEN abs(delta) BETWEEN 0.40 AND 0.60 THEN iv ELSE NULL END) as atm_iv "
        "FROM options WHERE ts = ? AND abs(delta) BETWEEN 0.40 AND 0.60 "
        "  AND expiry >= ? "
        "GROUP BY expiry ORDER BY expiry LIMIT 60;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, today_str, -1, SQLITE_STATIC);
    
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        double iv = sqlite3_column_double(stmt, 1);
        if (iv <= 0) continue;
        if (count == 0) front_iv = iv;
        back_iv = iv;
        count++;
    }
    sqlite3_finalize(stmt);
    
    if (count < 2 || front_iv <= 0) return 0;
    return (back_iv - front_iv) / front_iv;
}

// Compute IV skew: OTM put IV - OTM call IV for nearest expiry
static double compute_iv_skew(sqlite3 *db, int64_t ts) {
    // Get nearest forward expiry (>= today) with sufficient options
    time_t now = time(NULL);
    char today_str[16];
    struct tm *tm = gmtime(&now);
    strftime(today_str, sizeof(today_str), "%Y-%m-%d", tm);
    
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT expiry FROM options WHERE ts = ? AND type IS NOT NULL "
        "  AND expiry >= ? "
        "GROUP BY expiry HAVING COUNT(*) > 10 ORDER BY expiry LIMIT 1;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, today_str, -1, SQLITE_STATIC);
    
    char expiry[32] = "";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *e = (const char*)sqlite3_column_text(stmt, 0);
        if (e) strncpy(expiry, e, 31);
    }
    sqlite3_finalize(stmt);
    
    if (strlen(expiry) == 0) return 0;
    
    // OTM puts: delta between -0.20 and -0.30 (25-delta puts)
    double put_iv = 0;
    sqlite3_prepare_v2(db,
        "SELECT AVG(iv) FROM options WHERE ts = ? AND expiry = ? "
        "AND type = 'put' AND delta < -0.20 AND delta > -0.30;",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, expiry, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) put_iv = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    
    // OTM calls: delta between 0.20 and 0.30 (25-delta calls)
    double call_iv = 0;
    sqlite3_prepare_v2(db,
        "SELECT AVG(iv) FROM options WHERE ts = ? AND expiry = ? "
        "AND type = 'call' AND delta > 0.20 AND delta < 0.30;",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, expiry, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) call_iv = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    
    if (put_iv <= 0 || call_iv <= 0) return 0;
    return put_iv - call_iv;
}

// Compute ATM implied move (straddle mid / underlying price)
static double compute_implied_move(sqlite3 *db, int64_t ts) {
    time_t now = time(NULL);
    char today_str[16];
    struct tm *tm = gmtime(&now);
    strftime(today_str, sizeof(today_str), "%Y-%m-%d", tm);
    
    // Get nearest forward expiry
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT expiry FROM options WHERE ts = ? AND type IS NOT NULL "
        "  AND expiry >= ? "
        "GROUP BY expiry HAVING COUNT(*) > 10 ORDER BY expiry LIMIT 1;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, today_str, -1, SQLITE_STATIC);
    
    char expiry[32] = "";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *e = (const char*)sqlite3_column_text(stmt, 0);
        if (e) strncpy(expiry, e, 31);
    }
    sqlite3_finalize(stmt);
    
    if (strlen(expiry) == 0) return 0;
    
    // Get underlying price
    double underlying = 0;
    sqlite3_prepare_v2(db,
        "SELECT underlying FROM snapshots ORDER BY ts DESC LIMIT 1;",
        -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) underlying = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    
    if (underlying <= 0) return 0;
    
    // ATM straddle: find call and put closest to money, average bid+ask
    // Call closest to money
    double call_mid = 0, put_mid = 0;
    
    sqlite3_prepare_v2(db,
        "SELECT (bid + ask) / 2.0 FROM options WHERE ts = ? AND expiry = ? "
        "AND type = 'call' ORDER BY abs(strike - ?) LIMIT 1;",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, expiry, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, underlying);
    if (sqlite3_step(stmt) == SQLITE_ROW) call_mid = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    
    sqlite3_prepare_v2(db,
        "SELECT (bid + ask) / 2.0 FROM options WHERE ts = ? AND expiry = ? "
        "AND type = 'put' ORDER BY abs(strike - ?) LIMIT 1;",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, expiry, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, underlying);
    if (sqlite3_step(stmt) == SQLITE_ROW) put_mid = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    
    if (call_mid <= 0 || put_mid <= 0) return 0;
    
    double straddle = call_mid + put_mid;
    return straddle / underlying; // implied move as fraction
}

// Compute put/call volume ratio for nearest expiry
static double compute_pcr_flow(sqlite3 *db, int64_t ts) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT pcr_vol, pcr_oi FROM snapshots ORDER BY ts DESC LIMIT 1;",
        -1, &stmt, NULL);
    double pcr_vol = 0, pcr_oi = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        pcr_vol = sqlite3_column_double(stmt, 0);
        pcr_oi = sqlite3_column_double(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return pcr_vol > 0 ? pcr_vol : 0;
}

// Compute mean IV for near-term (≤7d) and next-term (8-30d) expiries
static void compute_iv_terms(sqlite3 *db, int64_t ts,
                              double *near_iv, double *next_iv, double *far_iv) {
    *near_iv = *next_iv = *far_iv = 0;
    int n_near = 0, n_next = 0, n_far = 0;
    
    time_t now = time(NULL);
    char today_str[16];
    struct tm *tm = gmtime(&now);
    strftime(today_str, sizeof(today_str), "%Y-%m-%d", tm);
    
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT expiry, AVG(iv) FROM options "
        "WHERE ts = ? AND abs(delta) BETWEEN 0.40 AND 0.60 "
        "  AND expiry >= ? "
        "GROUP BY expiry ORDER BY expiry;",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, today_str, -1, SQLITE_STATIC);
    
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *expiry_str = (const char*)sqlite3_column_text(stmt, 0);
        double iv = sqlite3_column_double(stmt, 1);
        if (!expiry_str || iv <= 0) continue;
        
        // Parse expiry date
        int y, m, d;
        if (sscanf(expiry_str, "%d-%d-%d", &y, &m, &d) != 3) continue;
        
        // Days until expiry
        struct tm exp_tm = {0};
        exp_tm.tm_year = y - 1900;
        exp_tm.tm_mon = m - 1;
        exp_tm.tm_mday = d;
        time_t exp_ts = timegm(&exp_tm);
        double days = difftime(exp_ts, now) / 86400.0;
        
        if (idx == 0) *near_iv = iv; // Closest expiry always
        if (days <= 7) { *near_iv = iv; n_near++; }
        else if (days <= 30) { *next_iv += iv; n_next++; }
        else if (idx < 8) { *far_iv += iv; n_far++; }
        idx++;
    }
    sqlite3_finalize(stmt);
    
    if (n_next > 0) *next_iv /= n_next;
    if (n_far > 0) *far_iv /= n_far;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s/SPY_flows.db", DB_DIR);
    
    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Can't open DB: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    
    // Get latest snapshot timestamp
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT ts FROM snapshots ORDER BY ts DESC LIMIT 1;", -1, &stmt, NULL);
    int64_t ts = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) ts = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    
    if (ts == 0) {
        fprintf(stderr, "No snapshots in DB\n");
        sqlite3_close(db);
        return 1;
    }
    
    // Compute features
    double term_struct_slope = compute_term_structure(db, ts);
    double iv_skew = compute_iv_skew(db, ts);
    double impl_move = compute_implied_move(db, ts);
    double pcr_vol = compute_pcr_flow(db, ts);
    
    double near_iv = 0, next_iv = 0, far_iv = 0;
    compute_iv_terms(db, ts, &near_iv, &next_iv, &far_iv);
    
    // Load existing features JSON if present
    json_t *root = NULL;
    FILE *f = fopen(OUT_FILE, "r");
    if (f) {
        json_error_t err;
        root = json_loadf(f, 0, &err);
        fclose(f);
    }
    if (!root) root = json_object();
    
    // Always set computed fields
    json_object_set_new(root, "iv_term_slope", json_real(term_struct_slope));
    json_object_set_new(root, "iv_skew", json_real(iv_skew));
    json_object_set_new(root, "atm_impl_move", json_real(impl_move));
    json_object_set_new(root, "pcr_vol", json_real(pcr_vol));
    json_object_set_new(root, "near_iv", json_real(near_iv));
    json_object_set_new(root, "next_iv", json_real(next_iv));
    json_object_set_new(root, "far_iv", json_real(far_iv));
    json_object_set_new(root, "source", json_string("cboe_options_chain"));
    
    // Write output
    FILE *out = fopen(OUT_FILE, "w");
    if (!out) {
        fprintf(stderr, "Can't write %s\n", OUT_FILE);
        json_decref(root);
        sqlite3_close(db);
        return 1;
    }
    json_dumpf(root, out, JSON_INDENT(2) | JSON_SORT_KEYS);
    fclose(out);
    json_decref(root);
    sqlite3_close(db);
    
    printf("Wrote %s\n", OUT_FILE);
    printf("  iv_term_slope: %.4f (%.0f%% contango)\n", term_struct_slope, term_struct_slope * 100);
    printf("  iv_skew:       %.4f (%.1f%% put premium)\n", iv_skew, iv_skew * 100);
    printf("  atm_impl_move: %.4f (%.2f%%)\n", impl_move, impl_move * 100);
    printf("  pcr_vol:       %.2f\n", pcr_vol);
    printf("  near_iv:       %.4f | next_iv: %.4f | far_iv: %.4f\n", near_iv, next_iv, far_iv);
    
    return 0;
}
