/**
 * timeline_analyzer.c — Market Timeline Thermometer
 * 
 * Reads 8.4M-row timeline.db, computes trend analysis in pure C.
 * No Python. No dependencies beyond libsqlite3 and libm.
 * 
 * NOT FINANCIAL ADVICE. This is a thermometer of market analysis
 * performed by algorithms. Subscribing to this data is subscribing
 * to an algorithmic analysis — not a financial recommendation.
 *
 * Build: gcc -O3 -std=c11 timeline_analyzer.c -lsqlite3 -lm -o timeline_analyzer
 * Usage: ./timeline_analyzer [source] [days]
 *   source: bitstamp_1min, kraken_btc, vix, sp500, gold, or "all"
 *   days: how many days back (default 30)
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sqlite3.h>
#include <time.h>

#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define MAX_ROWS 100000
#define MAX_SOURCES 100

/* ── Data point ── */
typedef struct {
    long long ts;
    double values[10];
    int n_values;
} DataPoint;

/* ── Analysis result ── */
typedef struct {
    const char *name;
    long long n_points;
    double mean;
    double stddev;
    double min;
    double max;
    double current;
    double trend;        /* +1 = up, -1 = down, 0 = flat */
    double momentum;     /* rate of change */
    double volatility;   /* stddev of returns */
    double zscore;       /* how far from mean */
    double pct_rank;     /* percentile of current value */
    double sma_short;    /* short moving average */
    double sma_long;     /* long moving average */
    double crossover;    /* +1 = bullish cross, -1 = bearish, 0 = no */
    double rsi;          /* relative strength index (0-100) */
} AnalysisResult;

/* ── SQLite callback ── */
static int count_callback(void *data, int argc, char **argv, char **col) {
    long long *count = (long long *)data;
    if (argc > 0 && argv[0]) *count = atoll(argv[0]);
    return 0;
}

/* ── Data collector callback ── */
typedef struct {
    DataPoint *points;
    int count;
    int capacity;
} PointCollector;

/* ── Extract price from JSON data column ── */
static double extract_price(const char *json_str) {
    if (!json_str) return 0;
    const char *targets[] = {"\"close\":", "\"value\":"};
    for (int t = 0; t < 2; t++) {
        const char *p = strstr(json_str, targets[t]);
        if (p) {
            p += strlen(targets[t]);
            while (*p && (*p == ' ' || *p == '\t')) p++;
            if (*p >= '0' && *p <= '9' || *p == '-') return atof(p);
        }
    }
    return 0;
}

static int point_callback(void *data, int argc, char **argv, char **col) {
    PointCollector *pc = (PointCollector *)data;
    if (pc->count >= pc->capacity) return 0;
    
    DataPoint *dp = &pc->points[pc->count];
    memset(dp, 0, sizeof(DataPoint));
    
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(col[i], "ts") == 0) {
            dp->ts = atoll(argv[i]);
        } else if (strcmp(col[i], "data") == 0) {
            dp->values[0] = extract_price(argv[i]);
            dp->n_values = 1;
        }
    }
    pc->count++;
    return 0;
}

/* ── Statistics ── */
static double calc_mean(double *vals, int n) {
    if (n <= 0) return 0;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += vals[i];
    return sum / n;
}

static double calc_stddev(double *vals, int n, double mean) {
    if (n <= 1) return 0;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (vals[i] - mean) * (vals[i] - mean);
    return sqrt(sum / (n - 1));
}

static int compare_double(const void *a, const void *b) {
    double da = *(double *)a, db = *(double *)b;
    return (da > db) - (da < db);
}

static double calc_percentile(double *sorted, int n, double val) {
    if (n <= 0) return 50;
    int below = 0;
    for (int i = 0; i < n; i++) if (sorted[i] <= val) below++;
    return (double)below / n * 100;
}

static double calc_rsi(double *vals, int n, int period) {
    if (n < period + 1) return 50;
    double gains = 0, losses = 0;
    for (int i = n - period; i < n - 1; i++) {
        double diff = vals[i+1] - vals[i];
        if (diff > 0) gains += diff;
        else losses -= diff;
    }
    if (losses == 0) return 100;
    double rs = gains / losses / period;
    return 100 - 100 / (1 + rs);
}

