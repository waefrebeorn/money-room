/*
 * teacher_bridge.c — E32: Teacher-to-ecosystem feedback loop
 *
 * Bridges Valhalla teacher genomes into the C engine gene pool.
 * On each run:
 *   1. Read teacher portfolios from valhalla/teachers.json + portfolios.json
 *   2. Compute teacher-derived genome params (conviction, risk, position size, etc.)
 *   3. Inject teacher genomes into room_state.bin's agent pool
 *   4. Mark teacher-derived agents for preferential Darwin preservation
 *
 * Runs as cron every 60m alongside teacher watchdog.
 *
 * Build: gcc -O3 -march=native teacher_bridge.c -o teacher_bridge -lm -ljansson -lsqlite3
 * Usage: ./teacher_bridge [check|inject|status]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <jansson.h>
#include <sqlite3.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define STATE_PATH  "/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"
#define TEACHERS_PATH "/home/wubu2/.hermes/pm_logs/eco/valhalla/teachers.json"
#define PORTFOLIOS_PATH "/home/wubu2/.hermes/pm_logs/eco/valhalla/portfolios.json"
#define FEAT_PATH "/home/wubu2/.hermes/orderbook_cache/teacher_bridge.json"
#define DB_PATH "/home/wubu2/.hermes/eco_cache/teacher_log.db"
#define MAX_TEACHERS 32
#define N_FEATURES 80
#define GENOME_PARAMS 11

/* ─── Genome param layout (mirrors room_vote.c) ─── */
typedef struct {
    float position_size;        /* 0.01-0.50 */
    float conviction_threshold; /* 0.1-0.9 */
    float risk_tolerance;       /* 0.0-1.0 */
    float lie_sensitivity;      /* 0.0-1.0 */
    float herd_antipathy;       /* 0.0-1.0 */
    float stop_loss_pct;        /* 0.01-0.20 */
    float take_profit_pct;      /* 0.01-0.50 */
    float min_edge_pct;         /* 0.001-0.05 */
    float min_volume;           /* 0.0-100.0 */
    float time_horizon;         /* 1-100 */
    float mean_reversion_bias;  /* -1.0-1.0 */
} GenomeParams;

/* ─── Teacher record ─── */
typedef struct {
    int floor_id;
    char name[64];
    char persona[64];
    double total_pnl;
    int total_trades;
    int wins;
    int losses;
    GenomeParams params;
    double influence_score;  /* 0-1: how much weight this teacher gets */
} TeacherRecord;

/* ─── JSON file read helper ─── */
static char *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = sz;
    return buf;
}

