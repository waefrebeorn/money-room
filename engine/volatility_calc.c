/**
 * volatility_calc.c — Historical Volatility Calculator (HV10, HV30)
 * Reads OHLCV close prices from timeline.db, computes rolling HV.
 *
 * Compile: gcc -O2 -Wall -o volatility_calc volatility_calc.c -lcurl -ljansson -lsqlite3 -lm
 * Usage:   ./volatility_calc
 * Output:  docs/data/volatility.json
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sqlite3.h>
#include <jansson.h>

#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"

/* Annual trading days */
#define TRADING_DAYS 252.0

/* Tickers to compute volatility for */
static const char *TICKERS[] = {
    "SPY", "QQQ", "IWM", "DIA",
    "GLD", "SLV", "USO",
    "TLT", "IEF", "LQD",
    "EEM",
    "XLF", "XLK", "XLE", "XLB", "XLI", "XLV", "XLY", "XLP", "XLU",
    "VIG",
    "^VIX", "^TNX",
    "GC=F", "CL=F",
    "BTC-USD", "ETH-USD",
    NULL
};

/* Compute stats for an array of log returns */
static void compute_stats(const double *logrets, int n, double *mean, double *sd) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += logrets[i];
    *mean = sum / n;
    double sq = 0.0;
    for (int i = 0; i < n; i++) {
        double d = logrets[i] - *mean;
        sq += d * d;
    }
    *sd = (n > 1) ? sqrt(sq / (n - 1)) : 0.0; /* sample stddev */
}

int main(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "ERROR: can't open %s: %s\n", DB_PATH, sqlite3_errmsg(db));
        return 1;
    }

    json_t *root = json_object();
    json_t *tickers_arr = json_array();

    int total_ok = 0, total_skip = 0;

    for (int t = 0; TICKERS[t]; t++) {
        const char *tkr = TICKERS[t];

        /* Normalize ticker to source name */
        char src[80];
        int si = 0;
        for (int j = 0; tkr[j] && si < 70; j++) {
            char c = tkr[j];
            if (c == '=' || c == '^' || c == '-') src[si++] = '_';
            else src[si++] = c;
        }
        src[si] = 0;

        char source[96];
        snprintf(source, sizeof(source), "yahoo_%s", src);

        /* Fetch close prices sorted by timestamp */
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT data, ts FROM timeline WHERE source=?1 ORDER BY ts ASC",
                -1, &st, 0) != SQLITE_OK) {
            fprintf(stderr, "[%s] query prep failed\n", tkr);
            continue;
        }
        sqlite3_bind_text(st, 1, source, -1, SQLITE_STATIC);

        /* First pass: count */
        double *closes = NULL;
        int n = 0, cap = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *data = (const char*)sqlite3_column_text(st, 0);
            if (!data) continue;

            json_t *jd = json_loads(data, 0, NULL);
            if (!jd) continue;
            json_t *jc = json_object_get(jd, "close");
            double close_val = 0.0;
            if (jc && json_is_real(jc)) close_val = json_real_value(jc);
            json_decref(jd);

            if (close_val <= 0.0) continue;

            if (n >= cap) {
                cap = cap ? cap * 2 : 64;
                double *tmp = realloc(closes, cap * sizeof(double));
                if (!tmp) { free(closes); sqlite3_finalize(st); return 1; }
                closes = tmp;
            }
            closes[n++] = close_val;
        }
        sqlite3_finalize(st);

        if (n < 2) {
            free(closes);
            total_skip++;
            continue;
        }

        /* Compute log returns: ln(c_i / c_{i-1}) */
        int nr = n - 1;
        double *logrets = malloc(nr * sizeof(double));
        for (int i = 0; i < nr; i++) {
            double r = closes[i+1] / closes[i];
            logrets[i] = log(r);
        }

        /* Remove outliers: filter returns > 4 sigma from mean for clean HV */
        double mean_r, sd_r;
        compute_stats(logrets, nr, &mean_r, &sd_r);
        double threshold = 4.0 * sd_r;
        int nf = 0;
        double *filtered = malloc(nr * sizeof(double));
        for (int i = 0; i < nr; i++) {
            if (fabs(logrets[i] - mean_r) <= threshold)
                filtered[nf++] = logrets[i];
        }

        /* Compute HV10: stddev of last min(10, nf) log returns */
        double hv10 = 0.0, hv30 = 0.0;
        if (nf >= 10) {
            double m10, sd10;
            compute_stats(filtered + (nf - 10), 10, &m10, &sd10);
            hv10 = sd10 * sqrt(TRADING_DAYS) * 100.0; /* as percentage */
        }

        /* Compute HV30 if we have enough data */
        if (nf >= 30) {
            double m30, sd30;
            compute_stats(filtered + (nf - 30), 30, &m30, &sd30);
            hv30 = sd30 * sqrt(TRADING_DAYS) * 100.0;
        } else if (nf >= 10) {
            /* Scale HV10 to 30-day estimate */
            double m10, sd10;
            compute_stats(filtered + (nf - 10), 10, &m10, &sd10);
            hv30 = sd10 * sqrt(TRADING_DAYS) * 100.0;
        }

        free(logrets);
        free(filtered);

        double mn = closes[0], mx = closes[0];
        for (int i = 1; i < n; i++) {
            if (closes[i] < mn) mn = closes[i];
            if (closes[i] > mx) mx = closes[i];
        }
        json_t *entry = json_pack("{s:s, s:i, s:f, s:f, s:f, s:f, s:f}",
            "ticker", tkr,
            "n", n,
            "hv10", hv10,
            "hv30", hv30,
            "last_close", closes[n-1],
            "min_close", mn,
            "max_close", mx);
        json_array_append_new(tickers_arr, entry);
        free(closes);
        total_ok++;
    }

    json_object_set_new(root, "tickers", tickers_arr);
    json_object_set_new(root, "total_tickers", json_integer(total_ok));
    json_object_set_new(root, "skipped", json_integer(total_skip));
    json_object_set_new(root, "updated_at", json_string("now"));

    /* Write output to stdout (wrapper redirects to JSON) */
    char *out = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS);
    json_decref(root);

    if (!out) {
        fprintf(stderr, "ERROR: json_dumps failed\n");
        sqlite3_close(db);
        return 1;
    }

    printf("%s\n", out);
    free(out);

    fprintf(stderr, "[volatility] %d tickers computed, %d skipped\n",
           total_ok, total_skip);
    sqlite3_close(db);
    return 0;
}
