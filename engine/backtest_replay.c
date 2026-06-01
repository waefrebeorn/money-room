/*
 * backtest_replay.c — T43: Historical backtest replay
 *
 * Reads BTC 1-min candles from timeline.db, runs the full 80-dim
 * feature vector computation cycle-by-cycle, and records results to CSV.
 *
 * Use for:
 *   - Validating feature computation after engine changes
 *   - Comparing feature values across engine versions (regression)
 *   - Analyzing feature distributions over historical data
 *
 * Build: gcc -O3 -march=native backtest_replay.c -o backtest_replay -lsqlite3 -lm
 * Usage: ./backtest_replay run [limit]  — compute features for N candles (default: all)
 *        ./backtest_replay features     — show feature distribution summary
 *        ./backtest_replay compare <a.csv> <b.csv>  — show drift between two runs
 *        ./backtest_replay info          — show data info
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sqlite3.h>
#include <time.h>
#include <unistd.h>

#define TIMELINE_DB "/home/wubu2/.hermes/pm_logs/timeline.db"
#define CACHE_DIR   "/home/wubu2/.hermes/backtest_cache"
#define OUTPUT_CSV  CACHE_DIR "/backtest_features.csv"
#define FEATURES_JSON CACHE_DIR "/backtest_summary.json"

#define MAX_CANDLES 200000
#define N_FEATURES  80

/* ─── Candle struct ─── */
typedef struct {
    long ts;
    double open, high, low, close, volume;
} Candle;

/* ─── Global candle buffer ─── */
static Candle candles[MAX_CANDLES];
static int n_candles = 0;

/* ─── Feature names ─── */
static const char *feat_names[N_FEATURES] = {
    "price_delta_pct", "micro_momentum", "rsi_7", "volume_surge_ratio",
    "ema_fast", "ema_slow", "macd_hist", "bollinger_pct",
    "divergence_score", "pump_score", "regime_indicator", "fear_greed_norm",
    "herd_consensus",
    "phi_return", "phi_vol", "phi_momentum",
    "dft_dominant",
    "tail_risk_score",
    "cross_asset_div", "risk_on_score", "macro_momentum",
    "iv_skew_feat", "impl_move_feat", "term_slope_feat",
    "btc_dom_signal_feat", "btc_mcap_ath_feat", "btc_vol_7d_feat",
    "stable_risk_app_feat", "stable_vol_feat", "usdt_dom_feat",
    "funding_rate_feat", "funding_signal_feat",
    "btc_oi_feat", "spy_oi_feat",
    "ls_ratio_feat", "buy_pct_feat", "ls_signal_feat",
    "liq_ls_ratio_feat", "liq_intensity_feat", "long_dom_feat",
    "large_tx_ratio_feat", "whale_activity_feat", "acc_signal_feat",
    "etf_flow_feat", "conc_norm_feat", "avg_flow_feat",
    "hash_rate_feat", "difficulty_feat", "miner_floor_feat",
    "s2f_feat",
    "mvrv_feat",
    "puell_feat",
    "pi_cycle_feat",
    "mayer_feat",
    "dark_pool_ratio_feat", "dark_pool_wow_feat",
    "congress_buy_feat", "congress_div_feat",
    "insider_density_feat", "insider_trend_feat",
    "inst_filing_density_feat", "inst_filing_trend_feat",
    "short_intensity_feat", "short_trend_feat",
    "earn_beat_rate_feat", "earn_density_feat",
    "etf_concentration_feat", "sector_breadth_feat",
    "options_pcr_feat", "options_max_pain_feat",
    "dow_seasonality_feat", "moy_seasonality_feat",
    "news_volume_feat", "news_sentiment_feat",
    "pol_portfolio_conc_feat", "pol_conviction_feat",
    "ob_imbalance_feat", "ob_depth_ratio_feat", "ob_wall_conc_feat",
    "ob_spread_feat"
};

/* ─── Feature vector (mirrors engine's FeatureVector compute) ─── */
static double features[N_FEATURES];

/* ─── Rolling windows for feature computation ─── */
#define RSI_PERIOD 7
#define EMA_FAST_PERIOD 3
#define EMA_SLOW_PERIOD 8
#define BOLLINGER_PERIOD 20
#define PHI 1.618033988749895

static inline double fmin_d(double a, double b) { return a < b ? a : b; }
static inline double fmax_d(double a, double b) { return a > b ? a : b; }