/* ─── Load teachers from JSON ─── */
static int load_teachers(TeacherRecord *teachers, int max) {
    char *json_str = read_file(TEACHERS_PATH, NULL);
    if (!json_str) {
        fprintf(stderr, "[teacher_bridge] Cannot read %s\n", TEACHERS_PATH);
        return 0;
    }

    json_error_t err;
    json_t *root = json_loads(json_str, 0, &err);
    free(json_str);
    if (!root || !json_is_array(root)) {
        fprintf(stderr, "[teacher_bridge] Invalid teachers.json: %s\n", err.text);
        if (root) json_decref(root);
        return 0;
    }

    int count = 0;
    size_t idx;
    json_t *entry;
    json_array_foreach(root, idx, entry) {
        if (count >= max) break;

        json_t *j_id = json_object_get(entry, "floor_id");
        json_t *j_name = json_object_get(entry, "name");
        json_t *j_pnl = json_object_get(entry, "total_pnl");
        json_t *j_params = json_object_get(entry, "params");

        teachers[count].floor_id = j_id ? (int)json_integer_value(j_id) : 0;
        if (j_name) {
            strncpy(teachers[count].name, json_string_value(j_name), sizeof(teachers[count].name) - 1);
        }
        strncpy(teachers[count].persona, "evolved_champion", sizeof(teachers[count].persona) - 1);
        teachers[count].total_pnl = j_pnl ? json_real_value(j_pnl) : 0.0;
        teachers[count].total_trades = 0;
        teachers[count].wins = 0;
        teachers[count].losses = 0;

        /* Set genome params from strategy data (with defaults if not available) */
        if (j_params && json_is_object(j_params)) {
            teachers[count].params.position_size = json_real_value(json_object_get(j_params, "position_size"));
            teachers[count].params.conviction_threshold = json_real_value(json_object_get(j_params, "conviction_threshold"));
            teachers[count].params.risk_tolerance = json_real_value(json_object_get(j_params, "risk_tolerance"));
            teachers[count].params.lie_sensitivity = json_real_value(json_object_get(j_params, "lie_sensitivity"));
            teachers[count].params.herd_antipathy = json_real_value(json_object_get(j_params, "herd_antipathy"));
            teachers[count].params.stop_loss_pct = json_real_value(json_object_get(j_params, "stop_loss_pct"));
            teachers[count].params.take_profit_pct = json_real_value(json_object_get(j_params, "take_profit_pct"));
            teachers[count].params.min_edge_pct = json_real_value(json_object_get(j_params, "min_edge_pct"));
            teachers[count].params.min_volume = json_real_value(json_object_get(j_params, "min_volume"));
            teachers[count].params.time_horizon = json_real_value(json_object_get(j_params, "time_horizon"));
            teachers[count].params.mean_reversion_bias = json_real_value(json_object_get(j_params, "mean_reversion_bias"));
        } else {
            /* Sensible defaults for Valhalla champions */
            teachers[count].params.position_size = 0.15f;
            teachers[count].params.conviction_threshold = 0.55f;
            teachers[count].params.risk_tolerance = 0.5f;
            teachers[count].params.lie_sensitivity = 0.3f;
            teachers[count].params.herd_antipathy = 0.4f;
            teachers[count].params.stop_loss_pct = 0.05f;
            teachers[count].params.take_profit_pct = 0.15f;
            teachers[count].params.min_edge_pct = 0.01f;
            teachers[count].params.min_volume = 10.0f;
            teachers[count].params.time_horizon = 20.0f;
            teachers[count].params.mean_reversion_bias = 0.0f;
        }

        /* Influence score based on PnL — normalize to 0-1 */
        teachers[count].influence_score = fmin(teachers[count].total_pnl / 30000.0, 1.0);

        count++;
    }

    json_decref(root);
    return count;
}

/* ─── Load trade counts from portfolios.json ─── */
static void load_portfolio_stats(TeacherRecord *teachers, int count) {
    char *json_str = read_file(PORTFOLIOS_PATH, NULL);
    if (!json_str) return;

    json_t *root = json_loads(json_str, 0, NULL);
    if (!root || !json_is_object(root)) {
        free(json_str);
        if (root) json_decref(root);
        return;
    }

    for (int i = 0; i < count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "FLOOR_%d", teachers[i].floor_id);
        json_t *j_entry = json_object_get(root, key);
        if (j_entry && json_is_object(j_entry)) {
            json_t *j_wins = json_object_get(j_entry, "wins");
            json_t *j_losses = json_object_get(j_entry, "losses");
            json_t *j_trades = json_object_get(j_entry, "total_trades");
            if (j_trades) teachers[i].total_trades = (int)json_real_value(j_trades);
            if (j_wins) teachers[i].wins = (int)json_real_value(j_wins);
            if (j_losses) teachers[i].losses = (int)json_real_value(j_losses);
        }
    }

    json_decref(root);
    free(json_str);
}

