/*
 * regime_detect.c — Market regime detection module
 * Classifies market into RANGE (0), TREND (1), or VOLATILE (2)
 * Based on ADX, ATR ratio, Bollinger Band width, and price position
 *
 * Compile: gcc -O3 -o regime_detect regime_detect.c -lm
 * Usage:   ./regime_detect <prices.csv> [--window N] [--smooth N]
 *          CSV format: timestamp,open,high,low,close,volume
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_CANDLES 100000
#define MIN_WINDOW  14

typedef struct {
    double ts, open, high, low, close, volume;
} Candle;

typedef struct {
    int regime;         /* 0=range, 1=trend, 2=volatile */
    double adx;         /* Average Directional Index (0-100) */
    double atr_pct;     /* ATR as % of price */
    double bb_width;    /* Bollinger Band width (std devs) */
    double price_pos;   /* Price position in range [0,1] */
    double volatility;  /* 20-day annualized vol */
    const char *label;
} RegimeResult;

static int cmp_candle_ts(const void *a, const void *b) {
    const Candle *ca = (const Candle *)a;
    const Candle *cb = (const Candle *)b;
    return (ca->ts > cb->ts) - (ca->ts < cb->ts);
}

/* SMA */
static double sma(const double *vals, int n, int period, int idx) {
    if (idx < period - 1) return vals[idx];
    double sum = 0;
    for (int i = idx - period + 1; i <= idx; i++) sum += vals[i];
    return sum / period;
}

/* Standard deviation */
static double stddev(const double *vals, int n, int period, int idx) {
    if (idx < period) return 0;
    double m = sma(vals, n, period, idx);
    double sum = 0;
    int count = 0;
    for (int i = idx - period + 1; i <= idx; i++) {
        double d = vals[i] - m;
        sum += d * d;
        count++;
    }
    return count > 1 ? sqrt(sum / (count - 1)) : 0;
}

/* True Range */
static double true_range(const Candle *c, const Candle *prev) {
    if (!prev) return c->high - c->low;
    double hl = c->high - c->low;
    double hcp = fabs(c->high - prev->close);
    double lcp = fabs(c->low - prev->close);
    return fmax(hl, fmax(hcp, lcp));
}

/* Directional Movement */
static double dm_plus(const Candle *c, const Candle *prev) {
    if (!prev) return 0;
    double up = c->high - prev->high;
    double down = prev->low - c->low;
    if (up > down && up > 0) return up;
    return 0;
}

static double dm_minus(const Candle *c, const Candle *prev) {
    if (!prev) return 0;
    double up = c->high - prev->high;
    double down = prev->low - c->low;
    if (down > up && down > 0) return down;
    return 0;
}

