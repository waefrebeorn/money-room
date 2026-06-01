/**
 * refresh_all.c — Refresh ALL training data inputs (replaces shell)
 *
 * Runs before multi_market_trainer to ensure fresh data.
 * Phases: BTC CSV → SP500 → VIX → Yahoo stocks → Fear/Greed → Freshness report
 *
 * Compile: gcc -O2 -o refresh_all refresh_all.c -lsqlite3
 * Usage:   ./refresh_all
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sqlite3.h>

#define ENGINE_DIR "/home/wubu2/money-room/engine"
#define LOG_DIR    "/home/wubu2/.hermes/pm_logs"
#define HIST_DIR   LOG_DIR "/historical"
#define TIMELINE_DB LOG_DIR "/timeline.db"

static FILE *g_log = NULL;

static void log_init(void) {
    char path[512];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    snprintf(path, sizeof(path), "%s/refresh_all_%s.log", LOG_DIR, ts);
    g_log = fopen(path, "a");
}

static void log_msg(const char *msg) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", tm);
    printf("%s\n", msg);
    if (g_log) fprintf(g_log, "[%s] %s\n", buf, msg);
}

static int exec_timeout(const char *bin, char *const argv[], int timeout_sec) {
    struct stat st;
    if (stat(bin, &st) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(bin, argv);
        _exit(127);
    }

    int status;
    struct timespec ts = {0, 100000000L};
    for (int w = 0; w < timeout_sec * 10; w++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGTERM);
    nanosleep(&(struct timespec){1, 0}, NULL);
    return -2;
}

static int export_to_csv(const char *sql, const char *csv_path) {
    sqlite3 *db;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) {
        log_msg("  WARN: Cannot open timeline.db");
        return -1;
    }

    FILE *out = fopen(csv_path, "w");
    if (!out) { sqlite3_close(db); return -1; }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        log_msg("  WARN: SQL prepare failed");
        fclose(out);
        sqlite3_close(db);
        return -1;
    }

    int rows = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *date = (const char *)sqlite3_column_text(stmt, 0);
        const char *val = (const char *)sqlite3_column_text(stmt, 1);
        fprintf(out, "%s,%s\n", date ? date : "", val ? val : "");
        rows++;
    }

    sqlite3_finalize(stmt);
    fclose(out);
    sqlite3_close(db);
    return rows;
}

static void export_sp500(void) {
    const char *sql =
        "SELECT strftime('%%Y-%%m-%%d', ts, 'unixepoch'), json_extract(data, '$.value') "
        "FROM timeline WHERE source='fred_sp500' "
        "AND json_extract(data, '$.value') IS NOT NULL ORDER BY ts";
    int rows = export_to_csv(sql, HIST_DIR "/sp500.csv");
    if (rows >= 0)
        log_msg("  Phase 2 done.");
    else
        log_msg("  Phase 2: no data exported.");
}

static void export_vix(void) {
    const char *sql =
        "SELECT strftime('%%Y-%%m-%%d', ts, 'unixepoch'), json_extract(data, '$.value') "
        "FROM timeline WHERE source='fred_vix' "
        "AND json_extract(data, '$.value') IS NOT NULL ORDER BY ts";
    mkdir(HIST_DIR "/raw/stocks", 0755);
    int rows = export_to_csv(sql, HIST_DIR "/raw/stocks/VIX_daily.csv");
    log_msg("  Phase 3 done.");
}

static void export_fear_greed(void) {
    const char *sql =
        "SELECT ts, value, value_classification FROM sentiment_features "
        "WHERE source='fear_greed' ORDER BY ts";
    mkdir(HIST_DIR "/raw/fear_greed", 0755);
    int rows = export_to_csv(sql, HIST_DIR "/raw/fear_greed/fear_greed_all.csv");
    log_msg("  Phase 5 done.");
}

int main(void) {
    log_init();
    log_msg("═══ REFRESH ALL START ═══");

    // Phase 1: BTC CSV refresh
    log_msg("Phase 1: BTC CSV refresh...");
    char *btc_argv[] = {NULL, NULL};
    char btc_path[256];
    snprintf(btc_path, sizeof(btc_path), "%s/btc_csv_refresher", ENGINE_DIR);
    int rc = exec_timeout(btc_path, btc_argv, 30);
    log_msg(rc == 0 ? "Phase 1 done." : "Phase 1: timed out or failed.");

    // Phase 2: SP500 → CSV
    log_msg("Phase 2: SP500 refresh...");
    export_sp500();

    // Phase 3: VIX → CSV
    log_msg("Phase 3: VIX refresh...");
    export_vix();

    // Phase 4: Yahoo stock data
    log_msg("Phase 4: Yahoo stock data...");
    const char *PAIRS[] = {"SPY", "DIA", "QQQ", "GLD", "SLV", "USO", NULL};
    for (int i = 0; PAIRS[i]; i++) {
        char yahoo_path[256];
        snprintf(yahoo_path, sizeof(yahoo_path), "%s/yahoo_collector", ENGINE_DIR);
        struct stat st;
        if (stat(yahoo_path, &st) == 0) {
            char *yahoo_argv[] = {yahoo_path, "--ticker", (char *)PAIRS[i], "--range", "5y", NULL};
            exec_timeout(yahoo_path, yahoo_argv, 30);
        }
    }
    log_msg("Phase 4 done.");

    // Phase 5: Fear & Greed
    log_msg("Phase 5: Fear & Greed...");
    export_fear_greed();

    // Phase 6: Freshness check
    log_msg("Phase 6: Freshness check...");
    struct stat st;
    if (stat(HIST_DIR "/btc_1min_latest.csv", &st) == 0) {
        double age = difftime(time(NULL), st.st_mtime);
        char age_msg[128];
        snprintf(age_msg, sizeof(age_msg), "  btc_1min_latest.csv: age=%.0fs", age);
        log_msg(age_msg);
    }
    log_msg("Phase 6 done.");

    log_msg("═══ REFRESH ALL DONE ═══");
    if (g_log) fclose(g_log);
    return 0;
}
