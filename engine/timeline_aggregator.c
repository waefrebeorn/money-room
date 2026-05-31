/**
 * timeline_aggregator.c — Rebuilds timeline_hourly from timeline
 *
 * Scans the timeline table for the last N hours and aggregates
 * rows into hourly buckets by source. Fixes stale hourly data.
 *
 * Build: gcc -O2 -o timeline_aggregator timeline_aggregator.c -lsqlite3 -lm
 * Usage: ./timeline_aggregator [hours_back] [--continuous]
 *   hours_back: how many hours to re-process (default: 24)
 *   --continuous: run forever, sleeping 3600s between passes
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>

#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"

/* Buffer sizes */
#define MAX_SOURCE 256
#define MAX_TIMELINE_HOURLY 2048
#define JSON_BUF 65536

typedef struct {
    char source[MAX_SOURCE];
    time_t ts_hour;    /* truncated to hour */
    char data[JSON_BUF];
    long long collected_at;
} HourlyRow;

static int hourly_count = 0;
static HourlyRow hourly_rows[MAX_TIMELINE_HOURLY];

/* SQLite callback: collect hourly rows from raw timeline */
static int collect_raw_cb(void *data, int argc, char **argv, char **colnames) {
    (void)data;
    if (hourly_count >= MAX_TIMELINE_HOURLY) return 0;
    
    HourlyRow *r = &hourly_rows[hourly_count];
    
    if (argv[0]) strncpy(r->source, argv[0], MAX_SOURCE-1);
    r->source[MAX_SOURCE-1] = '\0';
    
    if (argv[1]) {
        time_t ts = atol(argv[1]);
        r->ts_hour = (ts / 3600) * 3600;  /* truncate to hour */
    }
    
    /* Build compact JSON from remaining columns */
    char buf[JSON_BUF];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{");
    for (int i = 2; i < argc; i++) {
        if (i > 2) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        if (argv[i]) {
            char *safe = NULL;
            /* Simple JSON escaping for the value */
            size_t vlen = strlen(argv[i]);
            char *escaped = malloc(vlen * 2 + 1);
            if (!escaped) continue;
            int epos = 0;
            for (size_t j = 0; j < vlen; j++) {
                char c = argv[i][j];
                if (c == '"' || c == '\\') { escaped[epos++] = '\\'; escaped[epos++] = c; }
                else if (c < 0x20) { escaped[epos++] = ' '; }
                else { escaped[epos++] = c; }
            }
            escaped[epos] = '\0';
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\"%s\":\"%s\"", 
                          colnames[i] ? colnames[i] : "val", escaped);
            free(escaped);
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\"%s\":null",
                          colnames[i] ? colnames[i] : "val");
        }
        if (pos >= (int)sizeof(buf) - 100) break;
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "}");
    strncpy(r->data, buf, JSON_BUF-1);
    r->data[JSON_BUF-1] = '\0';
    
    r->collected_at = (long long)time(NULL);
    hourly_count++;
    return 0;
}

