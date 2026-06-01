/**
 * cycle_all_rooms.c — Cycle ALL rooms in C (replaces shell wrapper)
 *
 * Phase 1: Per-room feed generation (differentiated feeds by domain)
 * Phase 2: c_room multi-market engine (main)
 * Phase 3: All 15 room engines with ROOM_DIR
 *
 * Compile: gcc -O2 -o cycle_all_rooms cycle_all_rooms.c
 * Usage:   ./cycle_all_rooms
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

#define C_ENG    "/home/wubu2/.hermes/pm_logs/c_room/room_engine"
#define ROOMS_DIR "/home/wubu2/.hermes/pm_logs/rooms"
#define FEED_GEN "/home/wubu2/.hermes/scripts/room_feed_gen"

static const char *ROOMS[] = {
    "consensus", "crypto_prices", "economic", "elections", "kalshi",
    "macro", "manifold", "momentum", "options", "polymarket",
    "predictit", "science_tech", "sports", "stocks", "weather", "btc_main",
    NULL
};

static int run_cmd(const char *bin, const char *room_dir, int timeout_sec) {
    struct stat st;
    if (stat(bin, &st) != 0 || !(st.st_mode & S_IXUSR)) {
        return -1;  // Not executable
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        // Child: set ROOM_DIR if provided
        if (room_dir) {
            setenv("ROOM_DIR", room_dir, 1);
        }
        // Redirect stdout to /dev/null to suppress noise
        FILE *null = fopen("/dev/null", "w");
        if (null) {
            dup2(fileno(null), STDOUT_FILENO);
            dup2(fileno(null), STDERR_FILENO);
            fclose(null);
        }
        execl(bin, bin, NULL);
        _exit(127);
    }

    // Parent: wait with timeout
    struct timespec ts = {timeout_sec, 0};
    int status;
    pid_t result;
    do {
        result = waitpid(pid, &status, WNOHANG);
        if (result == 0) {
            nanosleep(&ts, NULL);  // Wait timeout then kill
            kill(pid, SIGTERM);
            // Give 1s to die gracefully
            nanosleep(&(struct timespec){1, 0}, NULL);
            result = waitpid(pid, &status, WNOHANG);
            if (result == 0) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            return -2;  // Timed out
        }
    } while (result == 0);

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -3;  // Signaled
}

int main(void) {
    printf("[ROOMS] Cycling all engines...\n");
    int total = 0, ok = 0;

    // ── Phase 1: Generate per-room feeds ──
    struct stat st;
    if (stat(FEED_GEN, &st) == 0 && (st.st_mode & S_IXUSR)) {
        for (int i = 0; ROOMS[i]; i++) {
            char room_dir[256];
            snprintf(room_dir, sizeof(room_dir), "%s/%s", ROOMS_DIR, ROOMS[i]);
            
            // Check room directory exists
            struct stat rd;
            if (stat(room_dir, &rd) != 0 || !S_ISDIR(rd.st_mode)) continue;
            
            int rc = run_cmd(FEED_GEN, room_dir, 5);
            total++;
            if (rc == 0 || rc == -1) ok++;
        }
    }
    printf("[ROOMS] Phase 1: %d/%d room feeds generated\n", ok, total);

    // ── Phase 2: c_room main engine ──
    if (stat(C_ENG, &st) == 0 && (st.st_mode & S_IXUSR)) {
        int rc = run_cmd(C_ENG, NULL, 15);
        printf("[ROOMS] Phase 2: main engine %s\n",
               rc == 0 ? "OK" : (rc == -2 ? "TIMEOUT" : "FAILED"));
    }

    // ── Phase 3: All room engines ──
    total = 0; ok = 0;
    for (int i = 0; ROOMS[i]; i++) {
        char eng_path[256], room_dir[256];
        snprintf(eng_path, sizeof(eng_path), "%s/%s/room_engine", ROOMS_DIR, ROOMS[i]);
        snprintf(room_dir, sizeof(room_dir), "%s/%s", ROOMS_DIR, ROOMS[i]);
        
        struct stat rd;
        if (stat(room_dir, &rd) != 0 || !S_ISDIR(rd.st_mode)) continue;
        
        int rc = run_cmd(eng_path, room_dir, 5);
        total++;
        if (rc == 0 || rc == -1) ok++;
    }
    printf("[ROOMS] Phase 3: %d/%d rooms cycled\n", ok, total);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    printf("[ROOMS] %02d:%02d: All engines cycled\n", tm->tm_hour, tm->tm_min);
    return 0;
}
