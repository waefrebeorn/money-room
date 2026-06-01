/*
 * param_tuner.c — E33: Ecosystem hyperparameter tuning
 *
 * Analyzes historical agent performance to find optimal genome parameter ranges.
 * Reads room_state.bin, groups agents by performance, and computes
 * recommended param ranges from the top decile.
 *
 * Build: gcc -O3 -march=native param_tuner.c -o param_tuner -lm -lsqlite3
 * Usage: ./param_tuner [room_state.bin]
 *        ./param_tuner --recommend   — print recommended genome template
 *        ./param_tuner --analyze     — full analysis with param distributions
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define DEFAULT_STATE "/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"
#define DB_PATH "/home/wubu2/.hermes/eco_cache/param_tuner.db"
#define HISTORY_DB "/home/wubu2/.hermes/pm_logs/eco/historical.db"

/* ─── Genome struct (must match types.h) ─── */
#define N_FEATURES 80
typedef struct {
    float position_size;
    float conviction_threshold;
    float risk_tolerance;
    float lie_sensitivity;
    float herd_antipathy;
    float stop_loss_pct;
    float take_profit_pct;
    float min_edge_pct;
    float time_horizon;
    float mean_reversion_bias;
    float feat_weight[N_FEATURES];
    float bias;
    float learning_rate;
} Genome;

/* ─── Agent state (partial, only what we need) ─── */
typedef struct {
    int id;
    Genome genome;
    float capital;
    float total_pnl;
    int trades;
    int wins;
    int losses;
    float win_rate_ema;
    float sharpe;
    float max_drawdown;
} AgentRecord;

/* ─── Param range recommendation ─── */
typedef struct {
    const char *name;
    float min;
    float max;
    float mean;
    float std;
    float top_p10;  /* 10th percentile of top decile */
    float top_p90;  /* 90th percentile of top decile */
} ParamRange;

#define AGENT_FIELD_COUNT 11
static const char *param_names[AGENT_FIELD_COUNT] = {
    "position_size", "conviction_threshold", "risk_tolerance",
    "lie_sensitivity", "herd_antipathy", "stop_loss_pct",
    "take_profit_pct", "min_edge_pct", "time_horizon",
    "mean_reversion_bias", "learning_rate"
};

/* ─── Extract genome param by index ─── */
static float get_param(const Genome *g, int idx) {
    switch (idx) {
        case 0:  return g->position_size;
        case 1:  return g->conviction_threshold;
        case 2:  return g->risk_tolerance;
        case 3:  return g->lie_sensitivity;
        case 4:  return g->herd_antipathy;
        case 5:  return g->stop_loss_pct;
        case 6:  return g->take_profit_pct;
        case 7:  return g->min_edge_pct;
        case 8:  return g->time_horizon;
        case 9:  return g->mean_reversion_bias;
        case 10: return g->learning_rate;
        default: return 0;
    }
}

/* ─── Comparison for sorting by PnL ─── */
static int cmp_pnl_desc(const void *a, const void *b) {
    const AgentRecord *pa = (const AgentRecord*)a;
    const AgentRecord *pb = (const AgentRecord*)b;
    if (pa->total_pnl > pb->total_pnl) return -1;
    if (pa->total_pnl < pb->total_pnl) return 1;
    return 0;
}

/* ─── Comparison for sorting by Sharpe ─── */
static int cmp_sharpe_desc(const void *a, const void *b) {
    const AgentRecord *pa = (const AgentRecord*)a;
    const AgentRecord *pb = (const AgentRecord*)b;
    if (pa->sharpe > pb->sharpe) return -1;
    if (pa->sharpe < pb->sharpe) return 1;
    return 0;
}

