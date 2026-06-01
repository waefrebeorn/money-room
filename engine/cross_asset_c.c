/**
 * cross_asset_c.c — Cross-asset correlation engine
 * Computes rolling Pearson correlations between BTC, SPY, VIX, yields.
 * Reads from timeline.db, writes to cache for engine features.
 *
 * Build: gcc -O2 cross_asset_c.c -o cross_asset_c -lsqlite3 -lm
 * Run:   ./cross_asset_c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sqlite3.h>
#include <time.h>
#include <sys/stat.h>

#define DB_PATH   "/home/wubu2/.hermes/pm_logs/timeline.db"
#define CACHE_DIR "/home/wubu2/.hermes/news_cache"
#define CACHE_PATH CACHE_DIR "/cross_asset.json"
#define HB_DIR    "/home/wubu2/.hermes/infra/heartbeats"
#define HB_PATH   HB_DIR "/cross-asset.heartbeat"

#define MAX_POINTS 50000
#define CORR_WINDOWS 3
#define CORR_DAYS (int[]){7, 30, 90}

typedef struct { double ts; double val; } Point;
typedef struct { Point *pts; int n; int cap; } Series;

static Series *series_new(void) {
    Series *s = calloc(1, sizeof(Series));
    s->cap = 1024;
    s->pts = malloc(s->cap * sizeof(Point));
    return s;
}

static void series_add(Series *s, double ts, double val) {
    if (s->n >= s->cap) {
        s->cap *= 2;
        s->pts = realloc(s->pts, s->cap * sizeof(Point));
    }
    s->pts[s->n].ts = ts;
    s->pts[s->n].val = val;
    s->n++;
}

static int compare_ts(const void *a, const void *b) {
    double ta = ((Point*)a)->ts;
    double tb = ((Point*)b)->ts;
    return (ta > tb) - (ta < tb);
}

/* Load numeric data from timeline.db for a given source */
static Series *load_series(const char *source) {
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open DB: %s\n", sqlite3_errmsg(db));
        return NULL;
    }

    Series *s = series_new();
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT ts, data FROM timeline WHERE source=?1 ORDER BY ts ASC");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Prepare error: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return s;
    }

    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_STATIC);
    int rc;
    rc = sqlite3_step(stmt);
    while (rc == SQLITE_ROW) {
        double ts = sqlite3_column_double(stmt, 0);
        const char *data = (const char*)sqlite3_column_text(stmt, 1);
        if (!data) { rc = sqlite3_step(stmt); continue; }

        /* Try to extract a numeric value from JSON data field */
        double val = 0;
        const char *close_str = strstr(data, "\"close\":");
        if (close_str) {
            val = atof(close_str + 8);
        }
        /* Try "value" (FRED data format) */
        if (val <= 0) {
            const char *val_str = strstr(data, "\"value\":");
            if (val_str) val = atof(val_str + 8);
        }
        /* Try direct numeric parse */
        if (val <= 0) val = atof(data);

        if (val > 0) series_add(s, ts, val);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return s;
}

/* Pearson correlation between two series, daily 1:1 matching */
static double pearson_r(Series *a, Series *b, double window_days) {
    double now = time(NULL);
    double start = now - window_days * 86400;
    double end = now;

    double ax[4096], bx[4096];
    int n = 0;
    int ai = 0, bi = 0;

    /* For each point in b (usually the daily series), find nearest a */
    while (bi < b->n && n < 4096) {
        if (b->pts[bi].ts < start) { bi++; continue; }
        if (b->pts[bi].ts > end) break;

        /* Advance ai to just before bi's timestamp */
        while (ai < a->n - 1 && a->pts[ai + 1].ts < b->pts[bi].ts) ai++;
        if (ai >= a->n) break;

        /* Find nearest within 36h tolerance */
        double best_diff = 1e18;
        int best_idx = -1;
        for (int j = ai; j < a->n && j < ai + 2880; j++) {  /* scan 2 days ahead max */
            double diff = fabs(a->pts[j].ts - b->pts[bi].ts);
            if (diff > 129600) break;  /* >36h, stop */
            if (diff < best_diff) { best_diff = diff; best_idx = j; }
        }
        if (best_idx >= 0) {
            ax[n] = a->pts[best_idx].val;
            bx[n] = b->pts[bi].val;
            n++;
        }
        bi++;
    }

    if (n < 5) return 0.0;  /* Not enough samples */

    /* Compute Pearson r */
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0, sum_y2 = 0;
    for (int i = 0; i < n; i++) {
        sum_x += ax[i];
        sum_y += bx[i];
        sum_xy += ax[i] * bx[i];
        sum_x2 += ax[i] * ax[i];
        sum_y2 += bx[i] * bx[i];
    }

    double numer = n * sum_xy - sum_x * sum_y;
    double denom = sqrt((n * sum_x2 - sum_x * sum_x) * (n * sum_y2 - sum_y * sum_y));
    if (denom < 1e-10) return 0.0;

    return numer / denom;
}

