/**
 * training_pile.c — Unified Training Pile Assembler
 *
 * AGGREGATION IS THE MAGIC.
 * Past data + present data + all sources = one unified training tensor.
 *
 * Reads timeline.db, aligns ALL sources to 1-min timestamps,
 * outputs an engine-compatible training CSV with target column.
 *
 * Sources normalized:
 *   crypto (1-min):     close price from bitstamp_1min, kraken_*, binance
 *   macro (daily):      sp500, vix, gold, dxy — forward-filled to 1-min
 *   sentiment:          fear_greed — forward-filled
 *   on-chain:           defillama TVL — forward-filled
 *   prediction markets: polymarket — latest price
 *   news:               gdelt — event count + sentiment
 *
 * NOT FINANCIAL ADVICE. Algorithmic analysis only.
 *
 * Build: make training-pile  (see Makefile)
 * Usage: ./training_pile [days] [output.csv]
 *        ./training_pile 365  # last year
 *        ./training_pile 0 ~/training_data.csv  # all data
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sqlite3.h>
#include <time.h>

#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define MAX_TIMESTAMPS 2000000
#define MAX_SOURCES 50

/* ── Source mapping: what data field to extract ── */
typedef struct {
    char name[64];          /* display name */
    char db_source[64];     /* timeline.source value */
    char json_field[32];    /* "close", "value", "price" */
    double fallback;        /* default if missing */
    double *buffer;         /* aligned to timestamps */
    int count;
    double latest;          /* for forward-fill */
} DataSource;

/* ── Timestamp index ── */
typedef struct {
    long long *ts;
    int count;
    int capacity;
} TimestampIndex;

/* ── SQLite helper ── */
static sqlite3 *g_db = NULL;

static int open_db(void) {
    return sqlite3_open(DB_PATH, &g_db);
}

static void close_db(void) {
    if (g_db) sqlite3_close(g_db);
}

/* ── Extract value from JSON ── */
static double extract_json_val(const char *json, const char *field) {
    if (!json || !field) return 0;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", field);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t')) p++;
    if (*p >= '0' && *p <= '9' || *p == '-' || *p == '.') return atof(p);
    return 0;
}

/* ── Build timestamp index from timeline ── */
static int build_ts_index(TimestampIndex *idx, int days_back) {
    idx->ts = malloc(MAX_TIMESTAMPS * sizeof(long long));
    idx->capacity = MAX_TIMESTAMPS;
    idx->count = 0;

    char sql[256];
    if (days_back > 0) {
        snprintf(sql, sizeof(sql),
            "SELECT DISTINCT ts FROM timeline WHERE ts > %lld ORDER BY ts LIMIT %d",
            (long long)(time(NULL) - (long long)days_back * 86400), MAX_TIMESTAMPS);
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT DISTINCT ts FROM timeline ORDER BY ts LIMIT %d", MAX_TIMESTAMPS);
    }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        idx->ts[idx->count++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return 0;
}

/* ── Load source data ── */
static int load_source(DataSource *src, TimestampIndex *idx) {
    src->count = idx->count;
    src->latest = src->fallback;
    src->buffer = calloc(idx->count, sizeof(double));

    /* Fetch data for this source */
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT ts, data FROM timeline WHERE source='%s' AND ts >= %lld AND ts <= %lld ORDER BY ts",
        src->db_source, (long long)idx->ts[0], (long long)idx->ts[idx->count - 1]);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    long long prev_ts = 0;
    double prev_val = src->fallback;
    int idx_pos = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        long long row_ts = sqlite3_column_int64(stmt, 0);
        const char *data = (const char *)sqlite3_column_text(stmt, 1);
        double val = data ? extract_json_val(data, src->json_field) : src->fallback;
        if (val == 0 && data) val = extract_json_val(data, "close");
        if (val == 0) val = src->fallback;

        while (idx_pos < idx->count && idx->ts[idx_pos] <= row_ts) {
            if (idx->ts[idx_pos] >= prev_ts) {
                src->buffer[idx_pos] = val;
            }
            idx_pos++;
        }
        prev_ts = row_ts;
        prev_val = val;
    }
    sqlite3_finalize(stmt);

    while (idx_pos < idx->count) {
        src->buffer[idx_pos] = prev_val;
        idx_pos++;
    }

    src->latest = prev_val;
    return 0;
}

/* ── Write training CSV ── */
static int write_csv(const char *path, TimestampIndex *idx, DataSource *sources, int n_sources, int align_1min) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Can't write %s\n", path);
        return -1;
    }

    /* Header */
    fprintf(f, "ts");
    for (int s = 0; s < n_sources; s++) {
        fprintf(f, ",%s", sources[s].name);
    }
    /* Target: next-period direction (1 if close goes up, 0 otherwise) */
    fprintf(f, ",target_up\n");

    /* Data rows */
    int btc_idx = -1;
    for (int s = 0; s < n_sources; s++) {
        if (strstr(sources[s].name, "bitstamp") || strstr(sources[s].name, "kraken_btc")) {
            btc_idx = s;
            break;
        }
    }

    for (int i = 0; i < idx->count; i++) {
        /* Align to 1-min if requested */
        if (align_1min && idx->ts[i] % 60 != 0) continue;

        fprintf(f, "%lld", idx->ts[i]);
        for (int s = 0; s < n_sources; s++) {
            fprintf(f, ",%.6f", sources[s].buffer[i]);
        }

        /* Target: did BTC close go up next period? */
        if (btc_idx >= 0 && i < idx->count - 1) {
            double curr = sources[btc_idx].buffer[i];
            double next = sources[btc_idx].buffer[i + 1];
            fprintf(f, ",%d\n", next > curr ? 1 : 0);
        } else {
            fprintf(f, ",0\n");
        }
    }

    fclose(f);
    return 0;
}