/* ─── Compute stats for a set of agents ─── */
static void compute_param_stats(AgentRecord *agents, int count,
                                 ParamRange *ranges, const char *label) {
    if (count == 0) return;
    
    printf("\n=== %s (%d agents) ===\n", label, count);
    printf("%-22s %8s %8s %8s %8s %8s %8s\n",
           "Param", "Min", "Max", "Mean", "Std", "P10", "P90");
    printf("───────────────────── ──────── ──────── ──────── ──────── ──────── ────────\n");

    for (int p = 0; p < AGENT_FIELD_COUNT; p++) {
        float vals[count];
        float sum = 0, sum2 = 0;
        for (int i = 0; i < count; i++) {
            vals[i] = get_param(&agents[i].genome, p);
            sum += vals[i];
            sum2 += vals[i] * vals[i];
        }
        
        /* Sort for percentiles */
        float sorted[count];
        memcpy(sorted, vals, sizeof(float) * count);
        for (int i = 0; i < count; i++) {
            for (int j = i + 1; j < count; j++) {
                if (sorted[i] > sorted[j]) {
                    float t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
                }
            }
        }

        float min = sorted[0];
        float max = sorted[count - 1];
        float mean = sum / count;
        float variance = fmax(sum2 / count - mean * mean, 0);
        float std = sqrtf(variance);
        float p10 = sorted[(int)(count * 0.1)];
        float p90 = sorted[(int)(count * 0.9)];
        if (p10 == p90 && count > 1) { p10 = sorted[count/4]; p90 = sorted[3*count/4]; }

        ranges[p].name = param_names[p];
        ranges[p].min = min;
        ranges[p].max = max;
        ranges[p].mean = mean;
        ranges[p].std = std;
        ranges[p].top_p10 = p10;
        ranges[p].top_p90 = p90;

        printf("%-22s %8.4f %8.4f %8.4f %8.4f %8.4f %8.4f\n",
               param_names[p], min, max, mean, std, p10, p90);
    }
}

/* ─── Print recommended genome template ─── */
static void print_recommendation(ParamRange *pnl_ranges, ParamRange *sharpe_ranges) {
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  RECOMMENDED GENOME TEMPLATE\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Based on top-decile agents by PnL and Sharpe\n\n");

    printf("  Genome init_template = {\n");
    for (int p = 0; p < AGENT_FIELD_COUNT; p++) {
        /* Blend recommendations: prefer PnL-based, but clamp to Sharpe range */
        float rec_min = fmax(pnl_ranges[p].top_p10, sharpe_ranges[p].top_p10);
        float rec_max = fmin(pnl_ranges[p].top_p90, sharpe_ranges[p].top_p90);
        if (rec_min > rec_max) {
            rec_min = pnl_ranges[p].top_p10;
            rec_max = pnl_ranges[p].top_p90;
        }
        float rec_mid = (rec_min + rec_max) / 2.0f;
        printf("    .%s = %.4f,  /* range [%.4f, %.4f] */\n",
               param_names[p], rec_mid, rec_min, rec_max);
    }
    printf("  };\n\n");
}

/* ─── Log tuning results to SQLite ─── */
static void log_to_db(ParamRange *ranges, int count) {
    sqlite3 *db = NULL;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p /home/wubu2/.hermes/eco_cache");
    system(cmd);

    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "DB error: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS tuning_snapshots ("
        "  ts INTEGER PRIMARY KEY,"
        "  param_name TEXT,"
        "  mean REAL, std REAL,"
        "  p10 REAL, p90 REAL,"
        "  min_val REAL, max_val REAL"
        ");", NULL, NULL, NULL);

    time_t now = time(NULL);
    for (int p = 0; p < count; p++) {
        sqlite3_stmt *stmt = NULL;
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO tuning_snapshots "
            "(ts, param_name, mean, std, p10, p90, min_val, max_val) "
            "VALUES (?,?,?,?,?,?,?,?);",
            -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, (long long)now);
        sqlite3_bind_text(stmt, 2, ranges[p].name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, ranges[p].mean);
        sqlite3_bind_double(stmt, 4, ranges[p].std);
        sqlite3_bind_double(stmt, 5, ranges[p].top_p10);
        sqlite3_bind_double(stmt, 6, ranges[p].top_p90);
        sqlite3_bind_double(stmt, 7, ranges[p].min);
        sqlite3_bind_double(stmt, 8, ranges[p].max);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    printf("[param_tuner] Logged %d params to %s\n", count, DB_PATH);
}

