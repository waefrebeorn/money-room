/*
 * trade_journal.c — Trade audit trail module (Money Room)
 * Reads real trade_log.csv from engine and outputs JSON audit to docs/data/
 *
 * CSV format: ts,agent_id,direction,size,entry_price,exit_price,won,pnl_pct,resolved_at,asset
 * Output: docs/data/trade_journal.json with aggregates + recent trades
 *
 * Compile: gcc -O2 -o trade_journal trade_journal.c -lm
 * Usage:   ./trade_journal                     (reads default path)
 *          ./trade_journal <path/to/trades.csv> (custom path)
 *          ./trade_journal --help               (this help)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define MAX_TRADES 50000
#define MAX_SUMMARY 100

static const char *DEFAULT_CSV = "/home/wubu2/.hermes/pm_logs/c_room/trade_log.csv";
static const char *OUTPUT_JSON = "../docs/data/trade_journal.json";

typedef struct {
    double ts;
    int    agent_id;
    char   direction[8];
    double size;
    double entry_price;
    double exit_price;
    char   won[8];
    double pnl_pct;
    double resolved_at;
    char   asset[32];
} Trade;

static int parse_trades(const char *csv_path, Trade *trades, int max) {
    FILE *f = fopen(csv_path, "r");
    if (!f) { fprintf(stderr, "[trade_journal] Cannot open %s\n", csv_path); return -1; }

    char line[4096];
    /* Read header */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }

    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        Trade *t = &trades[n];
        memset(t, 0, sizeof(Trade));
        int nf = sscanf(line, "%lf,%d,%7[^,],%lf,%lf,%lf,%7[^,],%lf,%lf,%31s",
                        &t->ts, &t->agent_id, t->direction,
                        &t->size, &t->entry_price, &t->exit_price,
                        t->won, &t->pnl_pct, &t->resolved_at, t->asset);
        if (nf < 7) continue;
        n++;
    }
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    const char *csv_path = DEFAULT_CSV;

    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Usage: %s [<trades.csv>]\n", argv[0]);
            printf("  Reads trade_log.csv, writes trade_journal.json to docs/data/\n");
            printf("  Default CSV: %s\n", DEFAULT_CSV);
            return 0;
        }
        csv_path = argv[1];
    }

    Trade *trades = calloc(MAX_TRADES, sizeof(Trade));
    if (!trades) { fprintf(stderr, "malloc failed\n"); return 1; }

    int n = parse_trades(csv_path, trades, MAX_TRADES);
    if (n < 0) { free(trades); return 1; }
    if (n == 0) {
        /* Write empty journal */
        FILE *out = fopen(OUTPUT_JSON, "w");
        if (!out) { perror("fopen output"); free(trades); return 1; }
        fprintf(out, "{\"trades\":0,\"gross_pnl\":0,\"win_rate\":0,\"max_drawdown\":0,\"summary\":[]}\n");
        fclose(out);
        printf("[trade_journal] Empty trade log, wrote empty journal\n");
        free(trades);
        return 0;
    }

    /* Compute aggregates */
    double gross_pnl = 0, peak = 0, max_dd = 0, cum = 0;
    int wins = 0, losses = 0;

    for (int i = 0; i < n; i++) {
        Trade *t = &trades[i];
        double dollar_pnl = t->size * t->pnl_pct;
        int is_win = (strncmp(t->won, "WIN", 3) == 0);

        gross_pnl += dollar_pnl;
        if (is_win) wins++; else losses++;

        cum += dollar_pnl;
        if (cum > peak) peak = cum;
        double dd = peak - cum;
        if (dd > max_dd) max_dd = dd;
    }

    double win_rate = (double)wins / n;
    double avg_pnl = gross_pnl / n;
    double total_fees = 0, total_slip = 0;
    for (int i = 0; i < n; i++) {
        Trade *t = &trades[i];
        total_fees += t->size * 0.0026 * 2;
        total_slip += t->size * 0.0005;
    }
    double net_pnl = gross_pnl - total_fees - total_slip;

    /* Write JSON output */
    FILE *out = fopen(OUTPUT_JSON, "w");
    if (!out) { perror("fopen output"); free(trades); return 1; }

    fprintf(out, "{\n");
    fprintf(out, "  \"trades\": %d,\n", n);
    fprintf(out, "  \"gross_pnl\": %.2f,\n", gross_pnl);
    fprintf(out, "  \"net_pnl\": %.2f,\n", net_pnl);
    fprintf(out, "  \"total_fees\": %.2f,\n", total_fees);
    fprintf(out, "  \"total_slippage\": %.2f,\n", total_slip);
    fprintf(out, "  \"wins\": %d,\n", wins);
    fprintf(out, "  \"losses\": %d,\n", losses);
    fprintf(out, "  \"win_rate\": %.4f,\n", win_rate);
    fprintf(out, "  \"avg_pnl_per_trade\": %.4f,\n", avg_pnl);
    fprintf(out, "  \"max_drawdown\": %.2f,\n", max_dd);
    fprintf(out, "  \"fee_ratio\": %.4f,\n", gross_pnl != 0 ? total_fees / fabs(gross_pnl) : 0);
    fprintf(out, "  \"summary\": [\n");

    int show = n < MAX_SUMMARY ? n : MAX_SUMMARY;
    for (int i = 0; i < show; i++) {
        Trade *t = &trades[i];
        double dollar_pnl = t->size * t->pnl_pct;
        char ts_str[32] = {0};
        time_t ts = (time_t)t->ts;
        strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M", gmtime(&ts));

        fprintf(out, "    {\"ts\":\"%s\",\"agent\":%d,\"dir\":\"%s\",\"size\":%.2f,"
                     "\"entry\":%.2f,\"exit\":%.2f,\"won\":%d,\"pnl\":%.2f,\"pnl_pct\":%.4f,\"asset\":\"%s\"}",
                ts_str, t->agent_id, t->direction, t->size,
                t->entry_price, t->exit_price,
                (strncmp(t->won, "WIN", 3) == 0) ? 1 : 0,
                dollar_pnl, t->pnl_pct, t->asset);
        if (i < show - 1) fprintf(out, ",\n");
        else fprintf(out, "\n");
    }

    fprintf(out, "  ]\n}\n");
    fclose(out);

    printf("[trade_journal] %d trades processed -> %s\n", n, OUTPUT_JSON);
    printf("  Gross PnL: $%.2f | Net: $%.2f | WR: %.1f%% | DD: $%.2f\n",
           gross_pnl, net_pnl, win_rate * 100, max_dd);

    free(trades);
    return 0;
}
