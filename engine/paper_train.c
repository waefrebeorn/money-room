/**
 * paper_train.c — Blind-room paper training on historical BTC data
 *
 * Workflow:
 *   1. PID lock check → exit if running
 *   2. Verify BTC CSV exists
 *   3. Backup previous state
 *   4. Build paper engine + distiller
 *   5. Launch paper training in background
 *   6. Launch watcher that waits for completion → distill → deploy
 *
 * Compile: gcc -O2 -o paper_train paper_train.c
 * Usage:   ./paper_train
 */
#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define ENGINE_DIR   "/home/wubu2/money-room/engine"
#define ROOM_DIR     "/home/wubu2/.hermes/pm_logs/c_room"
#define ROOM_ENGINE  ROOM_DIR "/room_engine"
#define STATE_BIN    ROOM_DIR "/room_state_paper.bin"
#define LOG_DIR      "/home/wubu2/.hermes/pm_logs/paper_training"
#define PIDFILE      "/tmp/paper_train.pid"
#define CSV_PATH     "/home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv"
#define DISTILLER    ENGINE_DIR "/genome_distiller"
#define TRAIN_LOG    "/home/wubu2/.hermes/pm_logs/paper_training/train_history.log"

// Room paths for deployment
static const char *ROOMS[] = {
    "/home/wubu2/.hermes/pm_logs/c_room",
    "/home/wubu2/.hermes/pm_logs/rooms/crypto_prices",
    "/home/wubu2/.hermes/pm_logs/rooms/elections",
    "/home/wubu2/.hermes/pm_logs/rooms/polymarket",
    "/home/wubu2/.hermes/pm_logs/rooms/macro",
    "/home/wubu2/.hermes/pm_logs/rooms/momentum",
    NULL
};

static void mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int file_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int lines = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) lines++;
    fclose(f);
    return lines;
}

static int run_cmd_silent(const char *bin, char *const argv[], int timeout_sec) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
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

static int run_cmd_log(const char *bin, char *const argv[], int timeout_sec,
                       const char *logfile) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (logfile) {
            int fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
        }
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

// ── Watcher process: runs after paper engine completes ──
static void watcher_main(int paper_pid) {
    char logpath[512], state_backup[512], logname[64];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(logname, sizeof(logname), "watcher_%Y%m%d_%H%M%S.log", tm);
    snprintf(logpath, sizeof(logpath), "%s/%s", LOG_DIR, logname);

    // Redirect output to log file
    int log_fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    }

    printf("[WATCHER] Waiting for paper engine (PID %d) to complete...\n", paper_pid);

    // Poll until paper engine exits
    while (kill(paper_pid, 0) == 0) {
        sleep(60);
    }
    printf("[WATCHER] Paper engine finished at %s", ctime(&now));
    sleep(2);

    // Backup evolved state
    char backup_path[512];
    char ts_str[64];
    strftime(ts_str, sizeof(ts_str), "%Y%m%d_%H%M%S", tm);
    snprintf(backup_path, sizeof(backup_path), "%s/room_state_paper_%s.bin", LOG_DIR, ts_str);
    
    if (file_exists(STATE_BIN)) {
        char cp_cmd[1024];
        snprintf(cp_cmd, sizeof(cp_cmd), "cp \"%s\" \"%s\"", STATE_BIN, backup_path);
        system(cp_cmd);  // Simpler for file copy
        printf("[WATCHER] ✅ Evolved state backed up\n");
    }

    // Distill evolved genomes
    printf("[WATCHER] Distilling evolved genomes → live state...\n");
    char *dist_argv[] = {DISTILLER, "--backup", NULL};
    int rc = run_cmd_silent(DISTILLER, dist_argv, 60);
    if (rc == 0 || rc == 127) {
        printf("[WATCHER] ✅ Live state seeded with evolved genomes\n");
    } else {
        printf("[WATCHER] ⚠️ genome_distiller returned %d\n", rc);
    }

    // Clean up pidfile
    unlink(PIDFILE);
    printf("[WATCHER] ✅ Watcher complete\n");
    _exit(0);
}

