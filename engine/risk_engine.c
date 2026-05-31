/*
 * risk_engine.c — Risk analytics module for Money Room engine
 * VaR (Value at Risk), CVaR, max drawdown, Sharpe, Sortino
 *
 * Reads trade_log.csv and engine state JSON. Outputs risk metrics JSON.
 *
 * Compile: gcc -O3 -o risk_engine risk_engine.c -lm
 * Usage:   ./risk_engine [--csv trade_log.csv] [--state engine_state.json]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define MAX_TRADES 1000000

typedef struct {
    double ts;
    double pnl;
    double capital;
    double win;
} Trade;

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

/* Compute VaR from sorted returns array */
static double calc_var(double *returns, int n, double confidence) {
    if (n < 2) return 0.0;
    int idx = (int)((1.0 - confidence) * n);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return returns[idx];
}

static double calc_cvar(double *returns, int n, double confidence) {
    if (n < 2) return 0.0;
    int idx = (int)((1.0 - confidence) * n);
    if (idx < 0) idx = 0;
    if (idx >= n) return 0.0;
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i <= idx && i < n; i++) {
        sum += returns[i];
        count++;
    }
    return count > 0 ? sum / count : 0.0;
}

int main(int argc, char **argv) {
    const char *csv_path = NULL;
    const char *state_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0 && i+1 < argc) csv_path = argv[++i];
        if (strcmp(argv[i], "--state") == 0 && i+1 < argc) state_path = argv[++i];
    }

    /* Heap-allocate for large arrays */
    Trade *trades = calloc(MAX_TRADES, sizeof(Trade));
    double *returns = calloc(MAX_TRADES, sizeof(double));
    if (!trades || !returns) { fprintf(stderr, "malloc failed\n"); return 1; }

    int n_trades = 0;

    /* Read trade log CSV */
    if (csv_path) {
        FILE *f = fopen(csv_path, "r");
        if (!f) { fprintf(stderr, "Cannot open %s\n", csv_path); return 1; }

        char line[4096];
        /* Skip header */
        if (!fgets(line, sizeof(line), f)) { fclose(f); return 1; }

        while (fgets(line, sizeof(line), f) && n_trades < MAX_TRADES) {
            Trade *t = &trades[n_trades];
            /* Try: ts,pnl,capital,win */
            if (sscanf(line, "%lf,%lf,%lf,%lf", &t->ts, &t->pnl, &t->capital, &t->win) >= 3) {
                returns[n_trades] = t->pnl;
                n_trades++;
            }
        }
        fclose(f);
    }

    /* Read engine state JSON (simplified) */
    double capital = 0, win_rate = 0, total_trades = 0;
    if (state_path) {
        FILE *f = fopen(state_path, "r");
        if (f) {
            char buf[65536];
            size_t n = fread(buf, 1, sizeof(buf)-1, f);
            buf[n] = '\0';
            fclose(f);
            fprintf(stderr, "[RISK] State file: %zu bytes read\n", n);
            /* Simple JSON field extraction */
            char *p;
            if ((p = strstr(buf, "\"capital\":"))) capital = atof(p + 10);
            if ((p = strstr(buf, "\"trades_total\":"))) total_trades = atof(p + 15);
            if ((p = strstr(buf, "\"win_rate\":"))) win_rate = atof(p + 11);
            fprintf(stderr, "[RISK] capital=%.0f total_trades=%.0f wr=%.4f\n", capital, total_trades, win_rate);
        }
    }

    /* Compute metrics */
    double total_pnl = 0, total_sq = 0;
    double max_drawdown = 0, peak = 0;
    int wins = 0, losses = 0;
    double run_capital = capital > 0 ? capital : 10000;

    for (int i = 0; i < n_trades; i++) {
        total_pnl += trades[i].pnl;
        total_sq += trades[i].pnl * trades[i].pnl;
        if (trades[i].pnl > 0) wins++;
        else losses++;

        run_capital += trades[i].pnl;
        if (run_capital > peak) peak = run_capital;
        double dd = (peak - run_capital) / peak;
        if (dd > max_drawdown) max_drawdown = dd;
    }

    /* Sort returns for VaR */
    qsort(returns, n_trades, sizeof(double), cmp_double);

    double mean = n_trades > 0 ? total_pnl / n_trades : 0;
    double stddev = n_trades > 1 ? sqrt(total_sq / (n_trades - 1) - mean * mean * n_trades / (n_trades - 1)) : 0;
    double sharpe = stddev > 0 ? (mean / stddev) * sqrt(252.0) : 0;

    /* Sortino: downside deviation only */
    double downside_sq = 0;
    int downside_n = 0;
    for (int i = 0; i < n_trades; i++) {
        if (trades[i].pnl < 0) {
            downside_sq += trades[i].pnl * trades[i].pnl;
            downside_n++;
        }
    }
    double downside_std = downside_n > 0 ? sqrt(downside_sq / downside_n) : 1;
    double sortino = downside_std > 0 ? (mean / downside_std) * sqrt(252.0) : 0;

    double wr = n_trades > 0 ? (double)wins / n_trades : 0;
    double avg_win = wins > 0 ? total_pnl * wr / wins : 0;
    double avg_loss = losses > 0 ? total_pnl * (1 - wr) / losses : 0;
    double profit_factor = losses > 0 ? fabs((double)wins * avg_win / (losses * avg_loss)) : (wins > 0 ? 999 : 0);

    /* Output JSON */
    printf("{\n");
    printf("  \"trades\": %d,\n", n_trades);
    printf("  \"total_pnl\": %.2f,\n", total_pnl);
    printf("  \"win_rate\": %.4f,\n", wr);
    printf("  \"mean_return\": %.6f,\n", mean);
    printf("  \"stddev\": %.6f,\n", stddev);
    printf("  \"sharpe_ratio\": %.4f,\n", sharpe);
    printf("  \"sortino_ratio\": %.4f,\n", sortino);
    printf("  \"max_drawdown\": %.6f,\n", max_drawdown);
    printf("  \"profit_factor\": %.4f,\n", profit_factor);
    printf("  \"var_95\": %.6f,\n", calc_var(returns, n_trades, 0.95));
    printf("  \"var_99\": %.6f,\n", calc_var(returns, n_trades, 0.99));
    printf("  \"cvar_95\": %.6f,\n", calc_cvar(returns, n_trades, 0.95));
    printf("  \"avg_win\": %.4f,\n", avg_win);
    printf("  \"avg_loss\": %.4f,\n", avg_loss);
    printf("  \"capital\": %.2f,\n", capital);
    printf("  \"win_rate_state\": %.4f\n", win_rate);
    printf("}\n");

    free(trades);
    free(returns);
    return 0;
}
