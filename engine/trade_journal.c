/*
 * trade_journal.c — Trade audit trail module
 * Reads trade log CSV, enriches with fees/slippage, outputs JSON audit
 *
 * CSV format: ts,pnl,capital,win,entry_price,exit_price,size,exchange
 * Output: JSON trade list with per-trade PnL attribution
 *
 * Compile: gcc -O3 -o trade_journal trade_journal.c -lm
 * Usage:   ./trade_journal <trades.csv> [--json] [--html]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define MAX_TRADES 100000

typedef struct {
    double ts;
    double pnl;
    double capital;
    int    win;
    double entry;
    double exit;
    double size;
    char   exchange[32];
    double fees;
    double slippage;
    double net_pnl;
    double pnl_pct;
} Trade;

int main(int argc, char **argv) {
    const char *csv_path = argc > 1 ? argv[1] : NULL;
    int output_json = 1, output_html = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) output_json = 1;
        if (strcmp(argv[i], "--html") == 0) { output_html = 1; output_json = 0; }
    }

    if (!csv_path) {
        fprintf(stderr, "Usage: %s <trades.csv> [--json] [--html]\n", argv[0]);
        return 1;
    }

    Trade *trades = calloc(MAX_TRADES, sizeof(Trade));
    int n = 0;

    FILE *f = fopen(csv_path, "r");
    if (!f) { perror("fopen"); free(trades); return 1; }

    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); free(trades); return 1; }

    while (fgets(line, sizeof(line), f) && n < MAX_TRADES) {
        Trade *t = &trades[n];
        t->exchange[0] = '\0';
        int nf = sscanf(line, "%lf,%lf,%lf,%d,%lf,%lf,%lf,%31s",
                        &t->ts, &t->pnl, &t->capital, &t->win,
                        &t->entry, &t->exit, &t->size, t->exchange);
        if (nf < 3) continue;

        /* Default size if not provided */
        if (nf < 7 || t->size <= 0) t->size = fabs(t->pnl) * 10;
        if (nf < 8) strcpy(t->exchange, "unknown");

        /* Fee estimate (simple 0.26% for estimation) */
        t->fees = t->size * 0.0026f;
        /* Slippage estimate (5bps baseline) */
        t->slippage = t->size * 0.0005f;
        t->net_pnl = t->pnl - t->fees - t->slippage;
        t->pnl_pct = t->size > 0 ? (t->pnl / t->size) * 100 : 0;
        n++;
    }
    fclose(f);

    /* Compute aggregates */
    double gross_pnl = 0, net_pnl = 0, total_fees = 0, total_slip = 0;
    int wins = 0, losses = 0;
    double max_drawdown = 0, peak = 0;
    double cum_pnl = 0;
    double *cum_series = calloc(n, sizeof(double));

    for (int i = 0; i < n; i++) {
        gross_pnl += trades[i].pnl;
        net_pnl += trades[i].net_pnl;
        total_fees += trades[i].fees;
        total_slip += trades[i].slippage;
        if (trades[i].pnl > 0) wins++; else losses++;
        cum_pnl += trades[i].pnl;
        cum_series[i] = cum_pnl;
        if (cum_pnl > peak) peak = cum_pnl;
        double dd = (peak - cum_pnl);
        if (dd > max_drawdown) max_drawdown = dd;
    }

    if (output_html) {
        /* Generate simple HTML audit page */
        printf("<html><head><title>Trade Journal</title>");
        printf("<style>body{font-family:monospace;background:#020617;color:white;padding:2rem}");
        printf("table{border-collapse:collapse;width:100%%;font-size:12px}");
        printf("th{background:#1e293b;padding:8px;text-align:left}");
        printf("td{padding:4px 8px;border-bottom:1px solid #1e293b}");
        printf(".win{color:#34d399}.loss{color:#fb7185}");
        printf("h2{color:#22d3ee}.summary{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:1rem;margin:1rem 0}");
        printf(".card{background:rgba(15,23,42,0.5);border:1px solid #1e293b;border-radius:8px;padding:1rem}");
        printf(".label{color:#94a3b8;font-size:11px}.value{font-size:18px;font-weight:700}");
        printf("</style></head><body>");
        printf("<h2>Trade Audit Trail</h2>");
        printf("<div class=summary>");
        printf("<div class=card><div class=label>Trades</div><div class=value>%d</div></div>", n);
        printf("<div class=card><div class=label>Gross PnL</div><div class=value>%+.2f</div></div>", gross_pnl);
        printf("<div class=card><div class=label>Net PnL</div><div class=value>%+.2f</div></div>", net_pnl);
        printf("<div class=card><div class=label>Win Rate</div><div class=value>%.1f%%</div></div>", n>0?100.0*wins/n:0);
        printf("<div class=card><div class=label>Fees Paid</div><div class=value>$%.2f</div></div>", total_fees);
        printf("<div class=card><div class=label>Slippage</div><div class=value>$%.2f</div></div>", total_slip);
        printf("</div>");
        printf("<table><tr><th>Time</th><th>PnL</th><th>Net</th><th>Size</th><th>Entry</th><th>Exit</th><th>Fees</th><th>Slip</th><th>Exch</th><th>W/L</th></tr>");
        for (int i = 0; i < n && i < 100; i++) {
            Trade *t = &trades[i];
            char ts_str[32];
            time_t ts = (time_t)t->ts;
            strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M", gmtime(&ts));
            printf("<tr><td>%s</td><td>%+.2f</td><td>%+.2f</td><td>$%.0f</td><td>%.2f</td><td>%.2f</td>",
                   ts_str, t->pnl, t->net_pnl, t->size, t->entry, t->exit);
            printf("<td>$%.2f</td><td>$%.2f</td><td>%s</td>",
                   t->fees, t->slippage, t->exchange);
            printf("<td class=%s>%s</td></tr>", t->win?"win":"loss", t->win?"WIN":"LOSS");
        }
        printf("</table></body></html>");
    } else {
        /* JSON output */
        printf("{\n");
        printf("  \"trades\": %d,\n", n);
        printf("  \"gross_pnl\": %.2f,\n", gross_pnl);
        printf("  \"net_pnl\": %.2f,\n", net_pnl);
        printf("  \"total_fees\": %.2f,\n", total_fees);
        printf("  \"total_slippage\": %.2f,\n", total_slip);
        printf("  \"wins\": %d,\n", wins);
        printf("  \"losses\": %d,\n", losses);
        printf("  \"win_rate\": %.4f,\n", n > 0 ? (double)wins / n : 0);
        printf("  \"max_drawdown\": %.2f,\n", max_drawdown);
        printf("  \"fee_ratio\": %.4f,\n", gross_pnl != 0 ? total_fees / fabs(gross_pnl) : 0);
        printf("  \"summary\": [\n");
        for (int i = 0; i < n && i < 100; i++) {
            Trade *t = &trades[i];
            printf("    {%u,%+.2f,%+.2f,%.0f,%.2f,%.2f,%.4f,\"exch\":\"%s\"}",
                   (unsigned)t->ts, t->pnl, t->net_pnl, t->size,
                   t->entry, t->exit, t->pnl_pct, t->exchange);
            printf("%s\n", i < n-1 && i < 99 ? "," : "");
        }
        printf("  ]\n}\n");
    }

    free(trades);
    free(cum_series);
    return 0;
}