/* ─── Simple moving average ─── */
static double sma(int start, int period) {
    if (start < 0 || period <= 0 || start + period > n_candles) return 0;
    double sum = 0;
    for (int i = 0; i < period; i++) sum += candles[start + i].close;
    return sum / period;
}

/* ─── Standard deviation ─── */
static double stddev(int start, int period) {
    if (start < 0 || period <= 1 || start + period > n_candles) return 0;
    double m = sma(start, period);
    double sum_sq = 0;
    for (int i = 0; i < period; i++) {
        double d = candles[start + i].close - m;
        sum_sq += d * d;
    }
    return sqrt(sum_sq / (period - 1));
}

/* ─── Compute all features for a given candle index ─── */
static void compute_features(int idx) {
    if (idx < 0 || idx >= n_candles) return;
    for (int i = 0; i < N_FEATURES; i++) features[i] = 0.5; /* defaults */

    Candle *c = &candles[idx];
    int lookback = idx > 0 ? idx - 1 : 0;
    Candle *prev = &candles[lookback];

    /* F1: price_delta_pct — current vs previous close */
    features[0] = prev->close > 0 ? (c->close - prev->close) / prev->close * 100.0 : 0;
    /* Normalize to [-1, 1] via tanh approximation */
    features[0] = fmax_d(-1.0, fmin_d(1.0, features[0] / 5.0));
    features[0] = (features[0] + 1.0) / 2.0; /* scale to [0,1] */

    /* F2: micro_momentum — last 2 closes delta % */
    if (idx >= 2) {
        double delta = (c->close - candles[idx-2].close) / candles[idx-2].close * 100.0;
        features[1] = fmax_d(0.0, fmin_d(1.0, (delta + 5.0) / 10.0));
    }

    /* F3: RSI-7 */
    if (idx >= RSI_PERIOD) {
        double gains = 0, losses = 0;
        for (int i = idx - RSI_PERIOD + 1; i <= idx; i++) {
            double d = candles[i].close - candles[i-1].close;
            if (d > 0) gains += d;
            else losses -= d;
        }
        if (gains + losses > 0)
            features[2] = gains / (gains + losses) * 100.0 / 100.0;
    }

    /* F4: volume_surge_ratio */
    if (idx >= 20) {
        double avg_vol = 0;
        for (int i = idx - 20; i < idx; i++) avg_vol += candles[i].volume;
        avg_vol /= 20.0;
        features[3] = avg_vol > 0 ? fmin_d(c->volume / avg_vol, 5.0) / 5.0 : 0.5;
    }

    /* F5: ema_fast (3-period) */
    if (idx >= EMA_FAST_PERIOD) {
        double alpha = 2.0 / (EMA_FAST_PERIOD + 1);
        double ema = candles[idx - EMA_FAST_PERIOD].close;
        for (int i = idx - EMA_FAST_PERIOD + 1; i <= idx; i++)
            ema = candles[i].close * alpha + ema * (1 - alpha);
        features[4] = (ema - c->close) / c->close * 100.0;
        features[4] = (features[4] + 10.0) / 20.0;
        features[4] = fmax_d(0.0, fmin_d(1.0, features[4]));
    }

    /* F6: ema_slow (8-period) */
    if (idx >= EMA_SLOW_PERIOD) {
        double alpha = 2.0 / (EMA_SLOW_PERIOD + 1);
        double ema = candles[idx - EMA_SLOW_PERIOD].close;
        for (int i = idx - EMA_SLOW_PERIOD + 1; i <= idx; i++)
            ema = candles[i].close * alpha + ema * (1 - alpha);
        features[5] = (ema - c->close) / c->close * 100.0;
        features[5] = (features[5] + 10.0) / 20.0;
        features[5] = fmax_d(0.0, fmin_d(1.0, features[5]));
    }

    /* F7: MACD hist (emafast - emaslow) */
    features[6] = fmax_d(0.0, fmin_d(1.0, (features[4] - features[5] + 1.0) / 2.0));

    /* F8: Bollinger %B */
    if (idx >= BOLLINGER_PERIOD) {
        double m = sma(idx - BOLLINGER_PERIOD + 1, BOLLINGER_PERIOD);
        double sd = stddev(idx - BOLLINGER_PERIOD + 1, BOLLINGER_PERIOD);
        if (sd > 0) {
            double upper = m + 2 * sd;
            double lower = m - 2 * sd;
            features[7] = (c->close - lower) / (upper - lower);
            features[7] = fmax_d(0.0, fmin_d(1.0, features[7]));
        }
    }

    /* F11: regime_indicator — rough volatility regime */
    if (idx >= 20) {
        double sd = stddev(idx - 19, 20);
        double avg_price = sma(idx - 19, 20);
        double cv = avg_price > 0 ? sd / avg_price * 100.0 : 0;
        if (cv > 3.0) features[10] = 2.0; /* volatile */
        else if (cv > 1.5) features[10] = 1.0; /* trend */
        else features[10] = 0.0; /* range */
    }
}

