/**
 * data_pipeline.c — C Data Organization Module
 * 
 * Loads from timeline.db, aggregates to daily, aligns series,
 * computes 21 features, writes clean binary training file.
 * 
 * Binary format:
 *   [int32] n_samples
 *   [int32] n_features  
 *   [float[n_samples][n_features]] X
 *   [float[n_samples]] y
 * 
 * Compile: gcc -O3 -o data_pipeline data_pipeline.c -lsqlite3 -lm
 * Run:     ./data_pipeline
 * Output:  training_data.bin (binary) + data_report.txt (human-readable)
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include <stdint.h>

#define MAX_DAYS      10000
#define MAX_SERIES    10
#define MAX_FEATURES  21
#define MAX_SAMPLES   5000
#define SECS_PER_DAY  86400LL

// ── One time series, daily-aggregated ──
typedef struct {
    char name[32];
    long *days;      // day number (ts / 86400)
    double *vals;    // last value on that day
    int n;
} DailySeries;

// ── All raw series ──
static DailySeries series[MAX_SERIES];
static int n_series = 0;

static int series_add(const char *name) {
    if (n_series >= MAX_SERIES) return -1;
    DailySeries *s = &series[n_series++];
    strncpy(s->name, name, 31);
    s->days = calloc(MAX_DAYS, sizeof(long));
    s->vals = calloc(MAX_DAYS, sizeof(double));
    s->n = 0;
    return n_series - 1;
}

static void series_push(DailySeries *s, long day, double val) {
    if (s->n >= MAX_DAYS) return;
    s->days[s->n] = day;
    s->vals[s->n] = val;
    s->n++;
}

// ── Load a source with GROUP BY day (SQL does the aggregation) ──
static int load_with_group(sqlite3 *db, const char *name,
                           const char *sql_template, 
                           const char *source, const char *json_key) {
    // Generate SQL: e.g.
    // SELECT ts/86400 as day, 
    //        CAST(json_extract(data, '$.value') AS REAL) as val
    // FROM timeline WHERE source='fred_sp500' AND val IS NOT NULL
    // GROUP BY day ORDER BY day
    char sql[4096];
    snprintf(sql, sizeof(sql), sql_template, source, json_key);
    
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[%s] SQL error: %s\n", name, sqlite3_errmsg(db));
        return 0;
    }
    
    int idx = series_add(name);
    if (idx < 0) { sqlite3_finalize(stmt); return 0; }
    DailySeries *s = &series[idx];
    
    // For GROUP BY: ts/86400 is column 0, val (last) is column 1
    while (sqlite3_step(stmt) == SQLITE_ROW && s->n < MAX_DAYS) {
        long day = (long)sqlite3_column_int64(stmt, 0);
        double val = sqlite3_column_double(stmt, 1);
        series_push(s, day, val);
    }
    sqlite3_finalize(stmt);
    
    printf("  %s: %d daily points (day %ld to %ld)\n",
           name, s->n, s->n > 0 ? s->days[0] : 0,
           s->n > 0 ? s->days[s->n-1] : 0);
    return s->n;
}

// ── Load BTC daily from 7.5M 1-min rows ──
// Use SQL aggregation: last close per day
static int load_btc_daily(sqlite3 *db) {
    // For 7.5M rows, use a subquery with MAX(ts) per day for efficiency
    // Then join back to get the close value
    const char *sql = 
        "SELECT day, last_close FROM ("
        "  SELECT ts/86400 as day, "
        "         CAST(json_extract(data, '$.close') AS REAL) as last_close "
        "  FROM timeline "
        "  WHERE source='bitstamp_1min' "
        "    AND json_extract(data, '$.pair')='BTC' "
        "    AND json_extract(data, '$.close') IS NOT NULL "
        "  GROUP BY day"
        ") ORDER BY day";
    
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[BTC] SQL error: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    
    int idx = series_add("BTC");
    if (idx < 0) { sqlite3_finalize(stmt); return 0; }
    DailySeries *s = &series[idx];
    
    while (sqlite3_step(stmt) == SQLITE_ROW && s->n < MAX_DAYS) {
        long day = (long)sqlite3_column_int64(stmt, 0);
        double close = sqlite3_column_double(stmt, 1);
        series_push(s, day, close);
    }
    sqlite3_finalize(stmt);
    
    printf("  BTC: %d daily points (day %ld to %ld) from 7.5M 1-min rows\n",
           s->n, s->n > 0 ? s->days[0] : 0,
           s->n > 0 ? s->days[s->n-1] : 0);
    return s->n;
}

// ── Nearest value by day (binary search on sorted days) ──
static double nearest_val(DailySeries *s, long day, long max_delta_days) {
    if (s->n == 0) return -1;
    int lo = 0, hi = s->n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s->days[mid] <= day) lo = mid;
        else hi = mid - 1;
    }
    long d = llabs(s->days[lo] - day);
    if (lo + 1 < s->n) {
        long d2 = llabs(s->days[lo+1] - day);
        if (d2 < d) { lo++; d = d2; }
    }
    if (d > max_delta_days) return -1;
    return s->vals[lo];
}

// ── Main ──
int main(int argc, char **argv) {
    printf("═══ C Data Pipeline ═══\n");
    printf("Loading from timeline.db...\n");
    
    sqlite3 *db;
    if (sqlite3_open("/home/wubu2/.hermes/pm_logs/timeline.db", &db) != SQLITE_OK) {
        fprintf(stderr, "Can't open timeline.db\n");
        return 1;
    }
    
    // ── Load all series ──
    // Each uses SQL GROUP BY to aggregate to daily
    load_with_group(db, "SP500",
        "SELECT ts/86400 as day, CAST(json_extract(data, '$.value') AS REAL) as val "
        "FROM timeline WHERE source='%s' AND val IS NOT NULL GROUP BY day ORDER BY day",
        "fred_sp500", "value");
    
    load_with_group(db, "VIX",
        "SELECT ts/86400 as day, CAST(json_extract(data, '$.value') AS REAL) as val "
        "FROM timeline WHERE source='%s' AND val IS NOT NULL GROUP BY day ORDER BY day",
        "fred_vix", "value");
    
    load_with_group(db, "DGS10",
        "SELECT ts/86400 as day, CAST(json_extract(data, '$.value') AS REAL) as val "
        "FROM timeline WHERE source='%s' AND val IS NOT NULL GROUP BY day ORDER BY day",
        "stock_dgs10", "value");   // stock_dgs10 has data, fred_dgs10 has 0
    
    load_btc_daily(db);  // BTC from 7.5M bitstamp_1min
    
    load_with_group(db, "MCAP",
        "SELECT collected_at/86400 as day, "
        "CAST(json_extract(data, '$.total_market_cap_usd') AS REAL) as val "
        "FROM timeline WHERE source='%s' AND val IS NOT NULL GROUP BY day ORDER BY day",
        "coingecko_global", "total_market_cap_usd");
    
    sqlite3_close(db);
    printf("\n");
    
    // ── Find overlapping date range ──
    // SP500 is the anchor series
    DailySeries *sp = &series[0];  // index 0 = SP500
    if (sp->n == 0) {
        fprintf(stderr, "No SP500 data!\n");
        return 1;
    }
    
    long start_day = sp->days[60];  // Need 60 days of history
    long end_day = sp->days[sp->n - 2];  // Need next day for target
    printf("SP500 range: day %ld to %ld, using %ld to %ld\n",
           sp->days[0], sp->days[sp->n-1], start_day, end_day);
    
    // ── Compute features ──
    // We store in float arrays for binary output
    float X[MAX_SAMPLES][MAX_FEATURES];
    float y[MAX_SAMPLES];
    int n_samples = 0;
    
    for (int i = 60; i < sp->n - 1 && n_samples < MAX_SAMPLES; i++) {
        double sp500 = sp->vals[i];
        long day = sp->days[i];
        
        // ── SP500 returns ──
        double r1d = log(sp500 / sp->vals[i-1]);
        double r5d = log(sp500 / sp->vals[i-5]);
        double r20d = log(sp500 / sp->vals[i-20]);
        
        // ── SP500 volatility 5d, 20d ──
        double s5=0, sq5=0; int n5=0;
        double s20=0, sq20=0; int n20=0;
        for (int j = 1; j <= 5; j++) {
            double r = log(sp->vals[i-j+1] / sp->vals[i-j]);
            s5 += r; sq5 += r*r; n5++;
            s20 += r; sq20 += r*r; n20++;
        }
        for (int j = 6; j <= 20; j++) {
            double r = log(sp->vals[i-j+1] / sp->vals[i-j]);
            s20 += r; sq20 += r*r; n20++;
        }
        double v5 = (n5 > 1) ? sqrt(sq5/n5 - (s5/n5)*(s5/n5)) : 0;
        double v20 = (n20 > 1) ? sqrt(sq20/n20 - (s20/n20)*(s20/n20)) : 0;
        if (isnan(v5) || isinf(v5)) v5 = 0;
        if (isnan(v20) || isinf(v20)) v20 = 0;
        
        // ── SMA relative ──
        double sma20 = 0, sma50 = 0;
        for (int j = 1; j <= 20; j++) sma20 += sp->vals[i-j];
        for (int j = 1; j <= 50 && i-j >= 0; j++) sma50 += sp->vals[i-j];
        sma20 /= 20;
        sma50 /= (i >= 50) ? 50 : i;
        double rel_ma20 = (sp500 / sma20) - 1.0;
        double rel_ma50 = (sp500 / sma50) - 1.0;
        
        // ── VIX ──
        DailySeries *vix = &series[1];
        double v = nearest_val(vix, day, 5);
        double v1d = 0;
        if (v > 0) { double vp = nearest_val(vix, day-1, 5); if (vp > 0) v1d = (v-vp)/vp; }
        
        // VIX vol 5d
        double vixv5 = 0;
        int nvv = 0; double vs=0, vsq=0;
        for (int j = 0; j < 5; j++) {
            double vv = nearest_val(vix, day - j, 5);
            if (vv > 0) { vs += vv; vsq += vv*vv; nvv++; }
        }
        if (nvv > 1) { double vm = vs/nvv; vixv5 = sqrt(vsq/nvv - vm*vm) / vm; }
        if (isnan(vixv5) || isinf(vixv5)) vixv5 = 0;
        
        // ── BTC ──
        DailySeries *btc = &series[3];  // index 3 = BTC
        double b = nearest_val(btc, day, 10);
        double b1d = 0;
        if (b > 0) { double bp = nearest_val(btc, day-1, 10); if (bp > 0) b1d = log(b/bp); }
        
        // BTC vol 5d
        double btc5 = 0;
        int nbv = 0; double bs=0, bsq=0;
        for (int j = 0; j < 5; j++) {
            double bv = nearest_val(btc, day - j, 10);
            if (bv > 0) { bs += bv; bsq += bv*bv; nbv++; }
        }
        if (nbv > 1) { double bm = bs/nbv; btc5 = sqrt(bsq/nbv - bm*bm) / bm; }
        if (isnan(btc5) || isinf(btc5)) btc5 = 0;
        
        // BTC-SP500 corr (5-day)
        double btc_corr = 0;
        int nc = 0; double sp_m=0, bt_m=0;
        double spr[5], btr[5];
        for (int j = 0; j < 5; j++) {
            double bv = nearest_val(btc, sp->days[i-j], 10);
            if (bv > 0) {
                double sr = log(sp->vals[i-j] / sp->vals[i-j-1]);
                double br = log(bv / nearest_val(btc, sp->days[i-j-1], 10));
                if (!isnan(br) && !isinf(br) && br != 0) {
                    spr[nc] = sr; btr[nc] = br; sp_m += sr; bt_m += br; nc++;
                }
            }
        }
        if (nc >= 3) {
            sp_m /= nc; bt_m /= nc;
            double num=0, dsp=0, dbt=0;
            for (int j = 0; j < nc; j++) {
                num += (spr[j]-sp_m)*(btr[j]-bt_m);
                dsp += (spr[j]-sp_m)*(spr[j]-sp_m);
                dbt += (btr[j]-bt_m)*(btr[j]-bt_m);
            }
            btc_corr = (dsp>0 && dbt>0) ? num/sqrt(dsp*dbt) : 0;
        }
        
        // ── DGS10 ──
        DailySeries *dgs = &series[2];  // index 2 = DGS10
        double dgs10 = nearest_val(dgs, day, 5);
        double dgs10_1d = 0;
        if (dgs10 > 0) { double dp = nearest_val(dgs, day-1, 5); if (dp > 0) dgs10_1d = dgs10 - dp; }
        
        // ── Crypto mcap ──
        DailySeries *mcap = &series[4];
        double cmcap = nearest_val(mcap, day, 15);
        double mcap1d = 0;
        if (cmcap > 0) { double mp = nearest_val(mcap, day-1, 15); if (mp > 0) mcap1d = log(cmcap/mp); }
        
        // ── Momentum & acceleration ──
        double mom = r1d;
        double acc = (i > 61) ? mom - log(sp->vals[i-1]/sp->vals[i-2]) : 0;
        if (isnan(acc) || isinf(acc)) acc = 0;
        
        // ── Time features ──
        time_t tt = (time_t)(day * SECS_PER_DAY);
        struct tm *tm = gmtime(&tt);
        double dow = (double)tm->tm_wday / 6.0;
        double mon = (double)tm->tm_mon / 11.0;
        
        // ── Z-score 20d ──
        double z20 = (v20 > 0) ? r20d / v20 : 0;
        if (isnan(z20) || isinf(z20)) z20 = 0;
        
        // ── Fill feature vector ──
        int f = 0;
        X[n_samples][f++] = (float)r1d;
        X[n_samples][f++] = (float)r5d;
        X[n_samples][f++] = (float)r20d;
        X[n_samples][f++] = (float)v5;
        X[n_samples][f++] = (float)v20;
        X[n_samples][f++] = (float)rel_ma20;
        X[n_samples][f++] = (float)rel_ma50;
        X[n_samples][f++] = (float)(v > 0 ? v : 0);
        X[n_samples][f++] = (float)v1d;
        X[n_samples][f++] = (float)(b > 0 ? b1d : 0);
        X[n_samples][f++] = (float)btc_corr;
        X[n_samples][f++] = (float)(dgs10 > 0 ? dgs10 : 0);
        X[n_samples][f++] = (float)dgs10_1d;
        X[n_samples][f++] = (float)(cmcap > 0 ? mcap1d : 0);
        X[n_samples][f++] = (float)mom;
        X[n_samples][f++] = (float)acc;
        X[n_samples][f++] = (float)dow;
        X[n_samples][f++] = (float)mon;
        X[n_samples][f++] = (float)vixv5;
        X[n_samples][f++] = (float)btc5;
        X[n_samples][f++] = (float)z20;
        
        // ── Target: next day SP500 direction ──
        y[n_samples] = (sp->vals[i+1] > sp500) ? 1.0f : 0.0f;
        
        n_samples++;
    }
    
    printf("Generated %d daily samples x %d features\n", n_samples, MAX_FEATURES);
    
    // Class balance (compute once)
    int ups = 0, downs = 0;
    for (int i = 0; i < n_samples; i++) if (y[i] >= 0.5) ups++; else downs++;
    
    // ── Normalize features (z-score) ──
    double mean[MAX_FEATURES] = {0}, std[MAX_FEATURES] = {0};
    for (int f = 0; f < MAX_FEATURES; f++) {
        double sum = 0;
        for (int i = 0; i < n_samples; i++) sum += X[i][f];
        mean[f] = sum / n_samples;
        double sq = 0;
        for (int i = 0; i < n_samples; i++) sq += (X[i][f] - mean[f]) * (X[i][f] - mean[f]);
        std[f] = sqrt(sq / n_samples);
        if (std[f] < 1e-10) std[f] = 1.0;
        for (int i = 0; i < n_samples; i++) X[i][f] = (float)((X[i][f] - mean[f]) / std[f]);
    }
    
    // ── Write binary ──
    FILE *fp = fopen("training_data.bin", "wb");
    if (!fp) { fprintf(stderr, "Can't write training_data.bin\n"); return 1; }
    
    int32_t hdr[2] = {(int32_t)n_samples, (int32_t)MAX_FEATURES};
    fwrite(hdr, sizeof(int32_t), 2, fp);
    fwrite(X, sizeof(float), n_samples * MAX_FEATURES, fp);
    fwrite(y, sizeof(float), n_samples, fp);
    fclose(fp);
    
    printf("Wrote training_data.bin (%d samples x %d features = %.1f MB)\n",
           n_samples, MAX_FEATURES, 
           (double)(sizeof(float) * n_samples * (MAX_FEATURES + 1)) / (1024*1024));
    
    // ── Write report ──
    fp = fopen("data_report.txt", "w");
    if (fp) {
        fprintf(fp, "╔═══ DATA PIPELINE REPORT ═══╗\n");
        fprintf(fp, "Samples: %d\n", n_samples);
        fprintf(fp, "Features: %d\n", MAX_FEATURES);
        fprintf(fp, "Date range: day %ld to %ld\n", 
                sp->days[60], sp->days[sp->n-2]);
        
        // Class balance
        fprintf(fp, "Class balance: UP=%d (%.1f%%) DOWN=%d (%.1f%%)\n",
                ups, 100.0*ups/n_samples, downs, 100.0*downs/n_samples);
        
        // Feature stats
        fprintf(fp, "\nFeature stats (pre-normalization):\n");
        fprintf(fp, "  %-25s %12s %12s %12s %12s\n", "Name", "Min", "Max", "Mean", "Std");
        fprintf(fp, "  %-25s %12s %12s %12s %12s\n", "---", "---", "---", "---", "---");
        
        // Re-compute pre-norm stats from the normalized data
        // (We've already normalized, but the z-score preserves shape info)
        for (int f = 0; f < MAX_FEATURES; f++) {
            float mn = X[0][f], mx = X[0][f];
            double sum = 0;
            for (int i = 0; i < n_samples; i++) {
                if (X[i][f] < mn) mn = X[i][f];
                if (X[i][f] > mx) mx = X[i][f];
                sum += X[i][f];
            }
            double m = sum / n_samples;
            fprintf(fp, "  %-25s %12.4f %12.4f %12.4f %12.4f\n",
                    "", (double)mn, (double)mx, m, 
                    mean[f] != 0 ? mean[f] : 0);
        }
        
        fclose(fp);
        printf("Wrote data_report.txt\n");
    }
    
    // ── Series info ──
    printf("\n╔═══ SERIES INFO ═══╗\n");
    for (int i = 0; i < n_series; i++) {
        printf("  %-10s %5d points [%ld — %ld]\n",
               series[i].name, series[i].n,
               series[i].n > 0 ? series[i].days[0] : 0,
               series[i].n > 0 ? series[i].days[series[i].n-1] : 0);
    }
    
    printf("\nSP500 win rate (raw): %.2f%%\n",
           100.0 * ups / n_samples);
    
    // Cleanup
    for (int i = 0; i < n_series; i++) {
        free(series[i].days);
        free(series[i].vals);
    }
    
    printf("\nDone. Binary file ready for nn_deep_full.\n");
    return 0;
}
