/**
 * populate_btc_db.c — Populate historical.db candles_multi with BTC/USD from CSV
 * Reads btc_1min_latest.csv, inserts into pm_logs/historical/historical.db
 */
#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <math.h>

#define BTC_CSV   "/home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv"
#define HIST_DB   "/home/wubu2/.hermes/pm_logs/historical/historical.db"

static sqlite3 *db;
static int total = 0, inserted = 0, skipped = 0;

int main(void) {
    printf("[BTC-DB] Opening CSV: %s\n", BTC_CSV);
    FILE *f = fopen(BTC_CSV, "r");
    if (!f) { fprintf(stderr, "ERROR: Cannot open BTC CSV\n"); return 1; }

    if (sqlite3_open(HIST_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "ERROR: Cannot open historical.db: %s\n", sqlite3_errmsg(db));
        fclose(f); return 1;
    }

    char *err = NULL;
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS candles_multi ("
        "ts INTEGER, pair TEXT, open REAL, high REAL, low REAL, "
        "close REAL, volume REAL, source TEXT, "
        "PRIMARY KEY (ts, pair))", NULL, NULL, &err);
    if (err) { fprintf(stderr, "SQL error: %s\n", err); sqlite3_free(err); }

    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    char buf[512];
    int lineno = 0;
    while (fgets(buf, sizeof(buf), f)) {
        lineno++;
        if (lineno == 1) continue; // skip header

        int64_t ts; double open, high, low, close, vol;
        if (sscanf(buf, "%ld,%lf,%lf,%lf,%lf,%lf", &ts, &open, &high, &low, &close, &vol) < 6) continue;

        // Validate — skip bad data
        if (close <= 0.0 || open <= 0.0 || vol < 0.0) { skipped++; continue; }
        if (fabs(close - open) / open > 0.5) { skipped++; continue; } // >50% move = bad tick

        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO candles_multi(pair,interval,ts,open,high,low,close,volume,trades) "
            "VALUES('BTC',1,?1,?2,?3,?4,?5,?6,0)", -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, ts);
        sqlite3_bind_double(stmt, 2, open);
        sqlite3_bind_double(stmt, 3, high);
        sqlite3_bind_double(stmt, 4, low);
        sqlite3_bind_double(stmt, 5, close);
        sqlite3_bind_double(stmt, 6, vol);

        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) inserted++;
        sqlite3_finalize(stmt);
        total++;

        if (total % 50000 == 0) {
            sqlite3_exec(db, "COMMIT; BEGIN;", NULL, NULL, NULL);
            printf("[BTC-DB] %d rows processed, %d inserted, %d skipped\n", total, inserted, skipped);
        }
    }

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    fclose(f);
    sqlite3_close(db);

    printf("[BTC-DB] DONE: %d total, %d inserted, %d skipped\n", total, inserted, skipped);
    return 0;
}