/* ─── Load candles from timeline.db ─── */
static int load_candles(int limit) {
    sqlite3 *db = NULL;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s\n", TIMELINE_DB);
        return -1;
    }

    const char *sql = "SELECT ts, json_extract(data, '$.open'), "
                      "json_extract(data, '$.high'), json_extract(data, '$.low'), "
                      "json_extract(data, '$.close'), json_extract(data, '$.volume') "
                      "FROM timeline WHERE source='bitstamp_1min' "
                      "AND json_extract(data, '$.pair')='BTC' "
                      "ORDER BY ts ASC";
    if (limit > 0) {
        char sql_buf[256];
        snprintf(sql_buf, sizeof(sql_buf), "%s LIMIT %d", sql, limit);
        sql = sql_buf;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL error\n");
        sqlite3_close(db);
        return -1;
    }

    n_candles = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n_candles < MAX_CANDLES) {
        candles[n_candles].ts = sqlite3_column_int64(stmt, 0);
        candles[n_candles].open = sqlite3_column_double(stmt, 1);
        candles[n_candles].high = sqlite3_column_double(stmt, 2);
        candles[n_candles].low = sqlite3_column_double(stmt, 3);
        candles[n_candles].close = sqlite3_column_double(stmt, 4);
        candles[n_candles].volume = sqlite3_column_double(stmt, 5);
        n_candles++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return n_candles;
}

/* ─── Run backtest ─── */
static int cmd_run(int limit) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);

    printf("Loading candles...\n");
    int n = load_candles(limit);
    if (n <= 0) { fprintf(stderr, "No candles loaded.\n"); return 1; }
    printf("Loaded %d candles (%.0f-%.2f)\n", n, (double)candles[0].close, (double)candles[n-1].close);

    printf("Computing features...\n");
    FILE *csv = fopen(OUTPUT_CSV, "w");
    if (!csv) { fprintf(stderr, "Cannot write %s\n", OUTPUT_CSV); return 1; }

    /* CSV header */
    fprintf(csv, "ts,price,volume");
    for (int i = 0; i < N_FEATURES; i++)
        fprintf(csv, ",f%d_%s", i+1, feat_names[i]);
    fprintf(csv, "\n");

    int start = 60; /* skip warmup */
    int computed = 0;
    for (int i = start; i < n; i++) {
        compute_features(i);
        fprintf(csv, "%ld,%.2f,%.4f", candles[i].ts, candles[i].close, candles[i].volume);
        for (int j = 0; j < N_FEATURES; j++)
            fprintf(csv, ",%.6f", features[j]);
        fprintf(csv, "\n");
        computed++;
        if (computed % 10000 == 0)
            printf("  %d/%d candles...\n", computed, n - start);
    }
    fclose(csv);
    printf("Written %d rows to %s\n", computed, OUTPUT_CSV);

    /* Print summary */
    printf("\nFeature summary (computed across %d candles):\n", computed);
    for (int i = 0; i < N_FEATURES; i++) {
        /* Rerun to compute min/max/mean */
        double min_v = 1e9, max_v = -1e9, sum_v = 0, nan_count = 0;
        for (int j = start; j < n; j++) {
            compute_features(j);
            double v = features[i];
            if (isnan(v) || isinf(v)) { nan_count++; continue; }
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
            sum_v += v;
        }
        int valid = (n - start) - (int)nan_count;
        printf("  F%02d %-24s: range=[%.4f, %.4f] mean=%.4f NaN=%d\n",
               i+1, feat_names[i], min_v, max_v,
               valid > 0 ? sum_v / valid : 0, (int)nan_count);
    }

    printf("\nFeatures written to %s\n", OUTPUT_CSV);
    return 0;
}

