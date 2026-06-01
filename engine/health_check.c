/**
 * health_check.c — System health endpoint (lightweight)
 *
 * Verifies key binaries exist, data files are fresh, engine runs.
 * Exit 0 = healthy, 1 = degraded.
 * Writes docs/data/health.json for dashboard.
 *
 * Compile:
 *   gcc -O2 -o health_check health_check.c -ljansson
 *
 * Usage:
 *   ./health_check          # write JSON, exit code reflects health
 *   ./health_check --check  # exit code only (for cron/monit)
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <jansson.h>

#define ENGINE_DIR   "/home/wubu2/money-room/engine"
#define DATA_DIR     "/home/wubu2/money-room/data"
#define DOCS_DATA    "/home/wubu2/money-room/docs/data"
#define PM_LOG_DIR   "/home/wubu2/.hermes/pm_logs"

typedef struct {
    const char *path;
    const char *name;
    int  critical; /* 1 = must exist, 0 = nice to have */
} BinaryCheck;

typedef struct {
    const char *path;
    const char *name;
    int  max_age_sec; /* 0 = must exist, >0 = max age in seconds */
    int  critical;
} DataCheck;

static const BinaryCheck BINARIES[] = {
    {ENGINE_DIR "/room_engine_v3",      "room_engine_v3",      1},
    {ENGINE_DIR "/room_engine_paper",   "room_engine_paper",   1},
    {ENGINE_DIR "/cycle_all_rooms",     "cycle_all_rooms",     1},
    {ENGINE_DIR "/collector_runner",    "collector_runner",    1},
    {ENGINE_DIR "/pipeline_monitor",    "pipeline_monitor",    1},
    {ENGINE_DIR "/data_quality",        "data_quality",        1},
    {ENGINE_DIR "/multi_market_trainer","multi_market_trainer",1},
    {ENGINE_DIR "/weather_collector",   "weather_collector",   1},
    {ENGINE_DIR "/sports_collector",    "sports_collector",    1},
    {ENGINE_DIR "/polymarket_collector","polymarket_collector",0},
    {ENGINE_DIR "/finnhub_collector",   "finnhub_collector",   0},
    {ENGINE_DIR "/btc_csv_refresher",   "btc_csv_refresher",   0},
    {"/home/wubu2/.hermes/scripts/kraken_collector","kraken_collector",0},
    {ENGINE_DIR "/paper_live_bridge",   "paper_live_bridge",   0},
    {0, 0, 0}
};

/* Data freshness checks — max_age_sec=0 means just check exists */
static const DataCheck DATA_FILES[] = {
    {DOCS_DATA "/pipeline_status.json","pipeline_status", 600, 1},
    {DOCS_DATA "/data_quality.json",   "data_quality",    600, 0},
    {DATA_DIR "/multi_market/market_summary.json", "market_summary", 86400, 0},
    {DATA_DIR "/multi_market/weather_data.json",   "weather_data",  86400, 0},
    {PM_LOG_DIR "/c_room/room_state_paper.bin", "paper_state", 600, 0},
    {0, 0, 0, 0}
};

/* ─── File check helpers ─── */

static int file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static int file_age_sec(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    time_t now = time(0);
    return (int)difftime(now, st.st_mtime);
}

static int proc_running(const char *name) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pgrep -x %s >/dev/null 2>&1", name);
    return (system(cmd) == 0);
}

/* ─── Main ─── */

