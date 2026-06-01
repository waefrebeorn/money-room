/*
 * politician_portfolio.c — P75: Politician Portfolio Aggregator
 *
 * Extends congress_trades.db with portfolio-level analytics:
 * - Per-politician portfolio tracking (net position, total exposure)
 * - Congressional buy concentration (HHI) — F77
 * - Volume-weighted conviction score — F78
 * - Congressional leaderboard (best/worst performing by conviction)
 *
 * Build: gcc -O3 -march=native politician_portfolio.c -o politician_portfolio -lsqlite3 -ljansson -lm
 * Usage: ./politician_portfolio update   — writes new features to congress_features.json
 *        ./politician_portfolio leaderboard [limit] — prints congressional leaderboard
 *        ./politician_portfolio politician <name>   — prints detailed portfolio for a politician
 *        ./politician_portfolio ticker <ticker>      — prints which politicians trade this ticker
 *        ./politician_portfolio signals             — prints new portfolio features
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sqlite3.h>
#include <jansson.h>
#include <time.h>

#define DB_DIR      "/home/wubu2/.hermes/congress_cache"
#define DB_PATH     DB_DIR "/congress_trades.db"
#define FEAT_PATH   DB_DIR "/congress_features.json"
#define OUT_PATH    DB_DIR "/politician_portfolio_features.json"

/* ─── Query helper: get int ─── */
static int query_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int val = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) val = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return val;
}

/* ─── Query helper: get double ─── */
static double query_double(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    double val = 0.0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) val = sqlite3_column_double(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return val;
}

/* ─── Compute portfolio concentration F77 ─── */
/* Uses Herfindahl-Hirschman Index on congressional buy amounts per ticker.
 * High HHI = everyone piling into same tickers = strong signal. */
static double compute_concentration(sqlite3 *db) {
    /* Get total buy amount across all tickers */
    double total = query_double(db,
        "SELECT SUM(amount_max) FROM trades "
        "WHERE (tx_type LIKE 'purchase%%' OR tx_type LIKE 'buy%%' "
        "   OR tx_type LIKE 'exchange%%' OR tx_type LIKE 'acquisition%%')");
    if (total <= 0) return 0.5;

    /* For each ticker, compute (ticker_total/total)^2 and sum */
    sqlite3_stmt *stmt = NULL;
    double hhi = 0.0;
    sqlite3_prepare_v2(db,
        "SELECT SUM(amount_max) as ticker_total FROM trades "
        "WHERE (tx_type LIKE 'purchase%%' OR tx_type LIKE 'buy%%' "
        "   OR tx_type LIKE 'exchange%%' OR tx_type LIKE 'acquisition%%') "
        "GROUP BY ticker ORDER BY ticker_total DESC",
        -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        double share = sqlite3_column_double(stmt, 0) / total;
        hhi += share * share;
    }
    sqlite3_finalize(stmt);

    /* Normalize HHI: 0=perfectly spread (1/n with n→∞), 1=perfectly concentrated.
     * For n tickers, min HHI = 1/n. Normalize: (HHI - 1/n) / (1 - 1/n) */
    int n_tickers = query_int(db,
        "SELECT COUNT(DISTINCT ticker) FROM trades");
    if (n_tickers <= 1) return fmin(hhi, 1.0);
    double min_hhi = 1.0 / n_tickers;
    double norm = (hhi - min_hhi) / (1.0 - min_hhi);
    return fmax(0.0, fmin(1.0, norm));
}

/* ─── Compute volume-weighted conviction score F78 ─── */
/* Weighted by trade amount: Σ(buy_amount * 1 + sell_amount * 0) / Σ(all_amount)
 * This gives a dollar-weighted buy ratio — a $500K purchase matters more than $1K */
static double compute_conviction(sqlite3 *db) {
    double buy_total = query_double(db,
        "SELECT COALESCE(SUM(amount_max), 0) FROM trades "
        "WHERE (tx_type LIKE 'purchase%%' OR tx_type LIKE 'buy%%' "
        "   OR tx_type LIKE 'exchange%%' OR tx_type LIKE 'acquisition%%')");
    double sell_total = query_double(db,
        "SELECT COALESCE(SUM(amount_max), 0) FROM trades "
        "WHERE (tx_type LIKE 'sale%%' OR tx_type LIKE 'sell%%')");
    double total = buy_total + sell_total;
    if (total <= 0) return 0.5;
    return fmax(0.0, fmin(1.0, buy_total / total));
}

