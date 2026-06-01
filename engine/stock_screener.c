/*
 * stock_screener.c — P68: Stock & Options Screener
 *
 * C engine that JOINs all 12 SQLite databases and computes composite
 * signal scores per ticker. On-demand tool — run from CLI.
 *
 * Build: gcc stock_screener.c -o stock_screener -lsqlite3 -lm -O2
 * Run:   ./stock_screener              # top 10
 *        ./stock_screener 20           # top 20
 *        ./stock_screener 10 SPY       # filtered to SPY
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>

#define HOME_DIR "/home/wubu2"
#define MAX_TICKERS 64
#define MAX_NAME 64
#define MISSING -999.0

typedef struct {
    char ticker[MAX_NAME];
    double signal_score;
    double options_score;      // -1 = no data
    double dark_pool_score;
    double congress_score;
    double insider_score;
    double inst_score;
    double short_score;
    double fundamental_score;
    double gdelt_score;
    double macro_score;
    int    signals_count;
    char   signals_detail[128];
} TickerSignal;

static int cmp_signals(const void *a, const void *b) {
    const TickerSignal *sa = (const TickerSignal*)a;
    const TickerSignal *sb = (const TickerSignal*)b;
    if (sb->signal_score > sa->signal_score) return 1;
    if (sb->signal_score < sa->signal_score) return -1;
    return 0;
}

static double qd(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return MISSING;
    double val = MISSING;
    if (sqlite3_step(stmt) == SQLITE_ROW) val = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return val;
}

static int qi(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int val = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) val = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return val;
}

static sqlite3 *dbo(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fclose(f);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { sqlite3_close(db); return NULL; }
    return db;
}

static double scale(double val, double max_val, double neutral) {
    if (val <= MISSING / 2) return -1;
    double s = fmin(val / max_val * 100.0, 100.0);
    return (s < 0) ? 0 : s;
}

static TickerSignal query_signals(const char *ticker) {
    TickerSignal s = {0};
    strncpy(s.ticker, ticker, MAX_NAME - 1);
    char path[512];
    sqlite3 *db = NULL;
    double v;
    int si = 0;
    char detail[128] = {0};

    s.options_score = -1; s.dark_pool_score = -1; s.congress_score = -1;
    s.insider_score = -1; s.inst_score = -1; s.short_score = -1;
    s.fundamental_score = -1; s.gdelt_score = -1; s.macro_score = -1;

    // 1. Options flow: premium alerts from SPY options chain
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/options_cache/SPY_flows.db");
    db = dbo(path);
    if (db) {
        v = qd(db, "SELECT CAST(COUNT(*) AS REAL) FROM options "
                    "WHERE premium > 500000 AND ts >= strftime('%s', 'now', '-7 days')");
        if (v != MISSING) { s.options_score = fmin(v * 15.0, 100.0); si++; strcat(detail, "O "); }
        sqlite3_close(db);
    }

    // 2. Dark pool ATS share % (weekly aggregate)
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/dark_pool_cache/SPY_darkpool.db");
    db = dbo(path);
    if (db) {
        v = qd(db, "SELECT COALESCE(AVG(ats_share_pct), 0) FROM aggregate "
                   "WHERE week_start_date >= date('now', '-30 days')");
        if (v != MISSING && v >= 0) { s.dark_pool_score = fmin(v * 300.0, 100.0); si++; strcat(detail, "D "); }
        sqlite3_close(db);
    }

    // 3. Congressional trades
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/congress_cache/congress_trades.db");
    db = dbo(path);
    if (db) {
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT CAST(SUM(CASE WHEN transaction_type='buy' THEN 1 "
            "WHEN transaction_type='sell' THEN -1 ELSE 0 END) AS REAL) "
            "/ MAX(CAST(COUNT(*) AS REAL), 1) * 100 FROM trades WHERE symbol='%s'", ticker);
        v = qd(db, sql);
        if (v != MISSING && v > -99 && v < 99) {
            s.congress_score = (v + 100.0) / 2.0;
            si++; strcat(detail, "C ");
        }
        sqlite3_close(db);
    }

    // 4. Insider filings (last 30 days)
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/insider_cache/insider_trades.db");
    db = dbo(path);
    if (db) {
        v = qd(db, "SELECT CAST(COUNT(*) AS REAL) * 3 FROM filings "
                   "WHERE file_date >= date('now', '-30 days')");
        if (v != MISSING) { s.insider_score = fmin(v, 100.0); si++; strcat(detail, "I "); }
        sqlite3_close(db);
    }

    // 5. 13F institutional filings (last 30 days)
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/13f_cache/13f.db");
    db = dbo(path);
    if (db) {
        v = qd(db, "SELECT CAST(COUNT(DISTINCT filer_name) AS REAL) * 3 FROM filings "
                   "WHERE filing_date >= date('now', '-30 days')");
        if (v != MISSING) { s.inst_score = fmin(v, 100.0); si++; strcat(detail, "F "); }
        sqlite3_close(db);
    }

    // 6. Short interest (by ticker)
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/short_cache/short.db");
    db = dbo(path);
    if (db) {
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT short_float_pct FROM short_history "
            "WHERE ticker='%s' ORDER BY date DESC LIMIT 1", ticker);
        v = qd(db, sql);
        if (v != MISSING && v >= 0) { s.short_score = fmin(v * 5.0, 100.0); si++; strcat(detail, "S "); }
        sqlite3_close(db);
    }

    // 7. Fundamentals from stock_cache (quotes.pe_ratio)
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/stock_cache/stock_fundamentals.db");
    db = dbo(path);
    if (db) {
        char sql[512];
        // Try quotes.pe_ratio first
        snprintf(sql, sizeof(sql),
            "SELECT CASE WHEN pe_ratio > 0 AND pe_ratio < 50 THEN "
            "(1.0 - pe_ratio / 50.0) * 100.0 ELSE 50.0 END "
            "FROM quotes WHERE symbol='%s' ORDER BY ts DESC LIMIT 1", ticker);
        v = qd(db, sql);
        if (v == MISSING || v < 0) {
            // Fallback: trailing_pe from fundamentals
            snprintf(sql, sizeof(sql),
                "SELECT CASE WHEN trailing_pe > 0 AND trailing_pe < 50 THEN "
                "(1.0 - trailing_pe / 50.0) * 100.0 ELSE 50.0 END "
                "FROM fundamentals WHERE symbol='%s' ORDER BY ts DESC LIMIT 1", ticker);
            v = qd(db, sql);
        }
        if (v != MISSING && v >= 0) { s.fundamental_score = v; si++; strcat(detail, "P "); }
        sqlite3_close(db);
    }

    // 8. GDELT news sentiment (last 3 days)
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/gdelt_cache/sentiment.db");
    db = dbo(path);
    if (db) {
        v = qd(db, "SELECT COALESCE(AVG((mean_sentiment * 50) + 50), 50) FROM sentiment "
                   "WHERE ts >= datetime('now', '-3 days')");
        if (v != MISSING && v >= 0) { s.gdelt_score = v; si++; strcat(detail, "G "); }
        sqlite3_close(db);
    }

    // 9. Macro event proximity
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/macro_cache/macro_events.db");
    db = dbo(path);
    if (db) {
        v = qd(db, "SELECT MIN(julianday(event_date) - julianday('now')) FROM macro_events "
                   "WHERE event_date >= date('now')");
        if (v != MISSING && v > 0) {
            // Closer events = more uncertainty = lower score
            double proximity = fmin(v / 30.0, 1.0);  // 0 = today, 1 = 30+ days out
            s.macro_score = (1.0 - proximity) * 100.0;
            si++; strcat(detail, "M ");
        }
        sqlite3_close(db);
    }

    // 10. Cross-asset risk-on score (last snapshot)
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/cross_asset_cache/cross_asset.db");
    db = dbo(path);
    if (db) {
        v = qd(db, "SELECT risk_on_score FROM cross_asset_snapshots "
                   "ORDER BY ts DESC LIMIT 1");
        if (v != MISSING && v >= 0) {
            double risk_score = v * 100.0;  // risk_on_score is 0-1
            // Blend into fundamental or use standalone
            if (s.fundamental_score > -1)
                s.fundamental_score = (s.fundamental_score + risk_score) / 2.0;
            else
                s.fundamental_score = risk_score;
            si++;
        }
        sqlite3_close(db);
    }

    s.signals_count = si;
    snprintf(s.signals_detail, sizeof(s.signals_detail), "%s", detail);

    // ── Weighted composite ──
    struct { double score; double w; } wg[] = {
        {s.options_score, 0.20}, {s.dark_pool_score, 0.15},
        {s.short_score, 0.15}, {s.insider_score, 0.10},
        {s.inst_score, 0.10}, {s.congress_score, 0.10},
        {s.fundamental_score, 0.10}, {s.gdelt_score, 0.05},
        {s.macro_score, 0.05}
    };
    double ws = 0, wt = 0;
    for (int i = 0; i < 9; i++) {
        if (wg[i].score >= 0) { ws += wg[i].score * wg[i].w; wt += wg[i].w; }
    }
    s.signal_score = (wt > 0.01) ? (ws / wt) : 50.0;
    return s;
}

static void print_header(void) {
    printf("╔══════════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                      🏦 STOCK & OPTIONS SCREENER                                       ║\n");
    printf("║              Composite signal: 0=pessimal 50=neutral 100=optimal                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║ %-6s │ %-7s │ %-6s │ %-6s │ %-6s │ %-6s │ %-5s │ %-6s │ %-6s ║\n",
           "Ticker", "Score", "Optns", "DkPool", "Shrt", "Insid", "Congr", "Fundm", "GDELT");
    printf("╟───────┼─────────┼────────┼────────┼────────┼────────┼───────┼────────┼────────╢\n");
}

static void print_ticker(const TickerSignal *s) {
    char score_str[16];
    if (s->signal_score >= 60) snprintf(score_str, 16, "🟢%5.0f", s->signal_score);
    else if (s->signal_score >= 40) snprintf(score_str, 16, "🟡%5.0f", s->signal_score);
    else snprintf(score_str, 16, "🔴%5.0f", s->signal_score);

    printf("║ %-6s │ %-7s │ %6.0f │ %6.0f │ %6.0f │ %6.0f │ %5.0f │ %6.0f │ %6.0f │ %s║\n",
           s->ticker, score_str,
           s->options_score >= 0 ? s->options_score : -1,
           s->dark_pool_score >= 0 ? s->dark_pool_score : -1,
           s->short_score >= 0 ? s->short_score : -1,
           s->insider_score >= 0 ? s->insider_score : -1,
           s->congress_score >= 0 ? s->congress_score : -1,
           s->fundamental_score >= 0 ? s->fundamental_score : -1,
           s->gdelt_score >= 0 ? s->gdelt_score : -1,
           s->signals_count > 0 ? "" : " ");
}

int main(int argc, char **argv) {
    int limit = 10;
    const char *filter_ticker = NULL;
    if (argc >= 2) { limit = atoi(argv[1]); if (limit <= 0 || limit > 100) limit = 10; }
    if (argc >= 3) filter_ticker = argv[2];

    const char *tickers[] = {
        "SPY","QQQ","IWM","DIA","GLD","SLV","USO",
        "AAPL","MSFT","GOOGL","AMZN","META","NVDA","TSLA",
        "JPM","BAC","GS","V","KO","PEP","XOM","CVX","UNH",
        "JNJ","PG","WMT","HD","DIS","NFLX","ADBE","CRM",
        "INTC","AMD","AVGO","ORCL","IBM","CSCO","TXN",
        "PYPL","UBER","ABNB","SHOP","PLTR","COIN","MSTR"
    };
    int n = sizeof(tickers)/sizeof(tickers[0]);

    TickerSignal res[64];
    int nr = 0;
    printf("\n[SCREENER] Scanning %d tickers...\n", n);
    for (int i = 0; i < n && nr < 64; i++) {
        if (filter_ticker && strcasecmp(tickers[i], filter_ticker) != 0) continue;
        res[nr++] = query_signals(tickers[i]);
    }

    qsort(res, nr, sizeof(TickerSignal), cmp_signals);
    print_header();
    int show = limit < nr ? limit : nr;
    for (int i = 0; i < show; i++) print_ticker(&res[i]);
    printf("╚══════════════════════════════════════════════════════════════════════════════════════════╝\n");

    int bull=0, neut=0, bear=0;
    for (int i = 0; i < nr; i++) {
        if (res[i].signal_score >= 60) bull++;
        else if (res[i].signal_score >= 40) neut++;
        else bear++;
    }
    double avg_sig = 0;
    for (int i = 0; i < nr; i++) avg_sig += res[i].signals_count;
    avg_sig /= nr;
    printf("\n  📊 🟢 %d  🟡 %d  🔴 %d  (%d tickers, %.1f signals avg)\n",
           bull, neut, bear, nr, avg_sig);
    printf("  📅 %s", ctime(&(time_t){time(NULL)}));
    return 0;
}
