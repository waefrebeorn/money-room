/**
 * param_sweep.c — T44: Parameter Sweep / Hyperparameter Search
 *
 * Reads room_state.bin, performs grid sweep over genome parameters,
 * finds optimal ranges by comparing top-decile vs bottom-decile agents.
 *
 * Build: gcc -O3 -march=native param_sweep.c -o param_sweep -lm
 * Usage: ./param_sweep [room_state.bin]
 *        ./param_sweep sweep <steps>    — full grid sweep (default 10)
 *        ./param_sweep quick            — fast scan (5 steps)
 *        ./param_sweep recommend        — print optimal genome template only
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
#define MAX_GENOME_PARAMS 10

static const char *param_names[MAX_GENOME_PARAMS] = {
    "position_size", "conviction_threshold", "risk_tolerance",
    "lie_sensitivity", "herd_antipathy", "stop_loss_pct",
    "take_profit_pct", "min_edge_pct", "time_horizon",
    "mean_reversion_bias"
};

typedef struct {
    int agent_id;
    float param_val;
    float pnl_pct;
    float win_rate;
    float sharpe;
    int trades;
    float capital;
} AgentRec;

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

static double calc_agent_sharpe(const AgentState *a) {
    return a->trades > 0
        ? (double)a->wins / a->trades * 100.0
        : 0;
}

static void print_param_range(const char *name, float min_v, float max_v,
                               float top_mean, float bot_mean,
                               int top_n, int bot_n) {
    char direction = top_mean > bot_mean ? '^' : 'v';
    printf("  %-22s [%.4f, %.4f]  top=%.4f  bot=%.4f  %c  (t:%d b:%d)\n",
           name, min_v, max_v, top_mean, bot_mean, direction, top_n, bot_n);
}

static int cmp_pnl_desc(const void *a, const void *b) {
    float va = ((AgentRec*)a)->pnl_pct;
    float vb = ((AgentRec*)b)->pnl_pct;
    if (va > vb) return -1;
    if (va < vb) return 1;
    return 0;
}

static int cmp_param(const void *a, const void *b) {
    float va = ((AgentRec*)a)->param_val;
    float vb = ((AgentRec*)b)->param_val;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

/* ─── Sweep a single parameter ─── */
static void sweep_param(RoomState *state, int param_idx, int n_steps, int top_pct) {
    int max_agents = state->stats.active_agents > 0 && state->stats.active_agents < MAX_AGENTS
                     ? state->stats.active_agents : MAX_AGENTS;
    if (max_agents > MAX_AGENTS) max_agents = MAX_AGENTS;
    if (max_agents < 10) { printf("  Not enough agents (%d)\n", max_agents); return; }

    /* Collect agents with trades */
    int n = 0;
    AgentRec *agents = malloc(max_agents * sizeof(AgentRec));
    if (!agents) return;
    
    for (int i = 0; i < max_agents; i++) {
        if (state->agents[i].trades > 0) {
            agents[n].agent_id = i;
            agents[n].param_val = get_param(&state->agents[i].genome, param_idx);
            agents[n].pnl_pct = state->agents[i].total_pnl;
            agents[n].win_rate = state->agents[i].trades > 0
                ? (float)state->agents[i].wins / state->agents[i].trades : 0;
            agents[n].sharpe = calc_agent_sharpe(&state->agents[i]);
            agents[n].trades = state->agents[i].trades;
            agents[n].capital = state->agents[i].capital;
            n++;
        }
    }

    if (n < 20) { free(agents); return; }

    /* Find param range */
    float p_min = agents[0].param_val, p_max = agents[0].param_val;
    for (int i = 1; i < n; i++) {
        if (agents[i].param_val < p_min) p_min = agents[i].param_val;
        if (agents[i].param_val > p_max) p_max = agents[i].param_val;
    }
    float range = p_max - p_min;
    if (range < 0.001f) { free(agents); return; }

    /* Sort by param value, then divide into steps */
    qsort(agents, n, sizeof(AgentRec), cmp_param);
    int per_bin = n / n_steps;
    if (per_bin < 2) per_bin = 2;

    printf("\n  %s [%.4f-%.4f]:\n", param_names[param_idx], p_min, p_max);
    printf("  %-12s %-8s %-8s %-8s %-8s\n", "Range", "PnL%", "WR%", "Sharpe", "N");

    for (int step = 0; step < n_steps && step * per_bin < n; step++) {
        int start = step * per_bin;
        int end = start + per_bin;
        if (end > n) end = n;
        if (start >= n) break;
        
        float lo = agents[start].param_val;
        float hi = agents[end-1].param_val;
        float sum_pnl = 0, sum_wr = 0, sum_sharpe = 0;
        for (int j = start; j < end; j++) {
            sum_pnl += agents[j].pnl_pct;
            sum_wr += agents[j].win_rate;
            sum_sharpe += agents[j].sharpe;
        }
        int cnt = end - start;
        printf("  [%.4f-%.4f] %+8.2f %7.2f%% %8.3f %5d\n",
               lo, hi, sum_pnl/cnt, sum_wr/cnt*100, sum_sharpe/cnt, cnt);
    }

    /* Find optimal range (top bins by avg PnL) */
    /* Sort by PnL descending, pick top_decile, compute their param stats */
    qsort(agents, n, sizeof(AgentRec), cmp_pnl_desc);
    int n_top = n * top_pct / 100;
    if (n_top < 5) n_top = 5;
    
    float top_pnl_sum = 0, top_param_sum = 0, top_param_min = agents[0].param_val, top_param_max = agents[0].param_val;
    for (int i = 0; i < n_top; i++) {
        top_pnl_sum += agents[i].pnl_pct;
        top_param_sum += agents[i].param_val;
        if (agents[i].param_val < top_param_min) top_param_min = agents[i].param_val;
        if (agents[i].param_val > top_param_max) top_param_max = agents[i].param_val;
    }
    
    /* Bottom decile */
    int n_bot = n * top_pct / 100;
    if (n_bot < 5) n_bot = 5;
    float bot_pnl_sum = 0, bot_param_sum = 0;
    for (int i = n - n_bot; i < n; i++) {
        bot_pnl_sum += agents[i].pnl_pct;
        bot_param_sum += agents[i].param_val;
    }

    printf("  ── Optimal range: [%.4f, %.4f]  (top %d%% avg=%.4f, bot %d%% avg=%.4f) %s\n",
           top_param_min, top_param_max, top_pct, top_param_sum/n_top,
           top_pct, bot_param_sum/n_bot,
           (top_param_sum/n_top > bot_param_sum/n_bot) ? "↑ higher better" : "↓ lower better");

    free(agents);
}

