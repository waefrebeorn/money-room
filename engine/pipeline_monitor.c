/**
 * pipeline_monitor.c — Pipeline Health Monitor
 * Inspects cron log mtimes, data file freshness, engine state.
 * Writes docs/data/pipeline_status.json for the dashboard.
 *
 * Compile:
 *   gcc -O3 -o pipeline_monitor pipeline_monitor.c -ljansson -lm -I.
 * Usage:
 *   ./pipeline_monitor
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <jansson.h>
#include "types.h"

#define PM_LOG_DIR   "/home/wubu2/.hermes/pm_logs"
#define DATA_DIR     "/home/wubu2/money-room/data"
#define DOCS_DATA    "/home/wubu2/money-room/docs/data"
#define ENGINE_BIN   "/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"
#define ENGINE_BIN_PAPER "/home/wubu2/.hermes/pm_logs/c_room/room_state_paper.bin"
#define STATS_JSON   "/home/wubu2/money-room/data/stats.json"
#define OUTPUT       "/home/wubu2/money-room/docs/data/pipeline_status.json"

// ── Pipeline components to monitor ──
typedef struct {
    const char *id;          // Unique ID
    const char *name;        // Display name
    const char *log_path;    // Log file to check mtime
    const char *category;    // "collector", "engine", "data"
    int max_age_seconds;     // Max acceptable age in seconds
    const char *schedule;    // Human-readable schedule
} PipelineComponent;

static const PipelineComponent PIPELINE[] = {
    // ── Engine ──
    {"engine", "Room Engine", ENGINE_BIN, "engine", 120, "Every minute"},
    {"engine_paper", "Paper Engine", ENGINE_BIN_PAPER, "engine", 86400, "On demand"},

    // ── Core collectors ──
    {"cycle_rooms", "Room Cycle", "/home/wubu2/.hermes/pm_logs/c_room/room_log.csv", "collector", 120, "Every minute"},
    {"collector_runner", "Collector Runner", "/home/wubu2/.hermes/pm_logs/collector_runner.log", "collector", 300, "Every 1-15 min"},
    {"monkey_runner", "Monkey Runner", "/home/wubu2/.hermes/pm_logs/monkey_runner.log", "collector", 300, "Every minute"},
    {"money_c", "Money Loop", "/home/wubu2/.hermes/pm_logs/money_c.log", "collector", 300, "Every 5 min"},
    {"room_watchdog", "Room Watchdog", "/home/wubu2/.hermes/pm_logs/room_watchdog.log", "collector", 300, "Every 5 min"},
    {"ws_feed", "WS Feed Bridge", "/home/wubu2/.hermes/pm_logs/ws_feed_watchdog.log", "collector", 120, "Every minute"},
    {"health_monitor", "Health Monitor", "/home/wubu2/.hermes/pm_logs/health_monitor.log", "collector", 1200, "Every 15 min"},
    {"eco_runner", "Economic Runner", "/home/wubu2/.hermes/pm_logs/eco_runner.log", "collector", 600, "Every 5 min"},
    {"teacher_watchdog", "Teacher Watchdog", "/home/wubu2/.hermes/pm_logs/teacher_watchdog.log", "collector", 600, "Every 5 min"},

    // ── Data sources ──
    {"btc_csv", "BTC 1-min CSV", "/home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv", "data", 86400 * 7, "Weekly refresh"},
    {"weather", "Weather Data", "/home/wubu2/money-room/data/multi_market/weather_data.json", "data", 172800, "Daily at 6:00"},
    {"sports", "Sports Data", "/home/wubu2/money-room/data/multi_market/sports_data.json", "data", 172800, "Daily at 6:30"},

    // ── Scheduled batch jobs ──
    {"weather_collector", "Weather Collector", "/home/wubu2/.hermes/pm_logs/weather_collector.log", "collector", 90000, "Daily at 6:00"},
    {"sports_collector", "Sports Collector", "/home/wubu2/.hermes/pm_logs/sports_collector.log", "collector", 90000, "Daily at 6:30"},
    {"trainer", "Multi-Market Trainer", "/home/wubu2/.hermes/pm_logs/multi_market_trainer.log", "collector", 90000, "Daily at 7:00"},
    {"eco_backup", "Eco Backup", "/home/wubu2/.hermes/pm_logs/eco_backup.log", "collector", 43200, "Every 6 hours"},

    // ── State files ──
    {"state_mmap", "State (MMAP)", ENGINE_BIN, "data", 120, "Live engine"},
    {"genomes_dir", "Trained Genomes", "/home/wubu2/money-room/data/multi_market", "data", 172800, "Daily at 7:00"},
};

#define N_PIPELINE (sizeof(PIPELINE) / sizeof(PIPELINE[0]))

// Check file age and size
static int check_file(const char *path, time_t now, time_t *mtime_out, off_t *size_out) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (mtime_out) *mtime_out = st.st_mtime;
    if (size_out) *size_out = st.st_size;
    return 0;
}

// Read stats.json for engine metrics
static int read_stats_json(json_t *stats) {
    json_error_t err;
    json_t *root = json_load_file(STATS_JSON, 0, &err);
    if (!root) return -1;
    
    // Copy relevant fields
    const char *keys[] = {"cycle", "capital", "win_rate", "trades_total", "agents",
                          "drawdown", "sharpe", "timestamp", "price", NULL};
    for (int i = 0; keys[i]; i++) {
        json_t *val = json_object_get(root, keys[i]);
        if (val) json_object_set(stats, keys[i], val);
    }
    
    json_decref(root);
    return 0;
}

// Count .bin genome files
static int count_genomes(void) {
    const char *dir = "/home/wubu2/money-room/data/multi_market";
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        size_t nlen = strlen(e->d_name);
        if (nlen > 4 && strcmp(e->d_name + nlen - 4, ".bin") == 0) count++;
    }
    closedir(d);
    return count;
}

int main(void) {
    time_t now = time(NULL);
    
    json_t *root = json_object();
    json_object_set_new(root, "generated_at", json_integer(now));
    json_object_set_new(root, "generated_at_iso", json_string(ctime(&now)));
    
    // ── Engine stats ──
    json_t *engine_stats = json_object();
    read_stats_json(engine_stats);
    json_object_set_new(root, "engine", engine_stats);
    
    // ── Pipeline components ──
    json_t *components = json_array();
    int total_healthy = 0, total_unhealthy = 0;
    
    for (int i = 0; i < (int)N_PIPELINE; i++) {
        json_t *comp = json_object();
        json_object_set_new(comp, "id", json_string(PIPELINE[i].id));
        json_object_set_new(comp, "name", json_string(PIPELINE[i].name));
        json_object_set_new(comp, "category", json_string(PIPELINE[i].category));
        json_object_set_new(comp, "schedule", json_string(PIPELINE[i].schedule));
        
        time_t mtime = 0;
        off_t size = 0;
        int exists = check_file(PIPELINE[i].log_path, now, &mtime, &size);
        
        if (exists != 0) {
            json_object_set_new(comp, "status", json_string("missing"));
            json_object_set_new(comp, "healthy", json_false());
            json_object_set_new(comp, "age_seconds", json_integer(-1));
            total_unhealthy++;
        } else {
            int age = (int)(now - mtime);
            int healthy = (age <= PIPELINE[i].max_age_seconds);
            
            json_object_set_new(comp, "status", json_string(healthy ? "ok" : "stale"));
            json_object_set_new(comp, "healthy", json_boolean(healthy));
            json_object_set_new(comp, "age_seconds", json_integer(age));
            json_object_set_new(comp, "last_run", json_integer(mtime));
            json_object_set_new(comp, "file_size", json_integer((long long)size));
            
            if (healthy) total_healthy++;
            else total_unhealthy++;
        }
        
        json_array_append_new(components, comp);
    }
    
    json_object_set_new(root, "components", components);
    
    // ── Summary ──
    json_t *summary = json_object();
    json_object_set_new(summary, "total", json_integer(N_PIPELINE));
    json_object_set_new(summary, "healthy", json_integer(total_healthy));
    json_object_set_new(summary, "unhealthy", json_integer(total_unhealthy));
    json_object_set_new(summary, "health_pct", json_real(
        N_PIPELINE > 0 ? (double)total_healthy / N_PIPELINE * 100.0 : 0.0));
    
    // Genome count
    json_object_set_new(summary, "genome_count", json_integer(count_genomes()));
    
    json_object_set_new(root, "summary", summary);
    
    // ── Write output ──
    mkdir(DOCS_DATA, 0755);
    if (json_dump_file(root, OUTPUT, JSON_INDENT(2)) != 0) {
        fprintf(stderr, "[MONITOR] Failed to write %s\n", OUTPUT);
        json_decref(root);
        return 1;
    }
    
    json_decref(root);
    printf("[MONITOR] Pipeline status: %d/%d healthy → %s\n",
           total_healthy, (int)N_PIPELINE, OUTPUT);
    return 0;
}