/* ─── Get ticker diversity: how many unique ticks have congressional action ─── */
static int get_ticker_count(sqlite3 *db) {
    return query_int(db, "SELECT COUNT(DISTINCT ticker) FROM trades");
}

/* ─── Get top politician by net buy count ─── */
typedef struct { char name[64]; char party[8]; char chamber[16]; int buys; int sells; double buy_pct; } PolStat;

static int cmp_pol(const void *a, const void *b) {
    double diff = ((PolStat*)b)->buy_pct - ((PolStat*)a)->buy_pct;
    return (diff > 0) - (diff < 0);
}

static void get_leaderboard(sqlite3 *db, int limit, PolStat *stats, int *n) {
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "SELECT politician, party, chamber, "
        "  SUM(CASE WHEN tx_type LIKE 'purchase%%' OR tx_type LIKE 'buy%%' "
        "       OR tx_type LIKE 'exchange%%' OR tx_type LIKE 'acquisition%%' THEN 1 ELSE 0 END) as buys, "
        "  SUM(CASE WHEN tx_type LIKE 'sale%%' OR tx_type LIKE 'sell%%' THEN 1 ELSE 0 END) as sells "
        "FROM trades GROUP BY politician ORDER BY (buys+sells) DESC",
        -1, &stmt, NULL);
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < limit) {
        const char *name = (const char*)sqlite3_column_text(stmt, 0);
        const char *party = (const char*)sqlite3_column_text(stmt, 1);
        const char *chamber = (const char*)sqlite3_column_text(stmt, 2);
        int b = sqlite3_column_int(stmt, 3);
        int s = sqlite3_column_int(stmt, 4);
        snprintf(stats[idx].name, sizeof(stats[idx].name), "%s", name ? name : "Unknown");
        snprintf(stats[idx].party, sizeof(stats[idx].party), "%s", party ? party : "?");
        snprintf(stats[idx].chamber, sizeof(stats[idx].chamber), "%s", chamber ? chamber : "?");
        stats[idx].buys = b;
        stats[idx].sells = s;
        stats[idx].buy_pct = (b + s) > 0 ? (double)b / (b + s) * 100.0 : 50.0;
        idx++;
    }
    sqlite3_finalize(stmt);
    *n = idx;
    qsort(stats, idx, sizeof(PolStat), cmp_pol);
}