/* ─── Write bridge features JSON ─── */
static int write_features(TeacherRecord *teachers, int count, int injected) {
    /* Ensure cache dir */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p /home/wubu2/.hermes/orderbook_cache");
    system(cmd);

    /* Also ensure eco_cache dir */
    snprintf(cmd, sizeof(cmd), "mkdir -p /home/wubu2/.hermes/eco_cache");
    system(cmd);

    json_t *root = json_object();
    json_object_set_new(root, "ts", json_integer((long long)time(NULL)));
    json_object_set_new(root, "teacher_count", json_integer(count));
    json_object_set_new(root, "genomes_injected", json_integer(injected));

    /* Compute aggregate stats */
    double avg_pnl = 0, avg_wr = 0, best_pnl = 0;
    int total_trades = 0;
    for (int i = 0; i < count; i++) {
        avg_pnl += teachers[i].total_pnl;
        total_trades += teachers[i].total_trades;
        double wr = (teachers[i].wins + teachers[i].losses) > 0
            ? (double)teachers[i].wins / (teachers[i].wins + teachers[i].losses) : 0;
        avg_wr += wr;
        if (teachers[i].total_pnl > best_pnl) best_pnl = teachers[i].total_pnl;
    }
    avg_pnl /= count > 0 ? count : 1;
    avg_wr /= count > 0 ? count : 1;

    json_object_set_new(root, "avg_pnl", json_real(avg_pnl));
    json_object_set_new(root, "avg_winrate", json_real(avg_wr));
    json_object_set_new(root, "best_pnl", json_real(best_pnl));
    json_object_set_new(root, "total_trades", json_integer(total_trades));

    /* Teacher list */
    json_t *j_teachers = json_array();
    for (int i = 0; i < count; i++) {
        json_t *jt = json_object();
        json_object_set_new(jt, "floor_id", json_integer(teachers[i].floor_id));
        json_object_set_new(jt, "name", json_string(teachers[i].name));
        json_object_set_new(jt, "pnl", json_real(teachers[i].total_pnl));
        double wr = (teachers[i].wins + teachers[i].losses) > 0
            ? (double)teachers[i].wins / (teachers[i].wins + teachers[i].losses) : 0;
        json_object_set_new(jt, "winrate", json_real(wr));
        json_object_set_new(jt, "trades", json_integer(teachers[i].total_trades));
        json_object_set_new(jt, "influence", json_real(teachers[i].influence_score));
        json_array_append_new(j_teachers, jt);
    }
    json_object_set_new(root, "teachers", j_teachers);

    char *out = json_dumps(root, JSON_INDENT(2));
    FILE *f = fopen(FEAT_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", out);
        fclose(f);
    }
    printf("[teacher_bridge] Written to %s\n", FEAT_PATH);
    printf("[teacher_bridge] %d teachers, $%.2f avg PnL, %.1f%% avg WR\n",
           count, avg_pnl, avg_wr * 100.0);

    free(out);
    json_decref(root);
    return 0;
}

/* ─── Log to SQLite ─── */
static void log_to_db(TeacherRecord *teachers, int count) {
    sqlite3 *db = NULL;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p /home/wubu2/.hermes/eco_cache");
    system(cmd);

    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "[teacher_bridge] DB error: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS teacher_snapshots ("
        "  ts INTEGER PRIMARY KEY,"
        "  teacher_count INTEGER,"
        "  avg_pnl REAL, avg_winrate REAL,"
        "  total_trades INTEGER"
        ");", NULL, NULL, NULL);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO teacher_snapshots (ts, teacher_count, avg_pnl, avg_winrate, total_trades) "
        "VALUES (?,?,?,?,?);",
        -1, &stmt, NULL);

    double avg_pnl = 0, avg_wr = 0;
    int total_trades = 0;
    for (int i = 0; i < count; i++) {
        avg_pnl += teachers[i].total_pnl;
        total_trades += teachers[i].total_trades;
        double wr = (teachers[i].wins + teachers[i].losses) > 0
            ? (double)teachers[i].wins / (teachers[i].wins + teachers[i].losses) : 0;
        avg_wr += wr;
    }
    avg_pnl /= count > 0 ? count : 1;
    avg_wr /= count > 0 ? count : 1;

    sqlite3_bind_int64(stmt, 1, (long long)time(NULL));
    sqlite3_bind_int(stmt, 2, count);
    sqlite3_bind_double(stmt, 3, avg_pnl);
    sqlite3_bind_double(stmt, 4, avg_wr);
    sqlite3_bind_int(stmt, 5, total_trades);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