/* Compute volatility (annualized std dev of log returns) */
static double calc_volatility(Series *s, double window_days) {
    if (!s || s->n < 5) return 0.0;
    double now = time(NULL);
    double start = now - window_days * 86400;

    /* Find first index within window */
    int first = -1;
    for (int i = 0; i < s->n; i++) {
        if (s->pts[i].ts >= start) { first = i; break; }
    }
    if (first < 1 || first >= s->n) return 0.0;
    if (s->pts[first].ts > now) return 0.0;

    double returns[4096];
    int n = 0;
    for (int i = first; i < s->n; i++) {
        if (s->pts[i].ts > now) break;
        if (i > first && s->pts[i].val > 0 && s->pts[i-1].val > 0) {
            returns[n++] = log(s->pts[i].val / s->pts[i-1].val);
            if (n >= 4096) break;
        }
    }
    if (n < 5) return 0.0;

    double mean = 0;
    for (int i = 0; i < n; i++) mean += returns[i];
    mean /= n;

    double var = 0;
    for (int i = 0; i < n; i++) var += (returns[i] - mean) * (returns[i] - mean);
    var /= (n - 1);

    /* Annualize: multiply by sqrt(365*24*60/interval) */
    /* Approximate as daily data: sqrt(365) */
    return sqrt(var) * sqrt(365.0);
}

int main(void) {
    printf("[cross_asset] Loading data from %s...\n", DB_PATH);

    Series *btc = load_series("bitstamp_1min");
    if (!btc || btc->n < 100) {
        if (btc) { free(btc->pts); free(btc); }
        btc = load_series("kraken_btc");
    }
    Series *spy = load_series("fred_sp500");
    Series *vix = load_series("fred_vix");

    printf("[cross_asset] Loaded: BTC=%d  SPY=%d  VIX=%d\n",
           btc ? btc->n : 0, spy ? spy->n : 0, vix ? vix->n : 0);

    /* Compute correlations */
    mkdir(CACHE_DIR, 0755);
    mkdir(HB_DIR, 0755);

    FILE *f = fopen(CACHE_PATH, "w");
    if (!f) { perror("fopen cache"); return 1; }

    fprintf(f, "{\n");
    fprintf(f, "  \"computed_at\": %ld,\n", time(NULL));

    /* BTC-SPY correlation */
    fprintf(f, "  \"btc_spy_corr\": {\n");
    for (int i = 0; i < CORR_WINDOWS; i++) {
        double r = pearson_r(btc, spy, CORR_DAYS[i]);
        fprintf(f, "    \"%dd\": %.4f%s\n", CORR_DAYS[i], r,
                i < CORR_WINDOWS - 1 ? "," : "");
    }
    fprintf(f, "  },\n");

    /* BTC volatility */
    fprintf(f, "  \"btc_volatility\": {\n");
    for (int i = 0; i < CORR_WINDOWS; i++) {
        double vol = calc_volatility(btc, CORR_DAYS[i]);
        fprintf(f, "    \"%dd\": %.4f%s\n", CORR_DAYS[i], vol,
                i < CORR_WINDOWS - 1 ? "," : "");
    }
    fprintf(f, "  },\n");

    /* SPY volatility */
    fprintf(f, "  \"spy_volatility\": {\n");
    for (int i = 0; i < CORR_WINDOWS; i++) {
        double vol = calc_volatility(spy, CORR_DAYS[i]);
        fprintf(f, "    \"%dd\": %.4f%s\n", CORR_DAYS[i], vol,
                i < CORR_WINDOWS - 1 ? "," : "");
    }
    fprintf(f, "  },\n");

    /* BTC-SPY rolling (latest values) */
    fprintf(f, "  \"btc_latest\": %.2f,\n", btc->n > 0 ? btc->pts[btc->n - 1].val : 0);
    fprintf(f, "  \"spy_latest\": %.2f\n", spy->n > 0 ? spy->pts[spy->n - 1].val : 0);

    fprintf(f, "}\n");
    fclose(f);

    /* Write heartbeat */
    FILE *hf = fopen(HB_PATH, "w");
    if (hf) { fprintf(hf, "%ld", time(NULL)); fclose(hf); }

    printf("[cross_asset] Done. Results written to %s\n", CACHE_PATH);
    printf("[cross_asset] BTC=%.2f SPY=%.2f\n",
           btc->n > 0 ? btc->pts[btc->n - 1].val : 0,
           spy->n > 0 ? spy->pts[spy->n - 1].val : 0);

    /* Cleanup */
    if (btc) { free(btc->pts); free(btc); }
    if (spy) { free(spy->pts); free(spy); }
    if (vix) { free(vix->pts); free(vix); }
    return 0;
}
