/**
 * collector_runner.c — Unified collector pipeline runner (replaces shell)
 *
 * Runs ALL data collector binaries with rate limiting between calls.
 * Schedules: fast (1-5m), normal (15m), slow (1h+), sports.
 *
 * Compile: gcc -O2 -o collector_runner collector_runner.c
 * Usage:   ./collector_runner [fast|normal|slow|sports|all]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define BASE_DIR  "/home/wubu2/.hermes/scripts"
#define LOG_FILE  "/home/wubu2/.hermes/pm_logs/collector_runner.log"
#define LOCK_FILE "/tmp/collector_runner.lock"

typedef struct {
    const char *script;
    const char *name;
    int timeout_sec;
} CollectorTask;

static void log_msg(const char *msg, int also_stdout) {
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", tm);
    fprintf(f, "[%s] %s\n", buf, msg);
    fclose(f);
    if (also_stdout) puts(msg);
}

static int run_script(const CollectorTask *t) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", BASE_DIR, t->script);

    struct stat st;
    if (stat(path, &st) != 0 || !(st.st_mode & S_IXUSR)) {
        char warn[256];
        snprintf(warn, sizeof(warn), "  WARN: %s binary not found", t->name);
        log_msg(warn, 1);
        return 1;
    }

    char run_msg[256];
    snprintf(run_msg, sizeof(run_msg), "  RUN: %s (timeout=%ds)", t->name, t->timeout_sec);
    log_msg(run_msg, 1);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        execl(path, path, NULL);
        _exit(127);
    }

    // Wait with timeout
    int status;
    struct timespec ts = {0, 50000000L};  // 50ms poll
    for (int waited = 0; waited < t->timeout_sec * 20; waited++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            int exit = WIFEXITED(status) ? WEXITSTATUS(status) : -3;
            char result[256];
            if (exit == 0)
                snprintf(result, sizeof(result), "  OK: %s", t->name);
            else
                snprintf(result, sizeof(result), "  EXIT: %s (exit=%d)", t->name, exit);
            log_msg(result, 1);
            // Rate limit
            nanosleep(&(struct timespec){2, 0}, NULL);
            return exit;
        }
        nanosleep(&ts, NULL);
    }

    // Timeout
    kill(pid, SIGTERM);
    nanosleep(&(struct timespec){1, 0}, NULL);
    char timeout_msg[256];
    snprintf(timeout_msg, sizeof(timeout_msg), "  TIMEOUT: %s after %ds", t->name, t->timeout_sec);
    log_msg(timeout_msg, 1);
    nanosleep(&(struct timespec){2, 0}, NULL);
    return 124;  // Same exit code as timeout command
}

static void run_category(const CollectorTask *tasks, int n, const char *label) {
    char header[256];
    snprintf(header, sizeof(header), "=== COLLECTOR RUNNER: %s ===", label);
    log_msg(header, 1);
    for (int i = 0; i < n; i++) {
        run_script(&tasks[i]);
    }
}

// ── Task definitions ──

static CollectorTask FAST_TASKS[] = {
    {"cycle_all_rooms.sh",     "cycle_all_rooms",   60},
    {"outlier_filter.sh",      "outlier_filter",    15},
    {"ws_feed_bridge.sh",      "ws_feed_watchdog",  15},
};
#define N_FAST (sizeof(FAST_TASKS) / sizeof(FAST_TASKS[0]))

static CollectorTask NORMAL_TASKS[] = {
    {"sports_collector.sh",    "sports",            60},
    {"options_feat.sh",        "options_features",  30},
    {"onchain_feat.sh",        "onchain_features",  30},
    {"stablecoin_feat.sh",     "stablecoin_features", 30},
    {"funding_feat.sh",        "funding_features",  30},
    {"open_interest_feat.sh",  "open_interest",     30},
    {"hashrate_feat.sh",       "hashrate_features", 30},
    {"orderbook_fetch.sh",     "orderbook_archive", 30},
    {"liquidation_feat.sh",    "liquidation_features", 30},
    {"ls_ratio_feat.sh",       "ls_ratio",          30},
    {"whale_feat.sh",          "whale_tracking",    30},
    {"etf_flow_feat.sh",       "etf_flow",          30},
    {"gdelt_c.sh",             "gdelt_sentiment",   60},
    {"news_fetch.sh",          "news_rss",          30},
    {"options_flow.sh",        "options_flow_monitor", 30},
    {"earnings_fetch.sh",      "earnings_calendar", 30},
};
#define N_NORMAL (sizeof(NORMAL_TASKS) / sizeof(NORMAL_TASKS[0]))

static CollectorTask SLOW_TASKS[] = {
    {"dark_pool_fetch.sh",     "dark_pool",         60},
    {"congress_fetch.sh",      "congress_trades",   60},
    {"insider_fetch.sh",       "insider_trades",    60},
    {"13f_fetch.sh",           "13f_holdings",      60},
    {"short_fetch.sh",         "short_interest",    60},
    {"tide_fetch.sh",          "market_tide",       60},
    {"etf_fetch.sh",           "etf_holdings",      60},
    {"seasonality_fetch.sh",   "seasonality",       30},
    {"teacher_bridge.sh",      "teacher_bridge",    30},
    {"param_tuner.sh",         "param_tuner",       60},
    {"pm_data_collector.sh",   "polymarket_scan",   120},
    {"auto_test_runner.sh",    "auto_test",         120},
    {"bounty_scanner_wrapper.sh", "bounty_scan",    60},
};
#define N_SLOW (sizeof(SLOW_TASKS) / sizeof(SLOW_TASKS[0]))

static CollectorTask SPORTS_TASKS[] = {
    {"sports_collector.sh",    "sports",            60},
    {"economic_collector.sh",  "economic",          60},
    {"weather_collector.sh",   "weather",           30},
    {"kalshi_collector.sh",    "kalshi",            300},
};
#define N_SPORTS (sizeof(SPORTS_TASKS) / sizeof(SPORTS_TASKS[0]))

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "all";

    // ── File lock ──
    int lock_fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) { perror("lock"); return 1; }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            log_msg("Already running — skipping", 1);
            close(lock_fd);
            return 0;
        }
        perror("flock");
        close(lock_fd);
        return 1;
    }

    if (strcmp(mode, "fast") == 0) {
        run_category(FAST_TASKS, N_FAST, "FAST cycle");
    } else if (strcmp(mode, "normal") == 0) {
        run_category(NORMAL_TASKS, N_NORMAL, "NORMAL cycle (15min)");
    } else if (strcmp(mode, "slow") == 0) {
        run_category(SLOW_TASKS, N_SLOW, "SLOW cycle (1h+)");
    } else if (strcmp(mode, "sports") == 0) {
        run_category(SPORTS_TASKS, N_SPORTS, "SPORTS cycle");
    } else {
        // "all" or default
        run_category(FAST_TASKS, N_FAST, "FAST cycle");
        run_category(NORMAL_TASKS, N_NORMAL, "NORMAL cycle (15min)");
        run_category(SLOW_TASKS, N_SLOW, "SLOW cycle (1h+)");
        run_category(SPORTS_TASKS, N_SPORTS, "SPORTS cycle");
        log_msg("=== COLLECTOR RUNNER: FULL cycle COMPLETE ===", 1);
    }

    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return 0;
}