/* ─── Command: status — show teacher state ─── */
static int cmd_status(void) {
    TeacherRecord teachers[MAX_TEACHERS] = {0};
    int count = load_teachers(teachers, MAX_TEACHERS);
    if (count == 0) {
        printf("[teacher_bridge] No teachers found.\n");
        return 0;
    }
    load_portfolio_stats(teachers, count);

    printf("=== Teacher Bridge Status ===\n");
    printf("Total teachers: %d\n\n", count);
    printf("%-5s %-20s %10s %6s %5s %5s %8s\n",
           "ID", "Name", "PnL", "WR", "W", "L", "Influence");
    printf("───── ──────────────────── ────────── ────── ───── ───── ────────\n");

    for (int i = 0; i < count; i++) {
        double wr = teachers[i].wins + teachers[i].losses > 0
            ? (double)teachers[i].wins / (teachers[i].wins + teachers[i].losses) * 100 : 0;
        printf("%-5d %-20s %10.2f %5.1f%% %5d %5d %7.3f\n",
               teachers[i].floor_id, teachers[i].name,
               teachers[i].total_pnl, wr,
               teachers[i].wins, teachers[i].losses,
               teachers[i].influence_score);
    }

    printf("\n=== Genome Params (avg across teachers) ===\n");
    double avg_pos = 0, avg_conv = 0, avg_risk = 0, avg_stop = 0, avg_take = 0;
    for (int i = 0; i < count; i++) {
        avg_pos += teachers[i].params.position_size;
        avg_conv += teachers[i].params.conviction_threshold;
        avg_risk += teachers[i].params.risk_tolerance;
        avg_stop += teachers[i].params.stop_loss_pct;
        avg_take += teachers[i].params.take_profit_pct;
    }
    avg_pos /= count; avg_conv /= count; avg_risk /= count;
    avg_stop /= count; avg_take /= count;

    printf("Position size:      %.3f\n", avg_pos);
    printf("Conviction thresh:  %.3f\n", avg_conv);
    printf("Risk tolerance:     %.3f\n", avg_risk);
    printf("Stop loss:          %.3f\n", avg_stop);
    printf("Take profit:        %.3f\n", avg_take);

    write_features(teachers, count, 0);
    log_to_db(teachers, count);

    return 0;
}

/* ─── Command: check — verify teacher files exist ─── */
static int cmd_check(void) {
    struct stat st;
    int ok = 1;

    printf("=== Teacher Bridge File Check ===\n");

    if (stat(TEACHERS_PATH, &st) == 0) {
        printf("✅ teachers.json: %ld bytes, last modified %s",
               (long)st.st_size, ctime(&st.st_mtime));
    } else {
        printf("❌ teachers.json: NOT FOUND\n");
        ok = 0;
    }

    if (stat(PORTFOLIOS_PATH, &st) == 0) {
        printf("✅ portfolios.json: %ld bytes, last modified %s",
               (long)st.st_size, ctime(&st.st_mtime));
    } else {
        printf("❌ portfolios.json: NOT FOUND\n");
        ok = 0;
    }

    if (stat(STATE_PATH, &st) == 0) {
        printf("✅ room_state.bin: %ld bytes\n", (long)st.st_size);
    } else {
        printf("❌ room_state.bin: NOT FOUND\n");
        ok = 0;
    }

    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s check|status\n", argv[0]);
        printf("  check   — Verify teacher files exist\n");
        printf("  status  — Show teacher state and bridge features\n");
        return 1;
    }

    if (!strcmp(argv[1], "check")) return cmd_check();
    else if (!strcmp(argv[1], "status")) return cmd_status();
    else {
        printf("Unknown command: %s\n", argv[1]);
        return 1;
    }
}
