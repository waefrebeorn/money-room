/*
 * iv_rank.c — CB-STOCK: IV Rank & Percentile Calculator
 *
 * Reads options flow SQLite databases from ~/.hermes/options_cache/
 * and computes implied volatility rank and percentile per ticker.
 *
 * IV Rank = (current_IV - 52w_low) / (52w_high - 52w_low)
 * IV Percentile = percent of days IV was lower than current
 *
 * Build: gcc iv_rank.c -o iv_rank -lsqlite3 -lm -O2
 * Output: docs/data/iv_rank.json
 *
 * Requires: options_flow.c to have been running for 52 weeks of data.
 * With less history, reports available data range.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sqlite3.h>

#define OPTIONS_DIR "/home/wubu2/.hermes/options_cache"
#define OUTPUT_PATH "../docs/data/iv_rank.json"
#define YEAR_SECONDS (365 * 86400LL)

/* ── Ticker DB suffix ── */
static int is_flow_db(const char *name) {
    int len = strlen(name);
    return (len > 9 && strcmp(name + len - 9, "_flows.db") == 0);
}

/* ── Helper: extract ticker from "SPY_flows.db" ── */
static void extract_ticker(const char *name, char *out, int sz) {
    int i;
    for (i = 0; name[i] && name[i] != '_' && i < sz - 1; i++)
        out[i] = name[i];
    out[i] = '\0';
}

/* ── Compute IV stats for one ticker DB ── */
static int compute_ticker(const char *db_path, const char *ticker,
                          double *out_current, double *out_high, double *out_low,
                          double *out_percentile, int *out_days, int *out_options)
{
    (void)ticker;
    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return 0;

    int64_t cutoff = time(NULL) - YEAR_SECONDS;

    // Get current IV (latest snapshot, ATM nearest)
    sqlite3_stmt *st;
    const char *sql = "SELECT iv FROM options "
        "WHERE ts = (SELECT MAX(ts) FROM options) "
        "AND strike > 1 AND iv > 0.01 AND iv < 2.0 "  // sane IV range (1%-200%)
        "ORDER BY ABS(strike - (SELECT underlying FROM snapshots "
        "  WHERE ts = (SELECT MAX(ts) FROM options))) "
        "LIMIT 1";
    double current_iv = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            current_iv = sqlite3_column_double(st, 0);
        sqlite3_finalize(st);
    }

    // Get 52-week high/low IV (all contracts, all snapshots) and time range
    double high_iv = 0, low_iv = 100;
    int days = 0, total_opts = 0;
    int64_t min_ts = 0, max_ts = 0;
    sql = "SELECT MAX(iv), MIN(iv), COUNT(DISTINCT ts), COUNT(*), "
          "MIN(ts), MAX(ts) "
          "FROM options WHERE ts >= ? AND strike > 1 AND iv > 0.01 AND iv < 2.0";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, cutoff);
        if (sqlite3_step(st) == SQLITE_ROW) {
            high_iv = sqlite3_column_double(st, 0);
            low_iv = sqlite3_column_double(st, 1);
            days = sqlite3_column_int(st, 2);
            total_opts = sqlite3_column_int(st, 3);
            min_ts = sqlite3_column_int64(st, 4);
            max_ts = sqlite3_column_int64(st, 5);
        }
        sqlite3_finalize(st);
    }

    // Actual calendar span
    int calendar_days = (max_ts > min_ts) ? (int)((max_ts - min_ts) / 86400) : 0;

    // Compute IV percentile: percent of historical IVs below current
    double percentile = 0;
    if (days > 0 && current_iv > 0) {
        // Count how many option records have IV < current_iv
        sql = "SELECT COUNT(*) FROM options WHERE ts >= ? AND strike > 1 AND iv > 0.01 AND iv < 2.0 AND iv < ?";
        int below = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, cutoff);
            sqlite3_bind_double(st, 2, current_iv);
            if (sqlite3_step(st) == SQLITE_ROW) below = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
        if (total_opts > 0)
            percentile = (double)below / (double)total_opts * 100.0;
    }

    sqlite3_close(db);

    *out_current = current_iv;
    *out_high = (high_iv > 0 && high_iv < 100) ? high_iv : 0;
    *out_low = (low_iv > 0 && low_iv < 100) ? low_iv : 0;
    *out_percentile = percentile;
    *out_days = days;
    *out_options = total_opts;
    return (days > 0) ? calendar_days : 0;
}

static int64_t now_ts(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

int main(void) {
    DIR *dir = opendir(OPTIONS_DIR);
    if (!dir) {
        fprintf(stderr, "Cannot open %s\n", OPTIONS_DIR);
        return 1;
    }

    // Capture output to string for file write
    // Simple approach: use a temp file or just print and redirect
    // We print to stdout — use shell redirection in wrapper
    printf("{\n  \"iv_rank\": [\n");
    int first = 1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_flow_db(entry->d_name)) continue;

        char ticker[16];
        extract_ticker(entry->d_name, ticker, sizeof(ticker));

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", OPTIONS_DIR, entry->d_name);

        double cur, high, low, pct;
        int days, opts, cal_days;
        cal_days = compute_ticker(path, ticker, &cur, &high, &low, &pct, &days, &opts);
        if (cal_days <= 0) continue;

        double rank = (high > low && cur > 0) ? (cur - low) / (high - low) * 100.0 : 0;
        int64_t ts = now_ts();

        if (!first) printf(",");
        first = 0;
        printf("    {\n");
        printf("      \"ticker\": \"%s\",\n", ticker);
        printf("      \"current_iv\": %.2f,\n", cur);
        printf("      \"iv_52w_high\": %.2f,\n", high);
        printf("      \"iv_52w_low\": %.2f,\n", low);
        printf("      \"iv_rank\": %.1f,\n", rank);
        printf("      \"iv_percentile\": %.1f,\n", pct);
        printf("      \"snapshots\": %d,\n", days);
        printf("      \"calendar_days\": %d,\n", cal_days);
        printf("      \"options_scanned\": %d,\n", opts);
        printf("      \"data_note\": \"%s\",\n",
               cal_days < 7 ? "insufficient history for 52w rank" :
               cal_days < 30 ? "partial data (<1 month)" :
               cal_days < 180 ? "moderate data (<6 months)" :
               "52-week window covered");
        printf("      \"updated_at\": %lld\n", (long long)ts);
        printf("    }");
    }
    closedir(dir);

    printf("\n  ],\n  \"generated_at\": %lld\n}\n", (long long)now_ts());

    return 0;
}
