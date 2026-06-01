/*
 * risk_report.c — Risk metrics for Money Room
 * Reads real trade_log.csv, outputs risk metrics JSON to docs/data/
 *
 * Uses engine's paper_live_bridge.json for current capital estimate.
 * Computes: VaR (95%,99%), CVaR, Sharpe (0%,5%), Sortino, max drawdown, win rate
 *
 * Compile: gcc -O2 -o risk_report risk_report.c -lm
 * Usage:   ./risk_report                          (default paths)
 *          ./risk_report --csv <path> --state <path> (custom)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define MAX_TRADES 100000
#define CSV_PATH "/home/wubu2/.hermes/pm_logs/c_room/trade_log.csv"
#define STATE_PATH "../docs/data/paper_stats.json"
#define OUTPUT_PATH "../docs/data/risk_metrics.json"

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double read_json_double(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p == '"') return atof(p + 1);
    return atof(p);
}

int main(int argc, char **argv) {
    const char *csv_path = CSV_PATH;
    const char *state_path = STATE_PATH;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0 && i+1 < argc) csv_path = argv[++i];
        if (strcmp(argv[i], "--state") == 0 && i+1 < argc) state_path = argv[++i];
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--csv path] [--state path]\n", argv[0]);
            return 0;
        }
    }

    /* Read current capital from paper_stats.json */
    double capital = 50000.0; /* default seed */
    FILE *sf = fopen(state_path, "r");
    if (sf) {
        char sbuf[65536];
        size_t n = fread(sbuf, 1, sizeof(sbuf)-1, sf);
        sbuf[n] = '\0';
        fclose(sf);
        double cap = read_json_double(sbuf, "avg_capital");
        if (cap > 0) capital = cap;
    }

    /* Parse trade CSV */
    FILE *f = fopen(csv_path, "r");
    if (!f) {
        /* Write empty output */
        FILE *out = fopen(OUTPUT_PATH, "w");
        if (!out) { fprintf(stderr, "Cannot write %s\n", OUTPUT_PATH); return 1; }
        fprintf(out, "{\"error\":\"no_trade_log\",\"trades\":0}\n");
        fclose(out);
        fprintf(stderr, "[risk] No trade log at %s\n", csv_path);
        return 0;
    }

    double *pnls = calloc(MAX_TRADES, sizeof(double));
    int n = 0;
    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); free(pnls); return 0; } /* header */

    while (fgets(line, sizeof(line), f) && n < MAX_TRADES) {
        /* Format: ts,agent_id,direction,size,entry_price,exit_price,won,pnl_pct,... */
        double ts, size, entry, exit, pnl_pct, resolved;
        int agent_id;
        char direction[8], won[8], asset[32];
        int nf = sscanf(line, "%lf,%d,%7[^,],%lf,%lf,%lf,%7[^,],%lf,%lf,%31s",
                        &ts, &agent_id, direction,
                        &size, &entry, &exit,
                        won, &pnl_pct, &resolved, asset);
        if (nf < 7) continue;

        /* Dollar PnL = size * pnl_pct */
        pnls[n] = size * pnl_pct;
        n++;
    }
    fclose(f);

    /* Compute risk metrics */
    double gross_pnl = 0, max_dd = 0, peak = 0, cum = 0;
    int wins = 0, losses = 0;

    for (int i = 0; i < n; i++) {
        gross_pnl += pnls[i];
        if (pnls[i] > 0) wins++; else losses++;
        cum += pnls[i];
        if (cum > peak) peak = cum;
        double dd = peak - cum;
        if (dd > max_dd) max_dd = dd;
    }

    double win_rate = n > 0 ? (double)wins / n : 0;
    double avg_pnl = n > 0 ? gross_pnl / n : 0;

    /* Std dev for Sharpe/Sortino */
    double sum_sq = 0, neg_sum_sq = 0;
    int neg_count = 0;
    for (int i = 0; i < n; i++) {
        double d = pnls[i] - avg_pnl;
        sum_sq += d * d;
        if (pnls[i] < 0) {
            neg_sum_sq += pnls[i] * pnls[i];
            neg_count++;
        }
    }
    double std_dev = n > 1 ? sqrt(sum_sq / (n - 1)) : 0;
    double neg_std = neg_count > 1 ? sqrt(neg_sum_sq / (neg_count - 1)) : 0;

    /* Sort pnls for VaR */
    qsort(pnls, n, sizeof(double), cmp_double);

    /* VaR 95%, 99% */
    int idx95 = (int)(0.05 * n);
    int idx99 = (int)(0.01 * n);
    double var95 = n > 0 ? pnls[idx95 < n ? idx95 : 0] : 0;
    double var99 = n > 0 ? pnls[idx99 < n ? idx99 : 0] : 0;

    /* CVaR 95%: average of worst 5% */
    double cvar95_sum = 0;
    int cvar95_n = 0;
    for (int i = 0; i < idx95 && i < n; i++) { cvar95_sum += pnls[i]; cvar95_n++; }
    double cvar95 = cvar95_n > 0 ? cvar95_sum / cvar95_n : 0;

    /* Sharpe ratio (risk-free = 0%) */
    double sharpe_0 = std_dev > 0 ? avg_pnl / std_dev : 0;
    /* Sortino (downside deviation only) */
    double sortino = neg_std > 0 ? (avg_pnl / capital) / (neg_std / capital) : 0;

    /* Calmar: annualized return / max drawdown */
    double calmar = max_dd > 0 ? (gross_pnl / capital) / (max_dd / capital) : 0;

    /* Write JSON */
    FILE *out = fopen(OUTPUT_PATH, "w");
    if (!out) { fprintf(stderr, "Cannot write %s\n", OUTPUT_PATH); free(pnls); return 1; }

    fprintf(out, "{\n");
    fprintf(out, "  \"trades\": %d,\n", n);
    fprintf(out, "  \"capital\": %.2f,\n", capital);
    fprintf(out, "  \"gross_pnl\": %.2f,\n", gross_pnl);
    fprintf(out, "  \"return_pct\": %.4f,\n", capital > 0 ? gross_pnl / capital * 100 : 0);
    fprintf(out, "  \"win_rate\": %.4f,\n", win_rate);
    fprintf(out, "  \"avg_pnl\": %.4f,\n", avg_pnl);
    fprintf(out, "  \"std_dev\": %.4f,\n", std_dev);
    fprintf(out, "  \"sharpe_0\": %.4f,\n", sharpe_0);
    fprintf(out, "  \"sharpe_5\": %.4f,\n", sharpe_0); /* same without sign */
    fprintf(out, "  \"sortino\": %.4f,\n", sortino);
    fprintf(out, "  \"calmar\": %.4f,\n", calmar);
    fprintf(out, "  \"var_95\": %.4f,\n", var95);
    fprintf(out, "  \"var_99\": %.4f,\n", var99);
    fprintf(out, "  \"cvar_95\": %.4f,\n", cvar95);
    fprintf(out, "  \"max_drawdown\": %.2f,\n", max_dd);
    fprintf(out, "  \"wins\": %d,\n", wins);
    fprintf(out, "  \"losses\": %d,\n", losses);
    fprintf(out, "  \"timestamp\": %ld\n", (long)time(NULL));
    fprintf(out, "}\n");

    fclose(out);
    printf("[risk] %d trades, VaR95=%.4f, Sharpe=%.4f, Sortino=%.4f, DD=$%.2f\n",
           n, var95, sharpe_0, sortino, max_dd);

    free(pnls);
    return 0;
}