static int process_hours(sqlite3 *db, int hours_back, int verbose) {
    time_t now = time(NULL);
    time_t cutoff = now - (hours_back * 3600);
    char sql[4096];
    int rc;
    char *err = NULL;
    
    /* Remove existing entries for this time range */
    snprintf(sql, sizeof(sql),
        "DELETE FROM timeline_hourly WHERE ts >= %lld",
        (long long)(cutoff / 3600 * 3600));
    /* Retry up to 5 times on lock */
    for (int retry = 0; retry < 5; retry++) {
        rc = sqlite3_exec(db, sql, NULL, NULL, &err);
        if (rc == SQLITE_OK) break;
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            if (err) sqlite3_free(err);
            err = NULL;
            usleep(100000 * (retry + 1));  /* 100ms, 200ms, 300ms... */
            continue;
        }
        fprintf(stderr, "DELETE error: %s\n", err);
        sqlite3_free(err);
        return 1;
    }
    if (verbose) printf("  Cleared existing hourly records from cutoff\n");
    
    /* Get column list from timeline */
    snprintf(sql, sizeof(sql), "PRAGMA table_info(timeline)");
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    
    /* Collect raw timeline data — build SQL first, then retry on lock */
    snprintf(sql, sizeof(sql),
        "SELECT source, ts, category, data FROM timeline WHERE ts >= %lld ORDER BY source, ts",
        (long long)cutoff);
    
    /* Retry raw SELECT on lock */
    for (int retry = 0; retry < 5; retry++) {
        hourly_count = 0;
        rc = sqlite3_exec(db, sql, collect_raw_cb, NULL, &err);
        if (rc == SQLITE_OK) break;
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            if (err) sqlite3_free(err);
            err = NULL;
            usleep(100000 * (retry + 1));
            continue;
        }
        fprintf(stderr, "SELECT error: %s\n", err);
        sqlite3_free(err);
        return 1;
    }
    
    if (verbose) printf("  Collected %d raw rows from timeline\n", hourly_count);
    
    /* Insert into timeline_hourly */
    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);
    
    int inserted = 0;
    for (int i = 0; i < hourly_count; i++) {
        HourlyRow *r = &hourly_rows[i];
        snprintf(sql, sizeof(sql),
            "INSERT OR REPLACE INTO timeline_hourly (source, ts, data, collected_at) "
            "VALUES ('%s', %lld, '%s', %lld)",
            r->source, (long long)r->ts_hour, r->data, r->collected_at);
        
        /* Escape single quotes in source and data */
        /* This is a simplified approach — for production, use parameterized queries */
        rc = sqlite3_exec(db, sql, NULL, NULL, &err);
        if (rc == SQLITE_OK) inserted++;
        else {
            if (verbose > 1) fprintf(stderr, "  Insert error for %s @ %lld: %s\n",
                                     r->source, (long long)r->ts_hour, err);
            sqlite3_free(err);
        }
    }
    
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    
    if (verbose) printf("  Inserted %d hourly rows\n", inserted);
    return 0;
}

int main(int argc, char **argv) {
    int hours_back = 24;
    int continuous = 0;
    int verbose = 1;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--continuous") == 0 || strcmp(argv[i], "-c") == 0)
            continuous = 1;
        else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0)
            verbose = 0;
        else if (argv[i][0] >= '0' && argv[i][0] <= '9')
            hours_back = atoi(argv[i]);
    }
    
    sqlite3_initialize();
    
    if (verbose) {
        time_t now = time(NULL);
        char buf[32];
        struct tm *tm = localtime(&now);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
        printf("Timeline Aggregator — %s\n", buf);
        printf("Processing last %d hours%s\n\n", hours_back, continuous ? " (continuous mode)" : "");
    }
    
    sqlite3 *db = NULL;
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open DB: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    
    /* Create table if not exists */
    char *err = NULL;
    const char *create = "CREATE TABLE IF NOT EXISTS timeline_hourly ("
        "source TEXT NOT NULL, ts INTEGER NOT NULL, data TEXT, "
        "collected_at INTEGER, PRIMARY KEY (source, ts))";
    sqlite3_exec(db, create, NULL, NULL, &err);
    if (err) {
        fprintf(stderr, "Create table error: %s\n", err);
        sqlite3_free(err);
    }
    
    do {
        int result = process_hours(db, hours_back, verbose);
        if (result != 0) {
            fprintf(stderr, "Processing failed\n");
            break;
        }
        
        if (continuous) {
            if (verbose) printf("\nSleeping 3600s...\n");
            sleep(3600);
        }
    } while (continuous);
    
    sqlite3_close(db);
    
    if (verbose) {
        /* Show final state */
        sqlite3_open(DB_PATH, &db);
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT COUNT(*), MAX(ts) FROM timeline_hourly", -1, &stmt, NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int cnt = sqlite3_column_int(stmt, 0);
            time_t mx = sqlite3_column_int64(stmt, 1);
            char buf[32];
            struct tm *tm = localtime(&mx);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
            printf("\nFinal: %d hourly rows, latest bucket = %s\n", cnt, buf);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
    
    return 0;
}