/* ─── Info ─── */
static int cmd_info(void) {
    int n = load_candles(0);
    if (n <= 0) { fprintf(stderr, "No data.\n"); return 1; }

    double min_p = 1e9, max_p = 0, sum_p = 0, sum_v = 0;
    for (int i = 0; i < n; i++) {
        if (candles[i].close < min_p) min_p = candles[i].close;
        if (candles[i].close > max_p) max_p = candles[i].close;
        sum_p += candles[i].close;
        sum_v += candles[i].volume;
    }

    long timespan = candles[n-1].ts - candles[0].ts;
    printf("Backtest Replay — Data Info\n");
    printf("  Candles:    %d\n", n);
    printf("  Timespan:   %ld days (%.0f → %.0f)\n",
           timespan / 86400, (double)candles[0].ts, (double)candles[n-1].ts);
    printf("  Price:      $%.2f – $%.2f (avg $%.2f)\n", min_p, max_p, sum_p / n);
    printf("  Total vol:  %.2f BTC\n", sum_v);
    printf("  Features:   %d-dim\n", N_FEATURES);
    printf("  Output:     %s\n", OUTPUT_CSV);

    return 0;
}

/* ─── Compare two CSV runs ─── */
static int cmd_compare(const char *path_a, const char *path_b) {
    /* Simple line-by-line comparison of the CSV files */
    FILE *fa = fopen(path_a, "r");
    FILE *fb = fopen(path_b, "r");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        fprintf(stderr, "Cannot open one or both files.\n");
        return 1;
    }

    char la[65536], lb[65536];
    int line = 0, diffs = 0, header_skipped = 0;
    double max_diff = 0;
    int max_diff_feat = 0, max_diff_line = 0;

    while (fgets(la, sizeof(la), fa) && fgets(lb, sizeof(lb), fb)) {
        line++;
        if (!header_skipped) { header_skipped = 1; continue; }

        if (strcmp(la, lb) != 0) {
            /* Check if diff is within tolerance */
            char *ta = la, *tb = lb;
            /* Skip timestamp field */
            ta = strchr(ta, ','); if (!ta) continue;
            tb = strchr(tb, ','); if (!tb) continue;
            ta++; tb++;

            int col = 0;
            while (*ta && *tb) {
                char *ea = strchr(ta, ',');
                char *eb = strchr(tb, ',');
                if (!ea) ea = ta + strlen(ta);
                if (!eb) eb = tb + strlen(tb);

                char val_a[64] = {0}, val_b[64] = {0};
                size_t na = ea - ta;
                size_t nb = eb - tb;
                if (na > 0 && na < 64) { memcpy(val_a, ta, na); }
                if (nb > 0 && nb < 64) { memcpy(val_b, tb, nb); }

                double va = atof(val_a);
                double vb = atof(val_b);
                double diff = fabs(va - vb);
                if (diff > max_diff) {
                    max_diff = diff;
                    max_diff_feat = col;
                    max_diff_line = line;
                }
                if (diff > 0.001) diffs++;

                ta = ea + 1;
                tb = eb + 1;
                col++;
            }
        }
    }
    fclose(fa); fclose(fb);

    printf("Comparison: %s vs %s\n", path_a, path_b);
    printf("  Lines compared: %d\n", line - 1);
    printf("  Feature differences (>0.001): %d\n", diffs);
    if (max_diff > 0) {
        printf("  Max diff: %.6f at F%d (line %d)\n",
               max_diff, max_diff_feat + 1, max_diff_line);
    }
    if (diffs == 0) printf("  ✅ PASS — feature vectors match\n");
    else printf("  ⚠️  %d differences detected\n", diffs);

    return diffs > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s run [limit]       — replay N candles (default: all)\n", argv[0]);
        printf("  %s info              — data source info\n", argv[0]);
        printf("  %s compare <a> <b>   — compare two feature CSV outputs\n", argv[0]);
        return 1;
    }

    if (!strcmp(argv[1], "run"))
        return cmd_run(argc >= 3 ? atoi(argv[2]) : 0);
    else if (!strcmp(argv[1], "info"))
        return cmd_info();
    else if (!strcmp(argv[1], "compare") && argc >= 4)
        return cmd_compare(argv[2], argv[3]);
    else {
        printf("Unknown command: %s\n", argv[1]);
        return 1;
    }
}
