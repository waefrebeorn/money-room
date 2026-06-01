/*
 * bitstamp_fixer.c — Backfill bitstamp_1min entries from kraken data
 * Reads kraken_btc entries from timeline.db, converts pair naming,
 * writes as bitstamp_1min entries so all downstream consumers work.
 *
 * gcc -O2 -o bitstamp_fixer bitstamp_fixer.c -lcurl -ljansson -lsqlite3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <jansson.h>
#include <math.h>

#define DB_TIMELINE "/home/wubu2/.hermes/pm_logs/timeline.db"
#define DB_HISTORICAL "/home/wubu2/.hermes/pm_logs/historical/historical.db"

int main(void) {
    sqlite3 *db_hist, *db_tl;

    /* Open source: historical.db candles_multi (has fresh kraken data) */
    if (sqlite3_open(DB_HISTORICAL, &db_hist) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s\n", DB_HISTORICAL);
        return 1;
    }
    /* Open target: timeline.db */
    if (sqlite3_open(DB_TIMELINE, &db_tl) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s\n", DB_TIMELINE);
        sqlite3_close(db_hist);
        return 1;
    }

    /* Get last bitstamp_1min timestamp from timeline.db */
    long long last_ts = 0;
    {
        sqlite3_stmt *stmt;
        const char *sql = "SELECT MAX(ts) FROM timeline WHERE source='bitstamp_1min'";
        if (sqlite3_prepare_v2(db_tl, sql, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                last_ts = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    time_t now = time(NULL);
    printf("bitstamp_fixer: last_ts=%lld (%s), now=%ld\n",
           last_ts, last_ts ? ctime((time_t*)&last_ts) : "never", (long)now);

    /* Get fresh BTC/1min candles from historical.db candles_multi after last_ts */
    const char *select_sql = "SELECT ts, close, volume FROM candles_multi "
                             "WHERE pair='BTC' AND interval=60 AND ts > ?1 "
                             "ORDER BY ts ASC";
    sqlite3_stmt *sel;
    if (sqlite3_prepare_v2(db_hist, select_sql, -1, &sel, NULL) != SQLITE_OK) {
        fprintf(stderr, "Select prepare error: %s\n", sqlite3_errmsg(db_hist));
        sqlite3_close(db_hist);
        sqlite3_close(db_tl);
        return 1;
    }
    sqlite3_bind_int64(sel, 1, last_ts);

    /* Insert/Replace into timeline.db */
    const char *insert_sql = "INSERT OR REPLACE INTO timeline (ts, source, category, data, collected_at) "
                             "VALUES (?1, 'bitstamp_1min', 'crypto', ?2, ?3)";
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db_tl, insert_sql, -1, &ins, NULL) != SQLITE_OK) {
        fprintf(stderr, "Insert prepare error: %s\n", sqlite3_errmsg(db_tl));
        sqlite3_finalize(sel);
        sqlite3_close(db_hist);
        sqlite3_close(db_tl);
        return 1;
    }

    sqlite3_exec(db_tl, "BEGIN TRANSACTION", NULL, NULL, NULL);

    int count = 0, errors = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        long long ts = sqlite3_column_int64(sel, 0);
        double close = sqlite3_column_double(sel, 1);
        double volume = sqlite3_column_double(sel, 2);

        if (!isfinite(close) || close <= 0.0) { errors++; continue; }

        /* Build JSON: {"pair":"BTC","close":...,"volume":...} */
        char json_buf[256];
        snprintf(json_buf, sizeof(json_buf),
                 "{\"pair\":\"BTC\",\"close\":%.2f,\"volume\":%.4f}",
                 close, volume);

        sqlite3_bind_int64(ins, 1, ts);
        sqlite3_bind_text(ins, 2, json_buf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins, 3, (long long)now);

        int rc = sqlite3_step(ins);
        if (rc != SQLITE_DONE) errors++;
        sqlite3_reset(ins);
        count++;
    }

    sqlite3_exec(db_tl, "COMMIT", NULL, NULL, NULL);

    printf("bitstamp_fixer: %d entries written, %d errors\n", count, errors);
    printf("bitstamp_fixer: bitstamp_1min now spans to %s\n",
           count > 0 ? ctime((time_t*)&now) : "(no new data)");

    sqlite3_finalize(sel);
    sqlite3_finalize(ins);
    sqlite3_close(db_hist);
    sqlite3_close(db_tl);
    return errors > 0 ? 1 : 0;
}
