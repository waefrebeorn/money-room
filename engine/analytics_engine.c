/*
 * analytics_engine.c — PnL Attribution & Correlation Matrix
 *
 * PnL Attribution: decomposes PnL into feature-level contributions
 * Correlation Matrix: pairwise asset correlations (Pearson)
 *
 * Compile: gcc -O3 -o analytics_engine analytics_engine.c -lm
 * Usage:   ./analytics_engine pnl <trades.csv>
 *          ./analytics_engine corr <prices1.csv> [prices2.csv ...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_TRADES 50000
#define MAX_ASSETS 16
#define MAX_CANDLES 50000

/* ── PnL Attribution ── */
/* Decompose PnL into: timing, sizing, direction, fee efficiency */

typedef struct {
    double total_pnl;
    int    trades;
    int    wins;
    double timing_score;    /* Entry/exit quality */
    double sizing_score;    /* Position sizing efficiency */
    double direction_score; /* Long/short prediction accuracy */
    double fee_efficiency;  /* Fee cost vs expectation */
    double avg_hold;        /* Average hold time (seconds) */
    double win_loss_ratio;  /* Avg win / avg loss */
} PnLAttribution;

PnLAttribution calc_attribution(const char *csv_path) {
    PnLAttribution a = {0};

    FILE *f = fopen(csv_path, "r");
    if (!f) return a;

    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return a; }

    double last_ts = 0;
    double total_win = 0, total_loss = 0;
    int win_count = 0, loss_count = 0;
    double total_inv = 0;

    while (fgets(line, sizeof(line), f) && a.trades < MAX_TRADES) {
        double ts, pnl, cap, entry, exit, size;
        int win, nf;
        nf = sscanf(line, "%lf,%lf,%lf,%d,%lf,%lf,%lf",
                    &ts, &pnl, &cap, &win, &entry, &exit, &size);

        if (nf < 4) continue;
        a.total_pnl += pnl;
        a.trades++;

        if (pnl > 0) { total_win += pnl; win_count++; }
        else { total_loss += fabs(pnl); loss_count++; }
        a.wins += win;

        if (nf >= 7 && size > 0) total_inv += size;

        /* Timing score: higher if entry is close to low/high */
        if (nf >= 6 && entry > 0 && exit > 0) {
            double range = fabs(exit - entry);
            double move = pnl / size;
            /* Positive move from favorable entry = good timing */
            a.timing_score += move > 0 ? fmin(move * 100, 5) : fmax(move * 100, -5);
        }

        /* Hold time */
        if (last_ts > 0) a.avg_hold += fabs(ts - last_ts);
        last_ts = ts;
    }
    fclose(f);

    if (a.trades > 0) {
        a.timing_score /= a.trades;
        a.avg_hold /= a.trades;
        a.win_loss_ratio = loss_count > 0 ? total_win / win_count / (total_loss / loss_count) : 0;
        a.sizing_score = a.trades > 0 ? fmin(total_inv / a.trades / 100.0, 1.0) : 0;
        a.direction_score = (double)a.wins / a.trades;
        a.fee_efficiency = a.total_pnl != 0 ?
            fmin(1.0, 1.0 - (a.trades * 0.0026 * total_inv / a.trades) / fabs(a.total_pnl)) : 0;
    }

    return a;
}

/* ── Correlation Matrix ── */
/* Pearson correlation between pairs of price series */

static double pearson(double *x, double *y, int n) {
    double sx = 0, sy = 0, sxy = 0, sx2 = 0, sy2 = 0;
    for (int i = 0; i < n; i++) {
        sx += x[i]; sy += y[i];
        sxy += x[i] * y[i];
        sx2 += x[i] * x[i];
        sy2 += y[i] * y[i];
    }
    double num = n * sxy - sx * sy;
    double den = sqrt((n * sx2 - sx * sx) * (n * sy2 - sy * sy));
    return den != 0 ? num / den : 0;
}

static int read_returns(const char *path, double *returns, int max_n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }

    double prev_close = 0;
    int n = 0;

    while (fgets(line, sizeof(line), f) && n < max_n) {
        double ts, open, high, low, close, vol;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf",
                   &ts, &open, &high, &low, &close, &vol) >= 5) {
            if (prev_close > 0) {
                returns[n++] = log(close / prev_close);
            }
            prev_close = close;
        }
    }
    fclose(f);
    return n;
}

/* ── Main ── */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s pnl <trades.csv>\n", argv[0]);
        fprintf(stderr, "  %s corr <price1.csv> [price2.csv ...]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "pnl") == 0) {
        PnLAttribution a = calc_attribution(argv[2]);
        printf("{\n");
        printf("  \"total_pnl\": %.2f,\n", a.total_pnl);
        printf("  \"trades\": %d,\n", a.trades);
        printf("  \"wins\": %d,\n", a.wins);
        printf("  \"direction_score\": %.4f,\n", a.direction_score);
        printf("  \"timing_score\": %.4f,\n", a.timing_score);
        printf("  \"sizing_score\": %.4f,\n", a.sizing_score);
        printf("  \"fee_efficiency\": %.4f,\n", a.fee_efficiency);
        printf("  \"avg_hold_sec\": %.0f,\n", a.avg_hold);
        printf("  \"win_loss_ratio\": %.4f\n", a.win_loss_ratio);
        printf("}\n");
    } else if (strcmp(argv[1], "corr") == 0) {
        int n_assets = argc - 2;
        if (n_assets > MAX_ASSETS) n_assets = MAX_ASSETS;

        double *returns[MAX_ASSETS];
        int n_points[MAX_ASSETS];
        char labels[MAX_ASSETS][64];

        for (int i = 0; i < n_assets; i++) {
            char *name = strrchr(argv[2+i], '/');
            name = name ? name + 1 : argv[2+i];
            char *dot = strrchr(name, '.');
            int len = dot ? (int)(dot - name) : (int)strlen(name);
            snprintf(labels[i], sizeof(labels[i]), "%.*s", len > 15 ? 15 : len, name);

            returns[i] = calloc(MAX_CANDLES, sizeof(double));
            n_points[i] = read_returns(argv[2+i], returns[i], MAX_CANDLES);
        }

        printf("{\n");
        printf("  \"assets\": [\n");
        /* Header row */
        printf("    [\"CORR\"");

        for (int i = 0; i < n_assets; i++) {
            printf(",\"%s\"", labels[i]);
        }
        printf("],\n");

        /* Data rows */
        for (int i = 0; i < n_assets; i++) {
            printf("    [\"%s\"", labels[i]);
            for (int j = 0; j < n_assets; j++) {
                int n = n_points[i] < n_points[j] ? n_points[i] : n_points[j];
                double r = (n > 5) ? pearson(returns[i], returns[j], n) : 0;
                printf(",%.4f", r);
            }
            printf("],\n");
        }
        printf("  ]\n");
        printf("}\n");

        for (int i = 0; i < n_assets; i++) free(returns[i]);
    }

    return 0;
}
