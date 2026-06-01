/**
 * room_health_monitor.c — E6: Money room health monitor (replaces 141-line Python)
 * Checks heartbeats, processes, snapshots, eco state, Q-controller, disk.
 * Build: gcc -O2 room_health_monitor.c -o room_health_monitor -ljansson -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define HB_DIR "/home/wubu2/.hermes/infra/heartbeats"
#define PM_DIR "/home/wubu2/.hermes/pm_logs"
#define STALE_THRESHOLD 7200

static time_t now;
static int issues = 0, warnings = 0;

static long hb_age(const char *name) {
    char path[512]; snprintf(path, sizeof(path), "%s/%s.heartbeat", HB_DIR, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long ts = 0; fscanf(f, "%ld", &ts); fclose(f);
    return ts > 0 ? (long)(now - ts) : -1;
}

static void check_hb(const char *name, long max_age, const char *label) {
    long age = hb_age(name);
    if (age < 0) { printf("  X %s: NO HEARTBEAT\n", label); issues++; }
    else if (age > max_age) { printf("  X %s: STALE (%ldm ago, max %ldm)\n", label, age/60, max_age/60); issues++; }
    else if (age > max_age/2) { printf("  W %s: aging (%ldm)\n", label, age/60); warnings++; }
}

int main(void) {
    now = time(NULL);
    struct tm *tm = localtime(&now);
    char tb[64]; strftime(tb, sizeof(tb), "%b %d %H:%M", tm);
    printf("Room Health Report - %s\n", tb);
    printf("==================================================\n");

    // 1. Heartbeat checks
    check_hb("room-feed", 300, "Room Feed Bridge");
    check_hb("kraken-collector", 1800, "Kraken Collector");
    check_hb("data-cleaner", 7200, "Data Cleaner");
    check_hb("live-news", 7200, "Live News");
    check_hb("pm-money-loop", 7200, "PM Money Loop");
    check_hb("money-loop-daily", 86400, "Daily Money Loop");

    // 2. Process check via pgrep
    int ec = system("pgrep -f room_engine > /dev/null 2>&1");
    int ecount = (ec == 0) ? 1 : 0;
    // Count with pgrep -c
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pgrep -c -f room_engine 2>/dev/null");
    FILE *pf = popen(cmd, "r");
    if (pf) { int c; if (fscanf(pf, "%d", &c) == 1) ecount = c; pclose(pf); }
    if (ecount == 0) { printf("  X Room Engine: NO PROCESSES\n"); issues++; }
    else printf("  room_engine processes: %d\n", ecount);

    // 3. Room snapshot freshness
    struct stat st;
    char snap[512]; snprintf(snap, sizeof(snap), "%s/c_room/room_snapshot.json", PM_DIR);
    if (stat(snap, &st) == 0) {
        long snap_age = now - st.st_mtime;
        if (snap_age > 3600) { printf("  X Room snapshot: %ldm old\n", snap_age/60); issues++; }
        else printf("  Room snapshot: %ldm old\n", snap_age/60);
    } else { printf("  X Room snapshot: MISSING\n"); issues++; }

    // 4. Eco state
    char eco[512]; snprintf(eco, sizeof(eco), "%s/eco/minute_log.jsonl", PM_DIR);
    if (stat(eco, &st) == 0) {
        long eco_age = now - st.st_mtime;
        if (eco_age > 7200) { printf("  X Eco minute_log: %ldm stale\n", eco_age/60); issues++; }
        else printf("  Eco minute_log: %ldm old\n", eco_age/60);
    } else { printf("  X Eco: no minute_log files\n"); issues++; }

    // 5. Q-controller state
    char qp[512]; snprintf(qp, sizeof(qp), "%s/c_room/q_controller_state.json", PM_DIR);
    if (stat(qp, &st) == 0) {
        long q_age = now - st.st_mtime;
        printf("  Q-controller state: %ldm old\n", q_age/60);
    } else { printf("  W Q-controller: MISSING (ok if not started)\n"); warnings++; }

    // 6. Room configs
    for (int i = 0; i < 4; i++) {
        const char *rooms[] = {"btc_main", "macro", "momentum", "polymarket"};
        char cfg[512]; snprintf(cfg, sizeof(cfg), "%s/rooms/%s/room_config.json", PM_DIR, rooms[i]);
        char eng[512]; snprintf(eng, sizeof(eng), "%s/rooms/%s/room_engine", PM_DIR, rooms[i]);
        if (stat(cfg, &st) != 0) { printf("  W Room %s: no config\n", rooms[i]); warnings++; }
        if (stat(eng, &st) != 0) { printf("  W Room %s: no engine binary\n", rooms[i]); warnings++; }
    }

    // 7. Disk usage
    FILE *df = popen("df -h /home 2>/dev/null | tail -1", "r");
    if (df) {
        char dev[64], sz[16], used[16], avail[16], pct_str[8];
        if (fscanf(df, "%s %s %s %s %s", dev, sz, used, avail, pct_str) == 5) {
            int pct = atoi(pct_str);
            if (pct > 90) { printf("  X DISK: %s%% full\n", pct_str); issues++; }
            else if (pct > 75) { printf("  W DISK: %s%% full\n", pct_str); warnings++; }
            else printf("  Disk: %s%% used\n", pct_str);
        }
        pclose(df);
    }

    printf("\nResults: %d issues, %d warnings\n", issues, warnings);
    if (issues) printf("\nISSUES REQUIRING ATTENTION\n");
    return issues > 0 ? 1 : 0;
}