/* ── Print summary ── */
static void print_summary(TimestampIndex *idx, DataSource *sources, int n_sources) {
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════╗\n");
    printf("  ║   TRAINING PILE — Aggregated Data Tensor            ║\n");
    printf("  ║   %d timestamps · %d sources\n", idx->count, n_sources);
    printf("  ╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  NOT FINANCIAL ADVICE. Algorithmic analysis only.\n");
    printf("\n");

    printf("  %-35s %12s %14s\n", "Source", "Points", "Latest");
    printf("  %s\n", "──────────────────────────────────────────────────────");
    time_t now = time(NULL);
    for (int s = 0; s < n_sources; s++) {
        /* Count actual non-fallback values */
        int has_data = 0;
        for (int i = 0; i < idx->count; i++) {
            if (fabs(sources[s].buffer[i] - sources[s].fallback) > 0.0001) {
                has_data = idx->count - i;
                break;
            }
        }
        printf("  %-35s %12d %14.2f\n", sources[s].name, has_data, sources[s].latest);
    }
    printf("\n");
}

/* ── Main ── */
int main(int argc, char *argv[]) {
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════╗\n");
    printf("  ║   TRAINING PILE ASSEMBLER — Past + Present Data     ║\n");
    printf("  ║   Aggregation IS the magic                          ║\n");
    printf("  ╚══════════════════════════════════════════════════════╝\n");
    printf("\n");

    int days = 30;
    const char *output = "training_pile.csv";

    if (argc >= 2) days = atoi(argv[1]);
    if (argc >= 3) output = argv[2];
    if (days < 0) days = 0;

    if (open_db() != SQLITE_OK) {
        fprintf(stderr, "Can't open %s\n", DB_PATH);
        return 1;
    }

    /* Build timestamp index */
    TimestampIndex idx = {0};
    if (build_ts_index(&idx, days) < 0) {
        close_db();
        return 1;
    }

    printf("  Timestamps: %d (last %d days)\n", idx.count, days > 0 ? days : idx.count);

    /* Define sources to aggregate */
    DataSource sources[] = {
        /* Crypto 1-min */
        { .name = "bitstamp_btc", .db_source = "bitstamp_1min", .json_field = "close", .fallback = 0 },
        { .name = "kraken_btc", .db_source = "kraken_btc", .json_field = "close", .fallback = 0 },
        { .name = "kraken_eth", .db_source = "kraken_eth", .json_field = "close", .fallback = 0 },
        { .name = "kraken_sol", .db_source = "kraken_sol", .json_field = "close", .fallback = 0 },
        /* Macro (daily → forward-filled) */
        { .name = "sp500", .db_source = "fred_sp500", .json_field = "value", .fallback = 0 },
        { .name = "vix", .db_source = "fred_vix", .json_field = "value", .fallback = 0 },
        { .name = "fedfunds", .db_source = "fred_fedfunds", .json_field = "value", .fallback = 0 },
        { .name = "cpi", .db_source = "fred_cpi", .json_field = "value", .fallback = 0 },
        { .name = "unemployment", .db_source = "fred_unemployment", .json_field = "value", .fallback = 0 },
        { .name = "t10y2y", .db_source = "fred_t10y2y", .json_field = "value", .fallback = 0 },
        /* Stocks (daily) */
        { .name = "gold", .db_source = "stock_gold", .json_field = "close", .fallback = 0 },
        { .name = "crude_oil", .db_source = "stock_crude_oil", .json_field = "close", .fallback = 0 },
        { .name = "usd_index", .db_source = "stock_usd_index", .json_field = "close", .fallback = 0 },
        { .name = "dow", .db_source = "stock_dow", .json_field = "close", .fallback = 0 },
        { .name = "nasdaq", .db_source = "stock_nasdaq", .json_field = "close", .fallback = 0 },
        { .name = "stock_vix", .db_source = "stock_vix", .json_field = "close", .fallback = 0 },
        /* Sentiment */
        { .name = "fear_greed", .db_source = "fear_greed_fear_greed_all", .json_field = "value", .fallback = 50 },
        /* Forex */
        { .name = "eurusd", .db_source = "forex_eurusd", .json_field = "close", .fallback = 0 },
        { .name = "gbpusd", .db_source = "forex_gbpusd", .json_field = "close", .fallback = 0 },
        { .name = "usdjpy", .db_source = "forex_usdjpy", .json_field = "close", .fallback = 0 },
    };
    int n_sources = sizeof(sources) / sizeof(sources[0]);

    /* Load all sources */
    printf("  Loading sources...\n");
    for (int s = 0; s < n_sources; s++) {
        load_source(&sources[s], &idx);
        printf("    %-30s → %.2f\n",
               sources[s].name, sources[s].latest);
    }

    /* Write CSV */
    printf("\n  Writing %s...\n", output);
    write_csv(output, &idx, sources, n_sources, 1);

    /* Print summary */
    print_summary(&idx, sources, n_sources);

    /* Cleanup */
    for (int s = 0; s < n_sources; s++) free(sources[s].buffer);
    free(idx.ts);
    close_db();

    printf("  ✅ Training pile written to %s\n", output);
    printf("  Rows: %d\n", idx.count);
    printf("\n");
    printf("  Next: ./training_pile 365 longer_history.csv  (1 year)\n");
    printf("  Next: ./training_pile 0 full_history.csv      (all time)\n");
    printf("\n");

    return 0;
}
