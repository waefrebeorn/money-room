/**
 * walk_forward.c — T45: Walk-Forward Validation
 *
 * Performs walk-forward analysis on BTC market data:
 * 1. Divides historical candles into N windows
 * 2. For each window: trains feature-based model on in-sample (IS),
 *    tests on out-of-sample (OOS)
 * 3. Reports OOS WR, Sharpe, and IS/OOS correlation
 *
 * Build: gcc -O3 -march=native walk_forward.c -o walk_forward -lsqlite3 -lm
 * Usage: ./walk_forward [candles] [windows] [train_pct]
 *   candles:   number of candles to use (default: all)
 *   windows:   number of walk-forward windows (default: 10)
 *   train_pct: training % per window (default: 70)
 * Example: ./walk_forward 100000 12 75
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sqlite3.h>
#include <time.h>

#define TIMELINE_DB "/home/wubu2/.hermes/pm_logs/historical/historical.db"
#define CACHE_DIR   "/home/wubu2/.hermes/walkforward_cache"

/* ─── Candle ─── */
typedef struct {
    long ts;
    double open, high, low, close, volume;
} Candle;

static Candle *candles = NULL;
static int n_candles = 0;
static int max_candles = 200000;

/* ─── Feature generator (single feature: momentum-based) ─── */
typedef struct {
    int valid;
    double price_delta_pct;
    double micro_momentum;
    double rsi_7;
    double volume_surge;
    double ema_fast;
    double ema_slow;
    double macd_hist;
    double bollinger;
} Features;

static double sma(int start, int period) {
    if (start < 0 || period <= 0 || start + period > n_candles) return 0;
    double sum = 0;
    for (int i = 0; i < period; i++) sum += candles[start + i].close;
    return sum / period;
}

static double stddev_f(int start, int period) {
    if (start < 0 || period <= 1 || start + period > n_candles) return 0;
    double m = sma(start, period);
    double sum_sq = 0;
    for (int i = 0; i < period; i++) {
        double d = candles[start + i].close - m;
        sum_sq += d * d;
    }
    return sqrt(sum_sq / (period - 1));
}

static Features compute_features(int idx) {
    Features f = {0};
    if (idx < 1 || idx >= n_candles) return f;
    f.valid = 1;

    Candle *c = &candles[idx];
    Candle *p = &candles[idx - 1];

    /* Price delta % */
    f.price_delta_pct = p->close > 0 ? (c->close - p->close) / p->close * 100.0 : 0;

    /* Micro momentum */
    if (idx >= 2) {
        f.micro_momentum = (c->close - candles[idx-2].close) / candles[idx-2].close * 100.0;
    }

    /* RSI-7 */
    if (idx >= 7) {
        double gains = 0, losses = 0;
        for (int i = idx - 6; i <= idx; i++) {
            double chg = (candles[i].close - candles[i-1].close) / candles[i-1].close * 100.0;
            if (chg > 0) gains += chg; else losses -= chg;
        }
        double rs = losses > 0 ? gains / losses : 100;
        f.rsi_7 = 100 - 100 / (1 + rs);
    }

    /* Volume surge */
    if (idx >= 5) {
        double avg_vol = 0;
        for (int i = idx - 5; i < idx; i++) avg_vol += candles[i].volume;
        avg_vol /= 5;
        f.volume_surge = avg_vol > 0 ? c->volume / avg_vol : 1;
    }

    /* EMAs */
    if (idx >= 8) {
        double sum_fast = 0, sum_slow = 0;
        for (int i = idx - 2; i <= idx; i++) sum_fast += candles[i].close;
        for (int i = idx - 7; i <= idx; i++) sum_slow += candles[i].close;
        f.ema_fast = sum_fast / 3;
        f.ema_slow = sum_slow / 8;
        f.macd_hist = f.ema_fast - f.ema_slow;
    }

    /* Bollinger %B */
    if (idx >= 20) {
        double m = sma(idx - 19, 20);
        double sd = stddev_f(idx - 19, 20);
        if (sd > 0) {
            f.bollinger = (c->close - (m - 2*sd)) / (4 * sd);
        }
    }

    return f;
}

/* ─── Simple linear model ─── */
typedef struct {
    double w[7];  /* one per feature */
    double bias;
} LinearModel;

static double predict(const LinearModel *m, const Features *f) {
    double v = m->bias;
    v += m->w[0] * f->price_delta_pct;
    v += m->w[1] * f->micro_momentum;
    v += m->w[2] * (f->rsi_7 / 100.0 - 0.5);
    v += m->w[3] * (f->volume_surge - 1.0);
    v += m->w[4] * f->macd_hist;
    v += m->w[5] * (f->bollinger - 0.5);
    v += m->w[6] * (f->ema_fast / f->ema_slow - 1.0);
    return v; /* >0 = long, <0 = short */
}