/* ─── Compare top vs bottom decile across all params ─── */
static void cmd_quick(RoomState *state) {
    int max_agents = state->stats.active_agents > 0 && state->stats.active_agents < MAX_AGENTS
                     ? state->stats.active_agents : MAX_AGENTS;
    if (max_agents > MAX_AGENTS) max_agents = MAX_AGENTS;
    if (max_agents < 10) { return; }

    int n = 0;
    AgentRec *agents = malloc(max_agents * sizeof(AgentRec));
    if (!agents) return;
    
    for (int i = 0; i < max_agents; i++) {
        if (state->agents[i].trades > 0) {
            agents[n].agent_id = i;
            agents[n].pnl_pct = state->agents[i].total_pnl;
            agents[n].trades = state->agents[i].trades;
            n++;
        }
    }
    if (n < 20) { free(agents); return; }

    qsort(agents, n, sizeof(AgentRec), cmp_pnl_desc);
    int n_top = n / 10;
    if (n_top < 5) n_top = 5;
    
    printf("\n=== Parameter Sweep (quick): %d agents, top/bottom %d%% ===\n", n, 10);
    printf("  %-22s %-12s %-12s %s\n", "Parameter", "Top-10% avg", "Bot-10% avg", "Direction");
    printf("  %s\n", "─────────────────────────────────────────────────────────────");

    for (int p = 0; p < MAX_GENOME_PARAMS; p++) {
        float top_sum = 0, bot_sum = 0;
        for (int i = 0; i < n_top; i++) {
            top_sum += get_param(&state->agents[agents[i].agent_id].genome, p);
        }
        for (int i = n - n_top; i < n; i++) {
            bot_sum += get_param(&state->agents[agents[i].agent_id].genome, p);
        }
        float top_avg = top_sum / n_top;
        float bot_avg = bot_sum / n_top;
        printf("  %-22s %-12.4f %-12.4f %s\n", param_names[p], top_avg, bot_avg, top_avg > bot_avg ? "higher=better ↑" : "lower=better ↓");
    }
    free(agents);
}

/* ─── Full sweep across all params ─── */
static void cmd_sweep(RoomState *state, int steps) {
    if (steps < 3) steps = 3;
    if (steps > 20) steps = 20;
    printf("\n=== Full Parameter Sweep (%d steps per param) ===\n", steps);
    for (int p = 0; p < MAX_GENOME_PARAMS; p++) {
        sweep_param(state, p, steps, 10);
    }
}