RegimeResult detect_regime(const Candle *candles, int n, int window, int smooth) {
    RegimeResult r = {0};
    if (n < window + 10) { r.label = "INSUFFICIENT_DATA"; return r; }

    if (window < MIN_WINDOW) window = MIN_WINDOW;
    if (smooth < 1) smooth = 3;

    /* Compute ATR */
    double atr_sum = 0;
    int atr_count = 0;
    for (int i = 1; i < n; i++) {
        double tr = true_range(&candles[i], &candles[i-1]);
        if (i >= n - window) { atr_sum += tr; atr_count++; }
    }
    double atr = atr_count > 0 ? atr_sum / atr_count : 0;
    double atr_pct = candles[n-1].close > 0 ? atr / candles[n-1].close * 100 : 0;

    /* Compute ADX */
    double dx_sum = 0;
    int dx_count = 0;
    double *tr_arr = calloc(n, sizeof(double));
    double *dm_p = calloc(n, sizeof(double));
    double *dm_m = calloc(n, sizeof(double));

    for (int i = 1; i < n; i++) {
        tr_arr[i] = true_range(&candles[i], &candles[i-1]);
        dm_p[i] = dm_plus(&candles[i], &candles[i-1]);
        dm_m[i] = dm_minus(&candles[i], &candles[i-1]);
    }

    /* Smoothed TR, DM+, DM- */
    double *tr_s = calloc(n, sizeof(double));
    double *dp_s = calloc(n, sizeof(double));
    double *dm_s = calloc(n, sizeof(double));

    for (int i = 0; i < n; i++) {
        if (i < smooth) {
            tr_s[i] = tr_arr[i];
            dp_s[i] = dm_p[i];
            dm_s[i] = dm_m[i];
        } else {
            tr_s[i] = tr_s[i-1] * (smooth - 1) / smooth + tr_arr[i] / smooth;
            dp_s[i] = dp_s[i-1] * (smooth - 1) / smooth + dm_p[i] / smooth;
            dm_s[i] = dm_s[i-1] * (smooth - 1) / smooth + dm_m[i] / smooth;
        }
    }

    /* ADX */
    double adx_sum = 0;
    int adx_count = 0;
    for (int i = smooth * 2; i < n; i++) {
        double di_p = tr_s[i] > 0 ? dp_s[i] / tr_s[i] * 100 : 0;
        double di_m = tr_s[i] > 0 ? dm_s[i] / tr_s[i] * 100 : 0;
        double di_sum = di_p + di_m;
        double dx = di_sum > 0 ? fabs(di_p - di_m) / di_sum * 100 : 0;

        if (i >= n - window) {
            dx_sum += dx;
            adx_count++;
        }
    }
    r.adx = adx_count > 0 ? dx_sum / adx_count : 0;

    free(tr_arr); free(dm_p); free(dm_m);
    free(tr_s); free(dp_s); free(dm_s);

    /* Bollinger Band width */
    int bb_period = 20;
    double *close_arr = malloc(n * sizeof(double));
    for (int i = 0; i < n; i++) close_arr[i] = candles[i].close;

    double recent_sma = sma(close_arr, n, bb_period, n-1);
    double recent_sd = stddev(close_arr, n, bb_period, n-1);
    r.bb_width = recent_sd > 0 ? recent_sd / recent_sma * 100 : 0;

    /* Price position in recent range */
    int lookback = 20;
    double hi = -1e99, lo = 1e99;
    for (int i = n - lookback; i < n; i++) {
        if (i < 0) continue;
        if (candles[i].high > hi) hi = candles[i].high;
        if (candles[i].low < lo) lo = candles[i].low;
    }
    r.price_pos = (hi - lo) > 0 ? (candles[n-1].close - lo) / (hi - lo) : 0.5;

    /* Annualized volatility (20-day) */
    int vol_period = 20;
    double ret_sum = 0, ret_sq = 0;
    for (int i = n - vol_period; i < n - 1; i++) {
        if (i < 0) continue;
        double ret = log(candles[i+1].close / candles[i].close);
        ret_sum += ret;
        ret_sq += ret * ret;
    }
    double ret_mean = ret_sum / (vol_period - 1);
    double ret_var = ret_sq / (vol_period - 1) - ret_mean * ret_mean;
    r.volatility = ret_var > 0 ? sqrt(ret_var * 252) * 100 : 0;

    /* Classify regime */
    if (atr_pct > 3.0 || r.volatility > 60) {
        r.regime = 2;  /* VOLATILE */
        r.label = "VOLATILE";
    } else if (r.adx > 25) {
        r.regime = 1;  /* TREND */
        r.label = "TREND";
    } else {
        r.regime = 0;  /* RANGE */
        r.label = "RANGE";
    }

    r.atr_pct = atr_pct;
    free(close_arr);
    return r;
}

int main(int argc, char **argv) {
    const char *csv_path = argc > 1 ? argv[1] : NULL;
    int window = MIN_WINDOW;
    int smooth = 3;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--window") == 0 && i+1 < argc) window = atoi(argv[++i]);
        if (strcmp(argv[i], "--smooth") == 0 && i+1 < argc) smooth = atoi(argv[++i]);
    }

    if (!csv_path) {
        fprintf(stderr, "Usage: %s <prices.csv> [--window N] [--smooth N]\n", argv[0]);
        return 1;
    }

    Candle *candles = calloc(MAX_CANDLES, sizeof(Candle));
    int n = 0;

    FILE *f = fopen(csv_path, "r");
    if (!f) { perror("fopen"); free(candles); return 1; }

    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); free(candles); return 1; }

    while (fgets(line, sizeof(line), f) && n < MAX_CANDLES) {
        Candle *c = &candles[n];
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf",
                   &c->ts, &c->open, &c->high, &c->low, &c->close, &c->volume) >= 5) {
            n++;
        }
    }
    fclose(f);

    qsort(candles, n, sizeof(Candle), cmp_candle_ts);

    RegimeResult r = detect_regime(candles, n, window, smooth);

    printf("{\n");
    printf("  \"regime\": %d,\n", r.regime);
    printf("  \"label\": \"%s\",\n", r.label);
    printf("  \"adx\": %.2f,\n", r.adx);
    printf("  \"atr_pct\": %.2f,\n", r.atr_pct);
    printf("  \"bb_width\": %.2f,\n", r.bb_width);
    printf("  \"price_position\": %.3f,\n", r.price_pos);
    printf("  \"volatility_annual\": %.1f,\n", r.volatility);
    printf("  \"candles\": %d,\n", n);
    printf("  \"window\": %d\n", window);
    printf("}\n");

    free(candles);
    return 0;
}
