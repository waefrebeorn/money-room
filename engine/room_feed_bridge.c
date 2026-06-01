/**
 * room_feed_bridge.c — Resilient feed bridge wrapper (replaces shell script)
 *
 * Runs the C feed bridge binary with retry logic, timeout, and fallback.
 * Phase 1: Try primary bridge binary (3 attempts, 2s backoff)
 * Phase 2: On failure, try WS fallback
 * Phase 3: On success, apply Q-learning reward
 *
 * Compile: gcc -O2 -o room_feed_bridge room_feed_bridge.c
 * Usage:   ./room_feed_bridge
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
#include <sys/stat.h>

#define BRIDGE    "/home/wubu2/.hermes/scripts/feed_bridge"
#define FALLBACK  "/home/wubu2/.hermes/scripts/feed_fallback"
#define Q_BIN     "/home/wubu2/.hermes/pm_logs/c_room/market_controller"
#define LOG       "/home/wubu2/.hermes/pm_logs/feed_bridge_error.log"
#define MAX_ATTEMPTS 3
#define TIMEOUT      30

static void log_msg(const char *msg) {
    FILE *f = fopen(LOG, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", tm);
    fprintf(f, "[%s] %s\n", buf, msg);
    fclose(f);
}

static int run_binary(const char *bin, int timeout_sec) {
    struct stat st;
    if (stat(bin, &st) != 0 || !(st.st_mode & S_IXUSR)) {
        return -100;  // Not found / not executable
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        // Child: run binary, capture stderr to log
        FILE *log_f = fopen(LOG, "a");
        if (log_f) {
            // Redirect stderr to log
            dup2(fileno(log_f), STDERR_FILENO);
            fclose(log_f);
        }
        // Capture stdout for output detection
        execl(bin, bin, NULL);
        _exit(127);
    }

    // Parent: wait with timeout
    int status;
    struct timespec ts = {0, 50000000L};  // 50ms poll interval
    for (int waited = 0; waited < timeout_sec * 20; waited++) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            return -3;  // Signaled
        }
        nanosleep(&ts, NULL);
    }

    // Timeout
    kill(pid, SIGTERM);
    nanosleep(&(struct timespec){1, 0}, NULL);
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    return -2;  // Timed out
}

int main(void) {
    // ── Attempt 1..MAX_ATTEMPTS with 2s backoff ──
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        int rc = run_binary(BRIDGE, TIMEOUT);
        
        if (rc == 0) {
            // Success
            // Apply Q-learning reward
            struct stat qst;
            if (stat(Q_BIN, &qst) == 0 && (qst.st_mode & S_IXUSR)) {
                pid_t qpid = fork();
                if (qpid == 0) {
                    // Suppress output
                    FILE *null = fopen("/dev/null", "w");
                    if (null) {
                        dup2(fileno(null), STDOUT_FILENO);
                        dup2(fileno(null), STDERR_FILENO);
                        fclose(null);
                    }
                    execl(Q_BIN, Q_BIN, "apply_reward", NULL);
                    _exit(127);
                }
                struct timespec qts = {10, 0};
                int qs;
                nanosleep(&qts, NULL);
                waitpid(qpid, &qs, WNOHANG);
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "Bridge OK (attempt %d)", attempt);
            log_msg(msg);
            return 0;
        }

        if (rc == -100) {
            log_msg("CRITICAL: Bridge binary missing");
            break;  // Try fallback
        }

        if (rc == -2) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Attempt %d: timed out after %ds", attempt, TIMEOUT);
            log_msg(msg);
        } else if (rc != 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Attempt %d: exit code %d", attempt, rc);
            log_msg(msg);
        }

        // Backoff before retry
        if (attempt < MAX_ATTEMPTS) {
            struct timespec bo = {2, 0};
            nanosleep(&bo, NULL);
        }
    }

    // ── All attempts failed. Try fallback ──
    log_msg("Running WS fallback");
    int fb_rc = run_binary(FALLBACK, 30);
    if (fb_rc == 0) {
        log_msg("Fallback OK");
        return 0;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Fallback failed (exit=%d)", fb_rc);
    log_msg(msg);
    return 1;
}