int main(int argc, char **argv) {
    int check_only = (argc > 1 && strcmp(argv[1], "--check") == 0);
    int all_ok = 1;
    int n_ok = 0, n_warn = 0, n_fail = 0;
    char issues[1024] = {0};
    size_t issues_off = 0;

    json_t *root = json_object();
    json_t *binary_arr = json_array();
    json_t *data_arr = json_array();

    /* Check binaries */
    for (int i = 0; BINARIES[i].path; i++) {
        json_t *e = json_object();
        json_object_set_new(e, "name", json_string(BINARIES[i].name));
        int exists = file_exists(BINARIES[i].path);
        json_object_set_new(e, "exists", json_boolean(exists));

        if (exists) {
            json_object_set_new(e, "status", json_string("ok"));
            n_ok++;
        } else if (BINARIES[i].critical) {
            json_object_set_new(e, "status", json_string("fail"));
            int n = snprintf(issues + issues_off, sizeof(issues) - issues_off,
                             "MISSING BIN: %s\n", BINARIES[i].name);
            if (n > 0) issues_off += n;
            n_fail++;
            all_ok = 0;
        } else {
            json_object_set_new(e, "status", json_string("warn"));
            int n = snprintf(issues + issues_off, sizeof(issues) - issues_off,
                             "MISSING (opt): %s\n", BINARIES[i].name);
            if (n > 0) issues_off += n;
            n_warn++;
        }
        json_array_append_new(binary_arr, e);
    }
    json_object_set_new(root, "binaries", binary_arr);

    /* Check data files */
    for (int i = 0; DATA_FILES[i].path; i++) {
        json_t *e = json_object();
        json_object_set_new(e, "name", json_string(DATA_FILES[i].name));

        int age = file_age_sec(DATA_FILES[i].path);
        json_object_set_new(e, "age_sec", json_integer(age));

        if (age < 0) {
            json_object_set_new(e, "exists", json_false());
            if (DATA_FILES[i].critical) {
                json_object_set_new(e, "status", json_string("fail"));
                int n = snprintf(issues + issues_off, sizeof(issues) - issues_off,
                                 "MISSING DATA: %s\n", DATA_FILES[i].name);
                if (n > 0) issues_off += n;
                n_fail++;
                all_ok = 0;
            } else {
                json_object_set_new(e, "status", json_string("warn"));
                n_warn++;
            }
        } else {
            json_object_set_new(e, "exists", json_true());
            int stale = (DATA_FILES[i].max_age_sec > 0 &&
                         age > DATA_FILES[i].max_age_sec);
            json_object_set_new(e, "stale", json_boolean(stale));
            if (stale) {
                json_object_set_new(e, "status", json_string("warn"));
                int n = snprintf(issues + issues_off, sizeof(issues) - issues_off,
                                 "STALE DATA: %s (%ds > %ds max)\n",
                                 DATA_FILES[i].name, age, DATA_FILES[i].max_age_sec);
                if (n > 0) issues_off += n;
                n_warn++;
            } else {
                json_object_set_new(e, "status", json_string("ok"));
                n_ok++;
            }
        }
        json_array_append_new(data_arr, e);
    }
    json_object_set_new(root, "data_files", data_arr);

    /* Process check */
    int engine_running = proc_running("room_engine");
    json_object_set_new(root, "engine_running", json_boolean(engine_running));
    if (!engine_running) {
        json_object_set_new(root, "engine_status", json_string("warn"));
        int n = snprintf(issues + issues_off, sizeof(issues) - issues_off,
                         "ENGINE NOT RUNNING: room_engine\n");
        if (n > 0) issues_off += n;
        n_warn++;
    } else {
        json_object_set_new(root, "engine_status", json_string("ok"));
        n_ok++;
    }

    /* Summary */
    json_object_set_new(root, "overall_status",
        json_string(all_ok && engine_running ? "healthy" : "degraded"));
    json_object_set_new(root, "checks_ok", json_integer(n_ok));
    json_object_set_new(root, "checks_warn", json_integer(n_warn));
    json_object_set_new(root, "checks_fail", json_integer(n_fail));
    json_object_set_new(root, "timestamp", json_integer((json_int_t)time(0)));

    if (issues_off > 0) {
        issues[issues_off] = '\0';
        json_object_set_new(root, "issues", json_string(issues));
    }

    /* Write output */
    char *out = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS);
    if (!check_only) {
        char outpath[512];
        snprintf(outpath, sizeof(outpath), "%s/health.json", DOCS_DATA);
        FILE *f = fopen(outpath, "w");
        if (f) {
            fprintf(f, "%s\n", out);
            fclose(f);
        }
        printf("%s\n", out);
    }
    free(out);
    json_decref(root);

    return all_ok ? 0 : 1;
}
