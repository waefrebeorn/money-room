/**
 * teacher_watchdog.c — E3: Auto-restart dead teacher daemons.
 * Replaces teacher_watchdog.py (83 lines Python, runs every 5min).
 * E5: Teacher process monitoring — checks 10 teacher PIDs every 5min.
 * If any dead, kills old group and respawns all 10 via system().
 * Writes heartbeat on success. Logs every run for auditing.
 * 
 * Compile: gcc -O3 -o teacher_watchdog teacher_watchdog.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#define PID_FILE   "/home/wubu2/.hermes/run/teachers.pid"
#define TEACHER_SCRIPT "/home/wubu2/.hermes/hermes-agent/scripts/polymarket/pm_teachers.py"
#define HB_PATH    "/home/wubu2/.hermes/infra/heartbeats/teachers.heartbeat"
#define PYTHON     "/home/wubu2/.hermes/hermes-agent/venv/bin/python3"
#define MAX_PIDS   10

static int read_pids(int *pids) {
    FILE *f = fopen(PID_FILE, "r");
    if (!f) return 0;
    
    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    
    int n = 0;
    char *tok = strtok(buf, ",\n");
    while (tok && n < MAX_PIDS) {
        while (*tok == ' ') tok++;
        pids[n++] = atoi(tok);
        tok = strtok(NULL, ",\n");
    }
    return n;
}

static int is_pid_alive(int pid) {
    if (pid <= 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    struct stat st;
    return (stat(path, &st) == 0);
}

static void write_heartbeat(void) {
    // Ensure directory exists
    char dir[256];
    snprintf(dir, sizeof(dir), "/home/wubu2/.hermes/infra/heartbeats");
    mkdir(dir, 0755);
    
    FILE *f = fopen(HB_PATH, "w");
    if (f) {
        fprintf(f, "%ld", (long)time(NULL));
        fclose(f);
    }
}

static void log_msg(const char *msg) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    printf("[watchdog] %02d:%02d:%02d %s\n",
           tm->tm_hour, tm->tm_min, tm->tm_sec, msg);
}

static void kill_old_teachers(int *pids, int n) {
    for (int i = 0; i < n; i++) {
        if (pids[i] > 0 && is_pid_alive(pids[i])) {
            kill(pids[i], SIGTERM);
        }
    }
    sleep(1);
    // Remove PID file
    unlink(PID_FILE);
}

static void spawn_teachers(void) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s %s start > /dev/null 2>&1 &",
             PYTHON, TEACHER_SCRIPT);
    int ret = system(cmd);
    (void)ret;
}

int main(void) {
    int pids[MAX_PIDS] = {0};
    int n = read_pids(pids);
    
    if (n == 0) {
        log_msg("No PID file — teachers not started. Spawning...");
        spawn_teachers();
        return 0;
    }
    
    // Check each PID
    int alive = 0;
    int dead_count = 0;
    int dead_ids[MAX_PIDS];
    
    for (int i = 0; i < n; i++) {
        if (is_pid_alive(pids[i])) {
            alive++;
        } else {
            dead_ids[dead_count++] = pids[i];
        }
    }
    
    if (dead_count > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Dead teachers (%d): respawning...", dead_count);
        log_msg(msg);
        
        // Kill any remaining, respawn
        kill_old_teachers(pids, n);
        spawn_teachers();
        return 0;
    }
    
    if (alive < 10) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Only %d/10 teachers alive — respawning", alive);
        log_msg(msg);
        kill_old_teachers(pids, n);
        spawn_teachers();
        return 0;
    }
    
    // All good
    char msg[128];
    snprintf(msg, sizeof(msg), "All %d/10 teachers alive", alive);
    log_msg(msg);
    write_heartbeat();
    return 0;
}