/* ─── Update features — extends congress_features.json with portfolio data ─── */
static int cmd_update(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open DB at %s\n", DB_PATH);
        return 1;
    }

    double conc = compute_concentration(db);
    double conv = compute_conviction(db);
    int tickers = get_ticker_count(db);

    /* Read existing congress_features.json if it exists */
    json_t *root = NULL;
    FILE *f = fopen(FEAT_PATH, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        char *buf = malloc(sz + 1);
        if (buf && fread(buf, 1, sz, f) == (size_t)sz) {
            buf[sz] = '\0';
            json_error_t err;
            root = json_loads(buf, 0, &err);
        }
        free(buf);
        fclose(f);
    }

    /* Build politician features JSON */
    json_t *feat = json_object();
    json_object_set_new(feat, "pol_portfolio_conc_norm", json_real(conc));
    json_object_set_new(feat, "pol_conviction_norm", json_real(conv));
    json_object_set_new(feat, "pol_ticker_count", json_integer(tickers));
    json_object_set_new(feat, "fetch_time", json_integer((long long)time(NULL)));

    /* Add politician breakdown */
    PolStat stats[50];
    int n_stats = 0;
    get_leaderboard(db, 20, stats, &n_stats);
    json_t *leaderboard = json_array();
    double max_conv = 0;
    for (int i = 0; i < n_stats; i++) {
        double offset = fabs(stats[i].buy_pct - 50.0);
        if (offset > max_conv) max_conv = offset;
    }
    for (int i = 0; i < n_stats; i++) {
        json_t *p = json_object();
        json_object_set_new(p, "name", json_string(stats[i].name));
        json_object_set_new(p, "party", json_string(stats[i].party));
        json_object_set_new(p, "chamber", json_string(stats[i].chamber));
        json_object_set_new(p, "buys", json_integer(stats[i].buys));
        json_object_set_new(p, "sells", json_integer(stats[i].sells));
        json_object_set_new(p, "total_trades", json_integer(stats[i].buys + stats[i].sells));
        json_object_set_new(p, "buy_pct", json_real(stats[i].buy_pct));
        json_object_set_new(p, "net_buy", json_integer(stats[i].buys - stats[i].sells));
        double conviction = (stats[i].buy_pct - 50.0) * 2.0 / 100.0; /* -1..1 */
        json_object_set_new(p, "conviction_signal", json_real(conviction));
        json_array_append_new(leaderboard, p);
    }
    json_object_set_new(feat, "leaderboard", leaderboard);

    /* Top tickers by total trade amount */
    sqlite3_stmt *stmt = NULL;
    json_t *top_tickers = json_array();
    sqlite3_prepare_v2(db,
        "SELECT ticker, COUNT(*) as cnt, SUM(amount_max) as total_vol, "
        "  SUM(CASE WHEN tx_type LIKE 'purchase%%' OR tx_type LIKE 'buy%%' "
        "       OR tx_type LIKE 'exchange%%' OR tx_type LIKE 'acquisition%%' THEN amount_max ELSE 0 END) as buy_vol, "
        "  SUM(CASE WHEN tx_type LIKE 'sale%%' OR tx_type LIKE 'sell%%' THEN amount_max ELSE 0 END) as sell_vol "
        "FROM trades GROUP BY ticker ORDER BY total_vol DESC LIMIT 15",
        -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json_t *t = json_object();
        json_object_set_new(t, "ticker", json_string((const char*)sqlite3_column_text(stmt, 0)));
        json_object_set_new(t, "count", json_integer(sqlite3_column_int(stmt, 1)));
        json_object_set_new(t, "volume_total", json_real(sqlite3_column_double(stmt, 2)));
        json_object_set_new(t, "volume_buy", json_real(sqlite3_column_double(stmt, 3)));
        json_object_set_new(t, "volume_sell", json_real(sqlite3_column_double(stmt, 4)));
        double buy_vol = sqlite3_column_double(stmt, 3);
        double sell_vol = sqlite3_column_double(stmt, 4);
        double tot_vol = buy_vol + sell_vol;
        json_object_set_new(t, "buy_ratio_vol", json_real(tot_vol > 0 ? buy_vol / tot_vol : 0.5));
        json_array_append_new(top_tickers, t);
    }
    sqlite3_finalize(stmt);
    json_object_set_new(feat, "top_tickers_by_volume", top_tickers);

    /* Write features file */
    char *out = json_dumps(feat, JSON_INDENT(2));
    f = fopen(OUT_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", out);
        fclose(f);
        printf("[P75] Written to %s\n", OUT_PATH);
        printf("[P75] Concentration (F77): %.4f\n", conc);
        printf("[P75] Conviction (F78):   %.4f\n", conv);
        printf("[P75] Tickers traded:     %d\n", tickers);
    } else {
        fprintf(stderr, "Cannot write %s\n", OUT_PATH);
    }
    free(out);
    json_decref(feat);
    sqlite3_close(db);
    return 0;
}

/* ─── Print leaderboard to stdout ─── */
static void cmd_leaderboard(int limit) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { printf("No DB.\n"); return; }
    PolStat stats[50];
    int n = 0;
    get_leaderboard(db, limit < 50 ? limit : 50, stats, &n);
    printf("\n━━━ Congressional Trading Leaderboard (top %d) ━━━\n", n);
    printf("%-4s %-32s %-4s %-10s %5s %5s %6s %6s\n",
           "Rank", "Politician", "Pty", "Chamber", "Buys", "Sells", "Total", "Buy%");
    for (int i = 0; i < n; i++) {
        printf("%-4d %-32s %-4s %-10s %5d %5d %6d %5.1f%%%s\n",
               i + 1, stats[i].name, stats[i].party, stats[i].chamber,
               stats[i].buys, stats[i].sells,
               stats[i].buys + stats[i].sells, stats[i].buy_pct,
               stats[i].buy_pct > 60.0 ? " ↑" :
               stats[i].buy_pct < 40.0 ? " ↓" : "");
    }
    sqlite3_close(db);
}