/* ═══════════════════════════════════════════════════════════ */
/*  Read room_state.bin and analyze genome parameters         */
/* ═══════════════════════════════════════════════════════════ */
static int cmd_analyze(const char *state_path) {
    /* Open and mmap state file */
    int fd = open(state_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s\n", state_path);
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    size_t fsize = st.st_size;
    void *map = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap failed for %s\n", state_path);
        return 1;
    }

    /* Parse room_state.bin header to find agent count */
    /* RoomState layout: magic(4) + cycle(4) + ... + agents[MAX_AGENTS] */
    unsigned char *data = (unsigned char*)map;
    uint32_t magic = *(uint32_t*)data;
    uint32_t cycle = *(uint32_t*)(data + 4);
    printf("=== Room State Analysis ===\n");
    printf("Magic:  0x%08X %s\n", magic, magic == 0x524F4F4D ? "(valid)" : "(INVALID)");
    printf("Cycle:  %u\n", cycle);
    printf("Size:   %zu bytes\n", fsize);

    /* Try to locate agents array by scanning for reasonable genome values */
    size_t genome_size = sizeof(Genome);
    size_t agent_struct_size = 2048; /* Approximate AgentState size */
    int max_agents = (fsize - 256) / agent_struct_size;
    if (max_agents > 10000) max_agents = 10000;

    printf("\nSearching for agents (struct ~%zu bytes, max %d)...\n",
           agent_struct_size, max_agents);

    /* Simple scan: look for plausible position_size range */
    AgentRecord *agents = malloc(sizeof(AgentRecord) * max_agents);
    int found = 0;

    for (int i = 0; i < max_agents; i++) {
        size_t offset = fsize - (i + 1) * agent_struct_size;
        if (offset >= fsize || offset < 256) continue;

        Genome *g = (Genome*)(data + offset + 64); /* Skip agent header fields */
        float ps = g->position_size;

        /* Heuristic: valid genome has position_size in reasonable range */
        if (ps >= 0.001f && ps <= 1.0f) {
            agents[found].id = i;
            memcpy(&agents[found].genome, g, sizeof(Genome));
            agents[found].capital = *(float*)(data + offset + 4);
            agents[found].total_pnl = *(float*)(data + offset + 8);
            agents[found].trades = *(int*)(data + offset + 12);
            agents[found].wins = *(int*)(data + offset + 16);
            agents[found].losses = *(int*)(data + offset + 20);
            agents[found].win_rate_ema = *(float*)(data + offset + 24);
            agents[found].sharpe = *(float*)(data + offset + 40);
            agents[found].max_drawdown = *(float*)(data + offset + 44);
            found++;
        }
    }

    printf("Found %d agents with valid genomes\n", found);

    if (found < 10) {
        printf("\nNot enough agents found. Try a different state file or run the engine first.\n");
        munmap(map, fsize);
        free(agents);
        return 0;
    }

    /* Sort by PnL */
    qsort(agents, found, sizeof(AgentRecord), cmp_pnl_desc);

    int top_n = found / 10;
    if (top_n < 5) top_n = 5;
    if (top_n > 100) top_n = 100;

    printf("\nTop 5 agents by PnL:\n");
    for (int i = 0; i < 5 && i < found; i++) {
        printf("  #%d: PnL=%.2f capital=%.2f WR=%.1f%% Sharpe=%.2f trades=%d\n",
               agents[i].id, agents[i].total_pnl, agents[i].capital,
               agents[i].win_rate_ema * 100, agents[i].sharpe, agents[i].trades);
    }

    printf("\nBottom 5 agents by PnL:\n");
    for (int i = found - 5; i < found; i++) {
        printf("  #%d: PnL=%.2f capital=%.2f WR=%.1f%% Sharpe=%.2f trades=%d\n",
               agents[i].id, agents[i].total_pnl, agents[i].capital,
               agents[i].win_rate_ema * 100, agents[i].sharpe, agents[i].trades);
    }

    /* Compute stats for top decile (by PnL) */
    ParamRange pnl_ranges[AGENT_FIELD_COUNT] = {0};
    compute_param_stats(agents, top_n, pnl_ranges, "Top Decile (by PnL)");

    /* Sort by Sharpe and compute top decile */
    qsort(agents, found, sizeof(AgentRecord), cmp_sharpe_desc);
    ParamRange sharpe_ranges[AGENT_FIELD_COUNT] = {0};
    compute_param_stats(agents, top_n, sharpe_ranges, "Top Decile (by Sharpe)");

    /* Print recommendation */
    print_recommendation(pnl_ranges, sharpe_ranges);

    /* Log to DB */
    log_to_db(pnl_ranges, AGENT_FIELD_COUNT);

    /* All agents summary */
    ParamRange all_ranges[AGENT_FIELD_COUNT] = {0};
    compute_param_stats(agents, found, all_ranges, "All Agents");

    munmap(map, fsize);
    free(agents);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s [state_file]|--recommend|--analyze\n", argv[0]);
        printf("  <state_file>  Path to room_state.bin (default: %s)\n", DEFAULT_STATE);
        printf("  --analyze     Analyze default state file\n");
        return 0;
    }

    const char *state_path = DEFAULT_STATE;

    if (!strcmp(argv[1], "--analyze") || !strcmp(argv[1], "--recommend")) {
        return cmd_analyze(state_path);
    }

    state_path = argv[1];
    return cmd_analyze(state_path);
}