/* ── Analyze a source ── */
static AnalysisResult analyze_source(sqlite3 *db, const char *source, int days) {
    AnalysisResult r = {0};
    r.name = source;
    
    /* Count rows first */
    long long n_rows = 0;
    char sql[512];
    if (days > 0) {
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM timeline WHERE source='%s' AND ts > %lld",
            source, (long long)(time(NULL) - (long long)days * 86400));
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM timeline WHERE source='%s'", source);
    }
    
    sqlite3_exec(db, sql, count_callback, &n_rows, NULL);
    if (n_rows == 0) {
        r.n_points = 0;
        return r;
    }
    
    /* Fetch data */
    int cap = (int)fmin(n_rows, MAX_ROWS);
    DataPoint *points = malloc(cap * sizeof(DataPoint));
    PointCollector pc = {points, 0, cap};
    
    if (days > 0) {
        snprintf(sql, sizeof(sql),
            "SELECT ts, data FROM timeline WHERE source='%s' AND ts > %lld ORDER BY ts LIMIT %d",
            source, (long long)(time(NULL) - (long long)days * 86400), cap);
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT ts, data FROM timeline WHERE source='%s' ORDER BY ts DESC LIMIT %d",
            source, cap);
    }
    
    sqlite3_exec(db, sql, point_callback, &pc, NULL);
    r.n_points = pc.count;
    
    if (r.n_points < 2) {
        free(points);
        return r;
    }
    
    /* Extract values for analysis (use first data field: price/rate) */
    double *vals = malloc(r.n_points * sizeof(double));
    for (int i = 0; i < r.n_points; i++) {
        vals[i] = points[i].n_values > 0 ? points[i].values[0] : 0;
    }
    
    /* Basic stats */
    r.current = vals[r.n_points - 1];
    r.mean = calc_mean(vals, r.n_points);
    r.stddev = calc_stddev(vals, r.n_points, r.mean);
    r.min = vals[0];
    r.max = vals[0];
    for (int i = 1; i < r.n_points; i++) {
        if (vals[i] < r.min) r.min = vals[i];
        if (vals[i] > r.max) r.max = vals[i];
    }
    
    /* Z-score */
    r.zscore = r.stddev > 0 ? (r.current - r.mean) / r.stddev : 0;
    
    /* Percentile rank */
    double *sorted = malloc(r.n_points * sizeof(double));
    memcpy(sorted, vals, r.n_points * sizeof(double));
    qsort(sorted, r.n_points, sizeof(double), compare_double);
    r.pct_rank = calc_percentile(sorted, r.n_points, r.current);
    free(sorted);
    
    /* Moving averages */
    int short_period = (int)fmin(20, r.n_points);
    int long_period = (int)fmin(50, r.n_points);
    double sum_short = 0, sum_long = 0;
    for (int i = 0; i < short_period; i++) sum_short += vals[r.n_points - 1 - i];
    for (int i = 0; i < long_period; i++) sum_long += vals[r.n_points - 1 - i];
    r.sma_short = sum_short / short_period;
    r.sma_long = sum_long / long_period;
    
    /* Crossover */
    double prev_short = 0, prev_long = 0;
    for (int i = 0; i < short_period && i < r.n_points - 1; i++) prev_short += vals[r.n_points - 2 - i];
    for (int i = 0; i < long_period && i < r.n_points - 1; i++) prev_long += vals[r.n_points - 2 - i];
    prev_short /= short_period;
    prev_long /= long_period;
    
    int now_bullish = r.sma_short > r.sma_long ? 1 : 0;
    int prev_bullish = prev_short > prev_long ? 1 : 0;
    if (now_bullish && !prev_bullish) r.crossover = 1.0;     /* Golden cross */
    else if (!now_bullish && prev_bullish) r.crossover = -1.0; /* Death cross */
    else r.crossover = 0;
    
    /* Trend: compare first vs last third */
    int third = r.n_points / 3;
    if (third > 0) {
        double first_avg = calc_mean(vals, third);
        double last_avg = calc_mean(&vals[r.n_points - third], third);
        double change = (last_avg - first_avg) / fmax(first_avg, 0.001);
        r.trend = (change > 0.02) ? 1 : ((change < -0.02) ? -1 : 0);
        r.momentum = change;
    }
    
    /* Volatility (stddev of daily returns) */
    if (r.n_points > 1) {
        double *returns = malloc((r.n_points - 1) * sizeof(double));
        for (int i = 1; i < r.n_points; i++) {
            double prev = vals[i-1];
            returns[i-1] = prev > 0 ? (vals[i] - prev) / prev : 0;
        }
        r.volatility = calc_stddev(returns, r.n_points - 1, calc_mean(returns, r.n_points - 1));
        free(returns);
    }
    
    /* RSI (14-period) */
    int rsi_period = (int)fmin(14, r.n_points / 2);
    r.rsi = calc_rsi(vals, r.n_points, rsi_period);
    
    free(vals);
    free(points);
    return r;
}

