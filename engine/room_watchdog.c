/**
 * room_watchdog.c — E6/E7: Room health watchdog + per-room heartbeats
 * 
 * All v3 engines (macro, momentum, polymarket) write to the same 
 * c_room snapshot via room_bridge.c (hardcoded path). 
 * This watchdog checks c_room snapshot freshness and cycles
 * any stale engine. Writes per-room heartbeats on successful cycle.
 * 
 * Runs every 5min via crontab.
 * Compile: gcc -O3 -o room_watchdog room_watchdog.c -lm
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define MAX_AGE_SECS  300  // 5 min stale threshold
#define HB_DIR        "/home/wubu2/.hermes/infra/heartbeats"

#define C_ROOM_SNAP   "/home/wubu2/.hermes/pm_logs/c_room/room_snapshot.json"
#define BTC_ENGINE    "/home/wubu2/.hermes/pm_logs/rooms/btc_main/room_engine"
#define V3_ENGINE     "/home/wubu2/.hermes/pm_logs/rooms/macro/room_engine_v3"

static void log_msg(const char *msg) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    printf("[wd] %02d:%02d:%02d %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, msg);
    fflush(stdout);
}

static void write_heartbeat(const char *name) {
    mkdir(HB_DIR, 0755);
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.heartbeat", HB_DIR, name);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%ld", (long)time(NULL)); fclose(f); }
}

static int snapshot_fresh(void) {
    struct stat st;
    if (stat(C_ROOM_SNAP, &st) != 0) return 0;
    return (difftime(time(NULL), st.st_mtime) < MAX_AGE_SECS);
}

static int engine_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && (st.st_mode & S_IXUSR));
}

static void cycle_engine(const char *name, const char *workdir, const char *engine) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "cd %s && timeout 10 %s > /dev/null 2>&1", workdir, engine);
    
    char msg[128];
    if (system(cmd) == 0) {
        snprintf(msg, sizeof(msg), "%s cycled OK", name);
        write_heartbeat(name);
    } else {
        snprintf(msg, sizeof(msg), "%s cycle FAILED", name);
    }
    log_msg(msg);
}

static void cycle_engines(void) {
    // btc_main: legacy engine (no bridge, just runs)
    if (engine_exists(BTC_ENGINE))
        cycle_engine("btc_main",
                     "/home/wubu2/.hermes/pm_logs/rooms/btc_main",
                     "./room_engine");
    
    // v3 rooms: macro, momentum, polymarket
    static const char *names[] = {"macro", "momentum", "polymarket"};
    static const char *dirs[] = {
        "/home/wubu2/.hermes/pm_logs/rooms/macro",
        "/home/wubu2/.hermes/pm_logs/rooms/momentum",
        "/home/wubu2/.hermes/pm_logs/rooms/polymarket"
    };
    
    if (engine_exists(V3_ENGINE)) {
        for (int i = 0; i < 3; i++) {
            cycle_engine(names[i], dirs[i], "./room_engine_v3");
        }
    }
}

int main(void) {
    if (snapshot_fresh()) {
        log_msg("All rooms healthy (snapshot fresh)");
        // Update all heartbeats on health
        write_heartbeat("btc_main");
        write_heartbeat("macro");
        write_heartbeat("momentum");
        write_heartbeat("polymarket");
        write_heartbeat("rooms");  // aggregate
        return 0;
    }
    
    // Snapshot stale — cycle all engines
    struct stat st;
    int age_secs = -1;
    if (stat(C_ROOM_SNAP, &st) == 0)
        age_secs = (int)difftime(time(NULL), st.st_mtime);
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Snapshot stale (%ds) — cycling all engines", age_secs);
    log_msg(msg);
    
    cycle_engines();
    
    // Check if cycle fixed it
    if (snapshot_fresh()) {
        log_msg("Snapshot fresh after cycle — all healthy");
        write_heartbeat("rooms");  // aggregate heartbeat
    } else {
        log_msg("Still stale after cycle — cron may be failing");
    }
    
    return 0;
}