/* ─── Print optimal genome template ─── */
static void cmd_recommend(RoomState *state) {
    int max_agents = state->stats.active_agents > 0 && state->stats.active_agents < MAX_AGENTS
                     ? state->stats.active_agents : MAX_AGENTS;
    if (max_agents > MAX_AGENTS) max_agents = MAX_AGENTS;
    
    int n = 0;
    AgentRec *agents = malloc(max_agents * sizeof(AgentRec));
    if (!agents) return;
    
    for (int i = 0; i < max_agents; i++) {
        if (state->agents[i].trades > 0) {
            agents[n].agent_id = i;
            agents[n].pnl_pct = state->agents[i].total_pnl;
            agents[n].trades = state->agents[i].trades;
            agents[n].capital = state->agents[i].capital;
            n++;
        }
    }
    if (n < 10) { free(agents); printf("Not enough trading agents.\n"); return; }

    qsort(agents, n, sizeof(AgentRec), cmp_pnl_desc);
    int n_top = n / 10;
    if (n_top < 5) n_top = 5;

    printf("\n=== Recommended Genome Template ===\n");
    printf("  Based on top %d/%d agents by total PnL\n\n", n_top, n);
    printf("  %-22s %-10s %-10s %-10s\n", "Parameter", "Optimal", "Min", "Max");
    printf("  %s\n", "───────────────────────────────────────────────────");
    
    for (int p = 0; p < MAX_GENOME_PARAMS; p++) {
        float sum = 0, min_v = 1e10, max_v = -1e10;
        for (int i = 0; i < n_top; i++) {
            float v = get_param(&state->agents[agents[i].agent_id].genome, p);
            sum += v;
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
        }
        printf("  %-22s %-10.4f %-10.4f %-10.4f\n", param_names[p], sum/n_top, min_v, max_v);
    }

    /* Also print the single best agent's genome */
    int best = agents[0].agent_id;
    printf("\n  Best Agent (#%d) — PnL: $%.2f, Trades: %d, WR: %.1f%%, Capital: $%.2f\n",
           best, state->agents[best].total_pnl, state->agents[best].trades,
           state->agents[best].trades > 0
               ? (float)state->agents[best].wins / state->agents[best].trades * 100 : 0.0f,
           state->agents[best].capital);
    printf("  Genome:\n");
    printf("    position_size:          %.4f\n", state->agents[best].genome.position_size);
    printf("    conviction_threshold:   %.4f\n", state->agents[best].genome.conviction_threshold);
    printf("    risk_tolerance:         %.4f\n", state->agents[best].genome.risk_tolerance);
    printf("    lie_sensitivity:        %.4f\n", state->agents[best].genome.lie_sensitivity);
    printf("    herd_antipathy:         %.4f\n", state->agents[best].genome.herd_antipathy);
    printf("    stop_loss_pct:          %.4f\n", state->agents[best].genome.stop_loss_pct);
    printf("    take_profit_pct:        %.4f\n", state->agents[best].genome.take_profit_pct);
    printf("    min_edge_pct:           %.4f\n", state->agents[best].genome.min_edge_pct);
    printf("    time_horizon:           %.4f\n", state->agents[best].genome.time_horizon);
    printf("    mean_reversion_bias:    %.4f\n", state->agents[best].genome.mean_reversion_bias);

    free(agents);
}

int main(int argc, char **argv) {
    const char *path = STATE_PATH_DEFAULT;
    int steps = 10;
    int mode = 0; /* 0=quick, 1=sweep, 2=recommend */

    if (argc > 1) {
        if (strcmp(argv[1], "sweep") == 0) {
            mode = 1;
            if (argc > 2) steps = atoi(argv[2]);
        } else if (strcmp(argv[1], "quick") == 0) {
            mode = 0;
        } else if (strcmp(argv[1], "recommend") == 0) {
            mode = 2;
        } else {
            path = argv[1];
        }
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Can't open %s\n", path); return 1; }
    struct stat st;
    fstat(fd, &st);
    RoomState *state = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (state == MAP_FAILED) { fprintf(stderr, "mmap failed\n"); return 1; }
    if (state->magic != 0x524F4F4D) {
        fprintf(stderr, "Bad magic 0x%08X\n", state->magic);
        munmap(state, st.st_size);
        return 1;
    }

    if (mode == 0) cmd_quick(state);
    else if (mode == 1) cmd_sweep(state, steps);
    else cmd_recommend(state);

    munmap(state, st.st_size);
    return 0;
}