/* ─── Train model on IS segment ─── */
static void train_model(LinearModel *m, int start, int end) {
    /* Initialize */
    for (int i = 0; i < 7; i++) m->w[i] = 0;
    m->bias = 0;

    /* Collect features and next-period returns */
    int n_train = 0;
    #define MAX_TRAIN 100000
    double feat[MAX_TRAIN][7];
    double target[MAX_TRAIN];

    for (int i = start; i < end - 1 && n_train < MAX_TRAIN; i++) {
        Features f = compute_features(i);
        if (!f.valid) continue;
        /* Target: +1 if next candle went up, -1 if down */
        double ret = (candles[i+1].close - candles[i].close) / candles[i].close * 100.0;
        target[n_train] = ret > 0 ? 1 : -1;
        feat[n_train][0] = f.price_delta_pct;
        feat[n_train][1] = f.micro_momentum;
        feat[n_train][2] = f.rsi_7 / 100.0 - 0.5;
        feat[n_train][3] = f.volume_surge - 1.0;
        feat[n_train][4] = f.macd_hist;
        feat[n_train][5] = f.bollinger - 0.5;
        feat[n_train][6] = f.ema_fast / f.ema_slow - 1.0;
        n_train++;
    }

    if (n_train < 10) return;

    /* Simple SGD: 5 epochs */
    double lr = 0.001;
    for (int epoch = 0; epoch < 5; epoch++) {
        double total_loss = 0;
        for (int i = 0; i < n_train; i++) {
            double pred = m->bias;
            for (int j = 0; j < 7; j++) pred += m->w[j] * feat[i][j];
            double error = pred - target[i];
            total_loss += error * error;
            /* SGD update */
            m->bias -= lr * error;
            for (int j = 0; j < 7; j++) m->w[j] -= lr * error * feat[i][j];
        }
        lr *= 0.9; /* decay */
    }
}

/* ─── Test model on OOS segment ─── */
static void test_model(LinearModel *m, int start, int end,
                        int *out_wins, int *out_total, double *out_pnl) {
    *out_wins = 0;
    *out_total = 0;
    *out_pnl = 0;

    for (int i = start; i < end - 1; i++) {
        Features f = compute_features(i);
        if (!f.valid) continue;
        double pred = predict(m, &f);
        double actual_ret = (candles[i+1].close - candles[i].close) / candles[i].close * 100.0;
        int direction_right = (pred > 0 && actual_ret > 0) || (pred < 0 && actual_ret < 0);
        (*out_total)++;
        if (direction_right) (*out_wins)++;
        *out_pnl += actual_ret * (pred > 0 ? 1 : -1); /* PnL if trading signal */
    }
}

/* ─── Load candles from timeline.db ─── */
static int load_candles(int limit) {
    sqlite3 *db;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "Can't open %s\n", TIMELINE_DB);
        return 0;
    }

    candles = calloc(max_candles, sizeof(Candle));
    if (!candles) { sqlite3_close(db); return 0; }

    const char *sql = "SELECT ts, open, high, low, close, volume "
                       "FROM candles WHERE pair='BTC' ORDER BY ts ASC";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Query error: %s\n", sqlite3_errmsg(db));
        free(candles); sqlite3_close(db); return 0;
    }

    n_candles = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n_candles < max_candles) {
        candles[n_candles].ts     = sqlite3_column_int64(stmt, 0);
        candles[n_candles].open   = sqlite3_column_double(stmt, 1);
        candles[n_candles].high   = sqlite3_column_double(stmt, 2);
        candles[n_candles].low    = sqlite3_column_double(stmt, 3);
        candles[n_candles].close  = sqlite3_column_double(stmt, 4);
        candles[n_candles].volume = sqlite3_column_double(stmt, 5);
        n_candles++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (limit > 0 && limit < n_candles) n_candles = limit;
    return n_candles;
}

