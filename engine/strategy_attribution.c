/**
 * strategy_attribution.c — T41: Strategy Attribution
 *
 * Reads room_state.bin, groups agents by genome parameter deciles,
 * computes average PnL per strategy bucket.
 *
 * Answers: "Strategy X earned Y%"
 *
 * Build: gcc -O3 -march=native strategy_attribution.c -o strategy_attribution -lm
 * Usage: ./strategy_attribution [/path/to/room_state.bin]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "types.h"

#define STATE_PATH_DEFAULT "/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"
#define N_BUCKETS 10  // decile buckets
#define N_PARAMS  10  // genome parameters

static const char *param_names[N_PARAMS] = {
    "position_size", "conviction_threshold", "risk_tolerance",
    "lie_sensitivity", "herd_antipathy", "stop_loss_pct",
    "take_profit_pct", "min_edge_pct", "time_horizon",
    "mean_reversion_bias"
};

// Extract a genome parameter value by index
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
        default: return 0;
    }
}

// For sorting agents by a parameter value
typedef struct {
    int   agent_id;
    float param_val;
    float pnl_pct;
    int   trades;
    float win_rate;
} AgentRecord;

static int cmp_by_param(const void *a, const void *b) {
    float va = ((AgentRecord*)a)->param_val;
    float vb = ((AgentRecord*)b)->param_val;
    if (va < vb) return -1;
    if (va > vb) return  1;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : STATE_PATH_DEFAULT;

    // ── mmap room_state.bin ──
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s\n", path);
        return 1;
    }
    struct stat st;
    fstat(fd, &st);
    RoomState *state;
    size_t map_size;
    // Relaxed size check: accept files >= 90% of expected (version drift)
    if (st.st_size < (off_t)(sizeof(RoomState) * 0.9)) {
        fprintf(stderr, "State file too small (%ld bytes, need %zu)\n",
                (long)st.st_size, sizeof(RoomState));
        close(fd);
        return 1;
    }
    map_size = st.st_size < (off_t)sizeof(RoomState) ? st.st_size : sizeof(RoomState);
    state = (RoomState *)mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (state == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("=== Strategy Attribution ===\n");
    printf("Cycle: %d  |  Active agents: %d\n\n",
           state->cycle, state->stats.active_agents);

    // Collect live agents
    AgentRecord *agents = malloc(MAX_AGENTS * sizeof(AgentRecord));
    int n_agents = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (state->agents[i].alive && state->agents[i].trades > 0) {
            agents[n_agents].agent_id  = i;
            agents[n_agents].pnl_pct   = state->agents[i].total_pnl;
            agents[n_agents].trades    = state->agents[i].trades;
            agents[n_agents].win_rate  = state->agents[i].trades > 0
                ? (float)state->agents[i].wins / state->agents[i].trades : 0;
            n_agents++;
        }
    }

    if (n_agents == 0) {
        printf("No agents with trades found.\n");
        munmap(state, st.st_size);
        free(agents);
        return 0;
    }

    // Top 10 agents by PnL
    printf("── Top 10 by PnL ──\n");
    printf("%-5s %10s %8s %6s %8s  Params\n", "ID", "PnL%", "WR%", "Trades", "Capital");
    for (int rank = 0; rank < 10 && rank < n_agents; rank++) {
        int best = 0;
        for (int i = 1; i < n_agents; i++)
            if (agents[i].pnl_pct > agents[best].pnl_pct) best = i;
        int aid = agents[best].agent_id;
        AgentState *a = &state->agents[aid];
        printf("%-5d %+8.2f%% %7.1f%% %6d %8.2f  ",
               aid, a->total_pnl * 100.0f, a->win_rate_ema * 100.0f,
               a->trades, a->capital);
        for (int p = 0; p < N_PARAMS; p++)
            printf("%s=%.3f ", param_names[p], get_param(&a->genome, p));
        printf("\n");
        agents[best].pnl_pct = -999999;  // mark used
    }

    // ── Per-parameter decile analysis ──
    printf("\n── Parameter Attribution (decile buckets) ──\n");
    for (int p = 0; p < N_PARAMS; p++) {
        // Sort agents by this parameter
        AgentRecord *sorted = malloc(n_agents * sizeof(AgentRecord));
        memcpy(sorted, agents, n_agents * sizeof(AgentRecord));
        // Restore PnL (we stomped top 10)
        for (int i = 0; i < n_agents; i++)
            sorted[i].pnl_pct = state->agents[sorted[i].agent_id].total_pnl;
        qsort(sorted, n_agents, sizeof(AgentRecord), cmp_by_param);

        float min_val = sorted[0].param_val;
        float max_val = sorted[n_agents-1].param_val;
        float range = max_val - min_val;

        printf("\n  %s (range: %.4f – %.4f):\n", param_names[p], min_val, max_val);

        // Compute per-decile stats
        for (int b = 0; b < N_BUCKETS; b++) {
            int start = b * n_agents / N_BUCKETS;
            int end   = (b + 1) * n_agents / N_BUCKETS;
            if (start >= n_agents) break;
            if (end > n_agents) end = n_agents;
            int count = end - start;

            double sum_pnl = 0, sum_wr = 0;
            float lo = sorted[start].param_val;
            float hi = sorted[end-1].param_val;
            for (int i = start; i < end; i++) {
                sum_pnl += sorted[i].pnl_pct;
                sum_wr  += sorted[i].win_rate;
            }
            printf("    D%d [%.4f-%.4f]: n=%3d avg_pnl=%+7.2f%% avg_wr=%5.1f%%\n",
                   b, lo, hi, count,
                   sum_pnl / count * 100.0,
                   sum_wr / count * 100.0);
        }
        free(sorted);
    }

    munmap(state, st.st_size);
    free(agents);
    return 0;
}