/* ── List available sources ── */
static void list_sources(sqlite3 *db) {
    printf("═══════════════════════════════════════════════════════\n");
    printf("  TIMELINE DATA SOURCES (%d+ sources available)\n", MAX_SOURCES);
    printf("═══════════════════════════════════════════════════════\n\n");
    
    const char *sql = "SELECT source, COUNT(*) as cnt, "
        "MIN(ts) as first, MAX(ts) as last "
        "FROM timeline GROUP BY source ORDER BY cnt DESC LIMIT 20";
    
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("  Error: %s\n", sqlite3_errmsg(db));
        return;
    }
    
    printf("  %-30s %12s %12s %12s\n", "Source", "Rows", "First", "Latest");
    printf("  %s\n", "─────────────────────────────────────────────────────────────────────");
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        long long cnt = sqlite3_column_int64(stmt, 1);
        long long first = sqlite3_column_int64(stmt, 2);
        long long last = sqlite3_column_int64(stmt, 3);
        
        char first_str[32] = "?", last_str[32] = "?";
        if (first > 0) {
            time_t ft = (time_t)first;
            struct tm *tm = gmtime(&ft);
            strftime(first_str, sizeof(first_str), "%Y-%m-%d", tm);
        }
        if (last > 0) {
            time_t lt = (time_t)last;
            struct tm *tm = gmtime(&lt);
            strftime(last_str, sizeof(last_str), "%Y-%m-%d", tm);
        }
        
        printf("  %-30s %12lld %12s %12s\n", name, cnt, first_str, last_str);
    }
    sqlite3_finalize(stmt);
    printf("\n");
}

/* ── Print analysis ── */
static void print_analysis(const AnalysisResult *r) {
    printf("  %s\n", r->name);
    printf("  %s\n", "──────────────────────────────");
    printf("  Points:    %lld\n", r->n_points);
    printf("  Current:   %.4f\n", r->current);
    printf("  Mean:      %.4f\n", r->mean);
    printf("  StdDev:    %.4f\n", r->stddev);
    printf("  Min/Max:   %.4f / %.4f\n", r->min, r->max);
    printf("  Z-Score:   %+.2f (%s)\n", r->zscore,
        fabs(r->zscore) < 1 ? "normal" :
        fabs(r->zscore) < 2 ? "unusual" : "extreme");
    printf("  Percentile: %.1f%%\n", r->pct_rank);
    printf("  Trend:     %s (momentum: %+.4f)\n",
        r->trend > 0 ? "🟢 UP" : (r->trend < 0 ? "🔴 DOWN" : "⚪ FLAT"),
        r->momentum);
    printf("  Volatility: %.4f (%s)\n", r->volatility,
        r->volatility < 0.01 ? "low" :
        r->volatility < 0.05 ? "moderate" :
        r->volatility < 0.10 ? "high" : "extreme");
    printf("  SMA(20):   %.4f\n", r->sma_short);
    printf("  SMA(50):   %.4f\n", r->sma_long);
    printf("  Crossover: %s\n",
        r->crossover > 0 ? "🟢 GOLDEN CROSS (bullish)" :
        r->crossover < 0 ? "🔴 DEATH CROSS (bearish)" : "⚪ no cross");
    printf("  RSI(14):   %.1f (%s)\n", r->rsi,
        r->rsi > 70 ? "overbought" :
        r->rsi < 30 ? "oversold" : "neutral");
    printf("\n");
}

/* ── Main ── */
int main(int argc, char *argv[]) {
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════╗\n");
    printf("  ║   TIMELINE THERMOMETER — Market Analysis in C       ║\n");
    printf("  ║   8.4M+ data points · 14 years · 100+ sources       ║\n");
    printf("  ╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  NOT FINANCIAL ADVICE. This is a static algorithmic\n");
    printf("  analysis — a thermometer of market data processed by\n");
    printf("  by autonomous systems. No deterministic predictions.\n");
    printf("  Subscribe to the algorithm, not a financial suggestion.\n");
    printf("\n");
    
    /* Open DB */
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "  ERROR: Can't open timeline.db at %s\n", DB_PATH);
        fprintf(stderr, "  %s\n", sqlite3_errmsg(db));
        return 1;
    }
    
    int days = 30;  /* default: 30 days */
    
    if (argc < 2) {
        /* No args — list sources */
        list_sources(db);
        printf("  Usage: %s <source> [days]\n", argv[0]);
        printf("  Example: %s bitstamp_1min 90\n", argv[0]);
        printf("  Example: %s all 7\n", argv[0]);
        printf("\n");
        sqlite3_close(db);
        return 0;
    }
    
    if (argc >= 3) days = atoi(argv[2]);
    if (days <= 0) days = 30;
    
    if (strcmp(argv[1], "all") == 0) {
        /* Analyze top sources */
        const char *top_sources[] = {
            "bitstamp_1min", "kraken_btc", "kraken_eth", "kraken_sol",
            "fred_sp500", "fred_vix", "stock_gold", "stock_crude_oil",
            "forex_eurusd", "fear_greed_fear_greed_all", NULL
        };
        
        for (int i = 0; top_sources[i]; i++) {
            AnalysisResult r = analyze_source(db, top_sources[i], days);
            if (r.n_points > 0) print_analysis(&r);
        }
    } else {
        AnalysisResult r = analyze_source(db, argv[1], days);
        if (r.n_points > 0) {
            print_analysis(&r);
        } else {
            printf("  No data for source '%s' in last %d days.\n", argv[1], days);
            printf("  Run with no args to list available sources.\n");
        }
    }
    
    sqlite3_close(db);
    return 0;
}