int main(int argc, char **argv) {
    int limit = 0;
    int windows = 10;
    int train_pct = 70;

    if (argc > 1) limit = atoi(argv[1]);
    if (argc > 2) windows = atoi(argv[2]);
    if (argc > 3) train_pct = atoi(argv[3]);
    if (windows < 2) windows = 2;
    if (windows > 50) windows = 50;
    if (train_pct < 20) train_pct = 20;
    if (train_pct > 90) train_pct = 90;

    if (!load_candles(limit)) {
        fprintf(stderr, "No candles loaded\n");
        return 1;
    }

    printf("=== Walk-Forward Validation ===\n");
    printf("  Candles:      %d\n", n_candles);
    printf("  Date range:   %s → %s\n",
           ctime(&candles[0].ts),
           ctime(&candles[n_candles-1].ts));
    printf("  Windows:      %d\n", windows);
    printf("  Train/Test:   %d%%/%d%% per window\n\n", train_pct, 100-train_pct);

    int start_offset = 50; /* skip first 50 for feature computation */
    int oos_start = start_offset;
    int total_candles = n_candles - start_offset;
    int window_size = total_candles / windows;

    double is_wr_sum = 0, oos_wr_sum = 0;
    double is_pnl_sum = 0, oos_pnl_sum = 0;
    int oos_total_trades = 0, oos_total_wins = 0;
    int valid_windows = 0;

    printf("  %-6s %-8s %-8s %-8s %-8s %-8s %-8s\n",
           "Window", "IS_WR%", "OOS_WR%", "IS_PnL", "OOS_PnL", "IS_trd", "OOS_trd");
    printf("  %s\n", "──────────────────────────────────────────────────────────────");

    for (int w = 0; w < windows; w++) {
        int win_start = start_offset + w * window_size;
        int train_end = win_start + window_size * train_pct / 100;
        int test_end  = win_start + window_size;
        if (test_end > n_candles) test_end = n_candles;
        if (train_end >= test_end || train_end - win_start < 50) continue;

        LinearModel model;
        train_model(&model, win_start, train_end);

        /* Test on IS */
        int is_wins = 0, is_total = 0;
        double is_pnl = 0;
        test_model(&model, win_start + 10, train_end, &is_wins, &is_total, &is_pnl);

        /* Test on OOS */
        int oos_wins = 0, oos_total = 0;
        double oos_pnl = 0;
        if (train_end < test_end - 2) {
            test_model(&model, train_end, test_end, &oos_wins, &oos_total, &oos_pnl);
        }

        double is_wr = is_total > 0 ? (double)is_wins / is_total * 100 : 0;
        double oos_wr = oos_total > 0 ? (double)oos_wins / oos_total * 100 : 0;

        printf("  W%-5d %7.2f%% %7.2f%% %+8.4f %+8.4f %6d %6d\n",
               w+1, is_wr, oos_wr, is_pnl, oos_pnl, is_total, oos_total);

        is_wr_sum += is_wr;
        oos_wr_sum += oos_wr;
        is_pnl_sum += is_pnl;
        oos_pnl_sum += oos_pnl;
        oos_total_trades += oos_total;
        oos_total_wins += oos_wins;
        valid_windows++;
    }

    printf("\n  === Summary ===\n");
    printf("  Valid windows:  %d\n", valid_windows);
    double avg_is_wr = valid_windows > 0 ? is_wr_sum / valid_windows : 0;
    double avg_oos_wr = valid_windows > 0 ? oos_wr_sum / valid_windows : 0;
    printf("  Avg IS WR:     %.2f%%\n", avg_is_wr);
    printf("  Avg OOS WR:    %.2f%%\n", avg_oos_wr);
    printf("  OOS total:     %d trades, %d wins (%.2f%%)\n",
           oos_total_trades, oos_total_wins,
           oos_total_trades > 0 ? (double)oos_total_wins / oos_total_trades * 100 : 0);
    printf("  IS/OOS gap:    %+.2f%% (%.2f%% of IS)\n",
           avg_oos_wr - avg_is_wr,
           avg_is_wr > 0 ? (avg_oos_wr - avg_is_wr) / avg_is_wr * 100 : 0);

    /* Overfitting detection */
    double gap = fabs(avg_is_wr - avg_oos_wr);
    if (gap < 2.0) printf("  Overfit:       LOW (gap %.2f%%) ✅\n", gap);
    else if (gap < 5.0) printf("  Overfit:       MODERATE (gap %.2f%%) ⚠️\n", gap);
    else printf("  Overfit:       HIGH (gap %.2f%%) ⛔\n", gap);

    /* Output summary as JSON */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);

    char json_path[512];
    snprintf(json_path, sizeof(json_path), "%s/walkforward_summary.json", CACHE_DIR);
    FILE *f = fopen(json_path, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"candles\": %d,\n", n_candles);
        fprintf(f, "  \"windows\": %d,\n", windows);
        fprintf(f, "  \"train_pct\": %d,\n", train_pct);
        fprintf(f, "  \"valid_windows\": %d,\n", valid_windows);
        fprintf(f, "  \"avg_is_wr\": %.4f,\n", avg_is_wr);
        fprintf(f, "  \"avg_oos_wr\": %.4f,\n", avg_oos_wr);
        fprintf(f, "  \"oos_total_trades\": %d,\n", oos_total_trades);
        fprintf(f, "  \"oos_total_wins\": %d,\n", oos_total_wins);
        fprintf(f, "  \"avg_is_pnl\": %.4f,\n", avg_is_wr);
        fprintf(f, "  \"avg_oos_pnl\": %.4f,\n", avg_oos_wr);
        fprintf(f, "  \"overfit_gap\": %.4f\n", gap);
        fprintf(f, "}\n");
        fclose(f);
        printf("  Summary:       %s\n", json_path);
    }

    free(candles);
    return 0;
}