/* ─── Print politician portfolio ─── */
static void cmd_politician(const char *name) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { printf("No DB.\n"); return; }
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT ticker, tx_type, tx_date, amount_min, amount_max, party, chamber "
        "FROM trades WHERE politician LIKE ? "
        "ORDER BY tx_date DESC LIMIT 30",
        -1, &stmt, NULL);
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "%%%s%%", name);
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
    printf("\n━━━ Trades for: %s ━━━\n", name);
    printf("%-6s %-18s %-12s Amount     Party\n", "Ticker", "Type", "Date");
    int cnt = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%-6s %-18s %-12s $%.0f-%.0f %s (%s)\n",
               sqlite3_column_text(stmt, 0),
               sqlite3_column_text(stmt, 1),
               sqlite3_column_text(stmt, 2),
               sqlite3_column_double(stmt, 3),
               sqlite3_column_double(stmt, 4),
               sqlite3_column_text(stmt, 5),
               sqlite3_column_text(stmt, 6));
        cnt++;
    }
    sqlite3_finalize(stmt);
    if (!cnt) printf("(none found)\n");

    /* Summary stats for this politician */
    int buys = query_int(db, "SELECT COUNT(*) FROM trades WHERE politician LIKE ? AND "
        "(tx_type LIKE 'purchase%%' OR tx_type LIKE 'buy%%' "
        " OR tx_type LIKE 'exchange%%' OR tx_type LIKE 'acquisition%%')");
    sqlite3_stmt *st2;
    sqlite3_prepare_v2(db, "SELECT politician LIKE ? AND tx_type LIKE 'sale%%' OR tx_type LIKE 'sell%%'", -1, &st2, NULL);
    sqlite3_bind_text(st2, 1, pattern, -1, SQLITE_TRANSIENT);
    int sells = query_int(db, "SELECT COUNT(*) FROM trades WHERE politician LIKE ? AND "
        "(tx_type LIKE 'sale%%' OR tx_type LIKE 'sell%%')");
    if (cnt > 0) {
        printf("\nSummary: %d buys / %d sells (%d total)\n", buys, sells, cnt);
    }
    sqlite3_close(db);
}

/* ─── Print which politicians trade a given ticker ─── */
static void cmd_ticker(const char *ticker) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { printf("No DB.\n"); return; }
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT politician, party, tx_type, tx_date, amount_min, amount_max "
        "FROM trades WHERE UPPER(ticker)=UPPER(?) ORDER BY tx_date DESC LIMIT 20",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, ticker, -1, SQLITE_TRANSIENT);
    printf("\n━━━ Politicians trading %s ━━━\n", ticker);
    printf("%-24s %-4s %-18s %-12s Amount\n", "Politician", "Pty", "Type", "Date");
    int cnt = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%-24s %-4s %-18s %-12s $%.0f-%.0f\n",
               sqlite3_column_text(stmt, 0),
               sqlite3_column_text(stmt, 1),
               sqlite3_column_text(stmt, 2),
               sqlite3_column_text(stmt, 3),
               sqlite3_column_double(stmt, 4),
               sqlite3_column_double(stmt, 5));
        cnt++;
    }
    sqlite3_finalize(stmt);
    if (!cnt) printf("(none for %s)\n", ticker);
    sqlite3_close(db);
}

/* ─── Print signals ─── */
static void cmd_signals(void) {
    FILE *f = fopen(OUT_PATH, "r");
    if (!f) { printf("No data. Run 'update' first.\n"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *b = malloc(sz + 1);
    fread(b, 1, sz, f);
    b[sz] = '\0';
    fclose(f);
    printf("%s\n", b);
    free(b);
}

static void print_usage(const char *p) {
    printf("Usage:\n");
    printf("  %s update              — compute portfolio features\n", p);
    printf("  %s leaderboard [N]     — show top N politicians by trades\n", p);
    printf("  %s politician <name>   — show portfolio for a politician\n", p);
    printf("  %s ticker <ticker>     — show which politicians trade it\n", p);
    printf("  %s signals             — print portfolio features JSON\n", p);
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }
    if (!strcmp(argv[1], "update"))
        return cmd_update();
    else if (!strcmp(argv[1], "leaderboard"))
        cmd_leaderboard(argc >= 3 ? atoi(argv[2]) : 10);
    else if (!strcmp(argv[1], "politician") && argc >= 3)
        cmd_politician(argv[2]);
    else if (!strcmp(argv[1], "ticker") && argc >= 3)
        cmd_ticker(argv[2]);
    else if (!strcmp(argv[1], "signals"))
        cmd_signals();
    else
        print_usage(argv[0]);
    return 0;
}