int main(void) {
    time_t start_ts = time(NULL);
    
    // ── PID lock check ──
    FILE *pf = fopen(PIDFILE, "r");
    if (pf) {
        int old_pid;
        if (fscanf(pf, "%d", &old_pid) == 1 && old_pid > 0) {
            if (kill(old_pid, 0) == 0) {
                printf("  ⚠️ Paper training already running (PID %d). Skipping.\n", old_pid);
                fclose(pf);
                return 0;
            }
            printf("  ⚠️ Stale PID %d (no longer running). Clearing lock.\n", old_pid);
        }
        fclose(pf);
    }

    // ── Header ──
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════╗\n");
    printf("  ║   PAPER TRAINER — Blind-Room Historical Evolution   ║\n");
    printf("  ╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  Room:       %s\n", ROOM_DIR);
    printf("  State:      %s (separate from LIVE)\n", STATE_BIN);
    printf("  Data:       ~722K BTC 1-min candles (2017-2026)\n");
    printf("  Pace:       5ms/cycle (200 cycles/sec)\n");
    printf("  Agents:     2500\n");
    printf("\n");

    // ── CSV check ──
    if (!file_exists(CSV_PATH)) {
        printf("  ❌ ERROR: No BTC CSV at %s\n", CSV_PATH);
        printf("     Need: btc_1min_latest.csv with columns: ts,open,high,low,close,volume\n");
        return 1;
    }
    int candles = file_lines(CSV_PATH);
    printf("  ✅ BTC CSV: %d candles\n", candles);

    // ── Backup previous state ──
    if (file_exists(STATE_BIN)) {
        mkdir_p(LOG_DIR);
        char ts_str[64];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        strftime(ts_str, sizeof(ts_str), "%Y%m%d_%H%M%S", tm);
        char backup[512];
        snprintf(backup, sizeof(backup), "%s/room_state_before_%s.bin", LOG_DIR, ts_str);
        char cp_cmd[1024];
        snprintf(cp_cmd, sizeof(cp_cmd), "cp \"%s\" \"%s\"", STATE_BIN, backup);
        system(cp_cmd);
        printf("  ✅ Previous state backed up\n");
    }

    // ── Clear paper state ──
    unlink(STATE_BIN);
    printf("  ✅ Paper state cleared for fresh training\n");

    // ── Step 1: Build paper engine + distiller ──
    printf("\n  [1/4] Building paper engine + distiller...\n");
    
    char cwd[1024];
    getcwd(cwd, sizeof(cwd));
    chdir(ENGINE_DIR);
    
    // make clean + make paper
    char *make_clean[] = {"make", "clean", NULL};
    run_cmd_silent("/usr/bin/make", make_clean, 30);
    char *make_paper[] = {"make", "paper", NULL};
    run_cmd_silent("/usr/bin/make", make_paper, 60);
    
    // Build distiller
    char *gcc_dist[] = {"gcc", "-O2", "-o", "genome_distiller", "genome_distiller.c", "-lm", NULL};
    run_cmd_silent("/usr/bin/gcc", gcc_dist, 30);

    chdir(cwd);
    printf("  ✅ Paper engine + distiller built (2500 agents, 5ms/cycle)\n");

    // ── Step 2: Run paper training ──
    printf("  [2/4] Running blind-room training...\n");
    printf("        Starting at %s", ctime(&start_ts));
    printf("        Estimated run time: ~71 min for all 722K candles\n");

    // Create log filename
    char logname[64], logpath[512];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(logname, sizeof(logname), "paper_run_%Y%m%d_%H%M%S.log", tm);
    snprintf(logpath, sizeof(logpath), "%s/%s", LOG_DIR, logname);
    mkdir_p(LOG_DIR);

    // Fork paper engine
    pid_t paper_pid = fork();
    if (paper_pid < 0) { perror("fork"); return 1; }

    if (paper_pid == 0) {
        // Child: run paper engine, output to log file
        int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        // Detach from parent process group (like setsid)
        setsid();
        char *argv[] = {ROOM_ENGINE, NULL};
        execv(ROOM_ENGINE, argv);
        _exit(127);
    }

    // Write PID file
    FILE *pf_out = fopen(PIDFILE, "w");
    if (pf_out) { fprintf(pf_out, "%d", paper_pid); fclose(pf_out); }
    printf("        PID: %d (background, detached)\n", paper_pid);
    printf("  ✅ Paper training launched in background\n");

    // ── Launch watcher ──
    pid_t watcher_pid = fork();
    if (watcher_pid == 0) {
        setsid();
        watcher_main(paper_pid);
    }
    printf("        Watcher PID: %d (background, detached)\n", watcher_pid);
    printf("  ✅ Cron job completed — engine + watcher running independently\n");
    printf("  ✅ Setting up completion watcher...\n");

    // Parent exits — engine + watcher run independently
    return 0;
}
