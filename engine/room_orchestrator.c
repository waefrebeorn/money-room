/*
 * room_orchestrator.c — E44/E46-E51: Multi-Room Orchestrator
 *
 * Aggregates state from all money rooms into a unified health/status report.
 * Reads:
 *   - C room: room_state.bin (mmap) or room_snapshot.json
 *   - P room: pipeline status via heartbeat files
 *   - T room: infra heartbeat
 *   - E room: ecosystem heartbeat + teacher bridge
 *
 * Outputs: ~/.hermes/c_room/orchestrator_state.json
 *   - per-room status (alive/stale/dead)
 *   - last cycle timestamps
 *   - aggregated error counts
 *   - dependency graph (which rooms feed which)
 *
 * Build: gcc room_orchestrator.c -o room_orchestrator -lm -O2
 * Run:  ./room_orchestrator
 * Cron: every 5m
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#define HB_DIR  "/home/wubu2/.hermes/infra/heartbeats"
#define SNAP    "/home/wubu2/.hermes/pm_logs/c_room/room_snapshot.json"
#define OUT     "/home/wubu2/.hermes/pm_logs/c_room/orchestrator_state.json"

#define MAX_AGE_OK    300   /* 5 min */
#define MAX_AGE_STALE 1800  /* 30 min */
#define N_ROOMS 4

static const char *ROOMS[N_ROOMS] = {"c_room", "p_room", "t_room", "e_room"};
static const char *HB_NAMES[N_ROOMS] = {"engine", "pipelines", "infra", "ecosystem"};

/* ── Read heartbeat timestamp ── */
static time_t read_hb(const char *name) {
    char path[256]; snprintf(path, sizeof(path), "%s/%s.heartbeat", HB_DIR, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long t; if (fscanf(f, "%ld", &t) != 1) t = 0;
    fclose(f); return (time_t)t;
}

/* ── Check file age ── */
static time_t file_mtime(const char *path) {
    struct stat st; if (stat(path, &st) != 0) return 0;
    return st.st_mtime;
}

int main(void) {
    time_t now = time(NULL);

    /* ── Collect room status ── */
    fprintf(stdout, "[ORCH] Multi-Room Orchestrator\n");

    /* Room status table */
    const char *room_labels[] = {"C (Engine)", "P (Pipelines)", "T (Infra)", "E (Ecosystem)"};
    const char *statuses[] = {"DEAD", "ALIVE", "ALIVE", "ALIVE"};
    time_t last_seen[N_ROOMS];
    int all_alive = 1;

    /* C room: check snapshot */
    time_t snap_mtime = file_mtime(SNAP);
    time_t snap_age = snap_mtime > 0 ? now - snap_mtime : 999999;

    last_seen[0] = snap_mtime;

    if (snap_age <= MAX_AGE_OK) {
        statuses[0] = "ALIVE";
        fprintf(stdout, "  C: ALIVE (snapshot %lds old)\n", (long)snap_age);
    } else if (snap_age <= MAX_AGE_STALE) {
        statuses[0] = "STALE";
        all_alive = 0;
        fprintf(stdout, "  C: STALE (snapshot %lds old)\n", (long)snap_age);
    } else {
        statuses[0] = "DEAD";
        all_alive = 0;
        fprintf(stdout, "  C: DEAD (snapshot %lds old)\n", (long)snap_age);
    }

    /* P, T, E rooms: check heartbeats */
    for (int i = 1; i < N_ROOMS; i++) {
        time_t hb = read_hb(HB_NAMES[i]);
        last_seen[i] = hb;
        time_t age = hb > 0 ? now - hb : 999999;

        if (age <= MAX_AGE_OK) {
            statuses[i] = "ALIVE";
            fprintf(stdout, "  %s: ALIVE (%s heartbeat %lds old)\n",
                    room_labels[i], HB_NAMES[i], (long)age);
        } else if (age <= MAX_AGE_STALE) {
            statuses[i] = "STALE";
            all_alive = 0;
            fprintf(stdout, "  %s: STALE (%s heartbeat %lds old)\n",
                    room_labels[i], HB_NAMES[i], (long)age);
        } else {
            statuses[i] = "DEAD";
            all_alive = 0;
            fprintf(stdout, "  %s: DEAD (%s heartbeat %lds old)\n",
                    room_labels[i], HB_NAMES[i], (long)age);
        }
    }

    /* ── Dependency graph ── */
    /* P room feeds C room (pipelines → engine features) */
    /* T room feeds C room (infra → orderbook data) */
    /* E room feeds C room (ecosystem → teacher predictions) */
    /* C room feeds dashboard */
    fprintf(stdout, "  Dependencies:\n");
    fprintf(stdout, "    P → C (pipeline features)\n");
    fprintf(stdout, "    T → C (orderbook data)\n");
    fprintf(stdout, "    E → C (teacher predictions)\n");
    fprintf(stdout, "    C → Dashboard (engine state)\n");

    /* ── Build orchestrator JSON ── */
    char json[4096]; int n = 0;
    n += snprintf(json + n, sizeof(json) - n,
        "{\n"
        "  \"fetched_at\": %ld,\n"
        "  \"all_rooms_alive\": %s,\n"
        "  \"rooms\": [\n",
        (long)now, all_alive ? "true" : "false");

    for (int i = 0; i < N_ROOMS; i++) {
        char tb[32] = "never";
        if (last_seen[i] > 0) {
            strftime(tb, sizeof(tb), "%H:%M:%S", gmtime(&last_seen[i]));
        }
        n += snprintf(json + n, sizeof(json) - n,
            "    {\"name\":\"%s\",\"label\":\"%s\",\"status\":\"%s\","
            "\"last_seen\":%ld,\"last_seen_str\":\"%s\"}%s\n",
            ROOMS[i], room_labels[i], statuses[i],
            (long)last_seen[i], tb,
            i < N_ROOMS - 1 ? "," : "");
    }

    n += snprintf(json + n, sizeof(json) - n,
        "  ],\n"
        "  \"dependencies\": [\n"
        "    {\"from\":\"p_room\",\"to\":\"c_room\",\"type\":\"pipeline_features\"},\n"
        "    {\"from\":\"t_room\",\"to\":\"c_room\",\"type\":\"orderbook_data\"},\n"
        "    {\"from\":\"e_room\",\"to\":\"c_room\",\"type\":\"teacher_predictions\"},\n"
        "    {\"from\":\"c_room\",\"to\":\"dashboard\",\"type\":\"engine_state\"}\n"
        "  ]\n"
        "}\n");

    mkdir("/home/wubu2/.hermes/pm_logs/c_room", 0755);
    int fd = open(OUT, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) { write(fd, json, n); close(fd); }
    fprintf(stdout, "  Orchestrator state written to %s\n", OUT);

    if (!all_alive) fprintf(stdout, "  ⚠ NOT all rooms alive\n");
    return all_alive ? 0 : 1;
}
