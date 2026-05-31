/* Minimal test: just load data and generate features */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>

typedef struct { long ts; double val; } TSPoint;

static double nearest_val(TSPoint *s, int n, long target, long max_delta) {
    if (!n) return -1;
    int lo = 0, hi = n - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (s[mid].ts <= target) lo = mid; else hi = mid - 1; }
    long d = llabs(s[lo].ts - target);
    if (lo+1 < n) { long d2 = llabs(s[lo+1].ts - target); if (d2 < d) { lo++; d = d2; } }
    return (d > max_delta) ? -1 : s[lo].val;
}

static int load_series(sqlite3 *db, const char *source, const char *json_key,
                       TSPoint *out, int max_n, const char *ts_col) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT %s, CAST(json_extract(data, '$.%s') AS REAL) as val "
        "FROM timeline WHERE source='%s' AND val IS NOT NULL ORDER BY %s ASC",
        ts_col, json_key, source, ts_col);
    sqlite3_stmt *stmt;
    int n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && n < max_n) {
            out[n].ts = (long)sqlite3_column_int64(stmt, 0);
            out[n].val = sqlite3_column_double(stmt, 1);
            n++;
        }
        sqlite3_finalize(stmt);
    }
    return n;
}

int main() {
    printf("Opening DB\n"); fflush(stdout);
    sqlite3 *db;
    if (sqlite3_open("/home/wubu2/.hermes/pm_logs/timeline.db", &db) != SQLITE_OK) return 1;
    
    printf("Loading SP500\n"); fflush(stdout);
    TSPoint sp[50000]; int n_sp = load_series(db, "fred_sp500", "value", sp, 50000, "ts");
    printf("SP500: %d pts\n", n_sp); fflush(stdout);
    
    TSPoint vix[50000]; int n_vix = load_series(db, "fred_vix", "value", vix, 50000, "ts");
    printf("VIX: %d pts\n", n_vix); fflush(stdout);
    
    sqlite3_close(db);
    printf("DB closed\n"); fflush(stdout);
    
    // Just compute 1 simple feature, no complexity
    double X[20000][21];
    double y[20000];
    int ns = 0;
    
    printf("Feature gen starting...\n"); fflush(stdout);
    for (int i = 60; i < n_sp - 1 && ns < 20000; i++) {
        double sp500 = sp[i].val;
        
        X[ns][0] = (sp[i-1].val > 0) ? log(sp500 / sp[i-1].val) : 0;
        y[ns] = (i+1 < n_sp && sp[i+1].val > sp500) ? 1.0 : 0.0;
        
        ns++;
        if (ns % 5000 == 0) { printf("  %d...\n", ns); fflush(stdout); }
    }
    printf("Generated %d samples\n", ns);
    return 0;
}
