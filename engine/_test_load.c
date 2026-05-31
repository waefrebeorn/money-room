#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include <float.h>

#define MAX_SAMPLES 20000
#define MAX_FEATURES 30

typedef struct { long ts; double val; } TSPoint;

int main() {
    printf("Opening DB...\n"); fflush(stdout);
    sqlite3 *db;
    if (sqlite3_open("/home/wubu2/.hermes/pm_logs/timeline.db", &db) != SQLITE_OK) {
        printf("FAILED\n"); return 1;
    }
    
    // Single query, no CAST
    printf("Querying SP500...\n"); fflush(stdout);
    TSPoint sp[50000];
    int n = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT ts, data FROM timeline WHERE source='fred_sp500' ORDER BY ts ASC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) { printf("SQL error: %s\n", sqlite3_errmsg(db)); return 1; }
    
    printf("Stepping...\n"); fflush(stdout);
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < 50000) {
        sp[n].ts = (long)sqlite3_column_int64(stmt, 0);
        const char *d = (const char*)sqlite3_column_text(stmt, 1);
        // Quick approximate: skip json extract, just use ts for count
        (void)d;
        n++;
        count++;
        if (count % 10000 == 0) { printf("  %d rows...\n", count); fflush(stdout); }
    }
    sqlite3_finalize(stmt);
    printf("Loaded %d SP500 rows\n", n); fflush(stdout);
    
    // Now walk the data
    printf("Walking %d points...\n", n); fflush(stdout);
    double X[MAX_SAMPLES][MAX_FEATURES];
    double y[MAX_SAMPLES];
    int ns = 0;
    
    for (int i = 60; i < n - 1 && ns < MAX_SAMPLES; i++) {
        double val = 0;
        // Try extracting value from the 6th comma (approx position)
        // Actually just compute a simple feature
        if (i >= 1) {
            X[ns][0] = (double)(sp[i].ts - sp[i-1].ts) / 86400.0;
        }
        y[ns] = 0;
        ns++;
        if (ns % 5000 == 0) { printf("  %d samples...\n", ns); fflush(stdout); }
    }
    
    printf("Generated %d features for %d samples\n", 1, ns); fflush(stdout);
    sqlite3_close(db);
    printf("Done\n");
    return 0;
}
