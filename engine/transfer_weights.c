/*
 * transfer_weights.c — Transfer Learning for Agent Weights
 *
 * Reads feature weights from one room_state.bin and applies them
 * to agents in another room_state.bin. Enables cross-asset transfer
 * learning: train on SP500, deploy on BTC.
 *
 * Two modes:
 *   apply <target_state> <source_state> [agent_count]
 *     Copies feat_weight + bias from top N agents in source to matching
 *     agents in target (by agent_id mod). If agent_count <= 0, copies
 *     to ALL alive agents in target.
 *   info <state_file>
 *     Shows top 10 agents' feature weights to inspect learned patterns.
 *
 * Build: gcc transfer_weights.c -o transfer_weights -lm -O2
 * Usage: ./transfer_weights apply target.bin source.bin 1000
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_AGENTS    10000
#define N_FEATURES    18
#define MAX_TRADE_HIST 1000000
#define STATE_MAGIC   0x524F4F4D

typedef struct {
    float position_size;            // 0.01–0.50 fraction of capital
    float conviction_threshold;     // 0.05–0.95 min conviction to act
    float risk_tolerance;           // 0.0–1.0
    float lie_sensitivity;          // 0.10–0.98 how much to distrust crony
    float herd_antipathy;           // 0.0–1.0 contrarian bias
    float stop_loss_pct;            // 0.01–0.25
    float take_profit_pct;          // 0.01–0.60
    float min_edge_pct;             // 1.0–100.0 min expected return
    float time_horizon;             // 0.1–10.0 minutes
    float mean_reversion_bias;      // -1.0–1.0
    // ── v2: Learned feature weights ──
    float feat_weight[N_FEATURES];  // Per-feature weight — Darwin evolves these
    float bias;                     // Learned bias term
    float learning_rate;            // SGD step size (0.001–0.1)
} Genome;

typedef struct {
    bool     alive;
    Genome   genome;
    float    capital;
    float    starting_capital;
    int      trades;
    int      wins, losses;
    float    total_pnl;
    float    max_drawdown;
    float    peak_capital;
    int      consecutive_losses;
    float    win_rate_ema;
    int      last_trade_window;
    float    hidden[4];
    float    last_conviction;
    float    last_features[N_FEATURES];
    float    conv_hi_wins;
    float    conv_hi_total;
    float    conv_lo_wins;
    float    conv_lo_total;
    float    weight_mag;
    float    return_sum;
    float    return_sum_sq;
} AgentState;

typedef struct {
    uint32_t magic;
    uint32_t state_size;
    int64_t  last_updated;
    int      cycle;
    char     _pad0[512];
    int      vote_count;
    char     _pad1[64];
    AgentState agents[MAX_AGENTS];
    char     _pad2[1024];
    int      trade_count;
    char     _trades_pad[56 * MAX_TRADE_HIST]; // TradeRecord[MAX_TRADE_HIST]
} RoomState;

// ── Agent fitness (for sorting) ──
typedef struct {
    int   id;
    float win_rate;
    float pnl;
    float capital;
    int   trades;
} AgentFitness;

int cmp_fitness_desc(const void *a, const void *b) {
    float diff = ((const AgentFitness*)a)->win_rate - ((const AgentFitness*)b)->win_rate;
    return (diff > 0) ? -1 : (diff < 0) ? 1 : 0;
}

// ── Mmap helper ──
static RoomState *map_state(const char *path, int *fd, size_t *size) {
    *fd = open(path, O_RDWR);
    if (*fd < 0) { perror("open"); return NULL; }

    struct stat st;
    if (fstat(*fd, &st) < 0) { perror("fstat"); close(*fd); return NULL; }
    *size = (size_t)st.st_size;

    RoomState *s = (RoomState*)mmap(NULL, *size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, *fd, 0);
    if (s == MAP_FAILED) { perror("mmap"); close(*fd); return NULL; }

    if (s->magic != STATE_MAGIC) {
        fprintf(stderr, "Bad magic: 0x%08X (expected 0x%08X)\n", s->magic, STATE_MAGIC);
        munmap(s, *size); close(*fd); return NULL;
    }
    return s;
}

static void unmap_state(RoomState *s, size_t size, int fd) {
    msync(s, size, MS_SYNC);
    munmap(s, size);
    close(fd);
}

// ── Weight names for display ──
static const char *FEATURE_NAMES[18] = {
    "price_delta_pct", "micro_momentum", "rsi_7", "volume_surge_ratio",
    "divergence_score", "pump_score", "regime", "fear_greed_norm",
    "herd_consensus", "nested_pred", "phi_return", "phi_vol",
    "phi_momentum", "dft_dominant", "tail_risk", "price_lag_1",
    "price_lag_5", "price_lag_15"
};

// ── Info mode: display top agents' feature weights ──
static int cmd_info(const char *path) {
    int fd; size_t size;
    RoomState *s = map_state(path, &fd, &size);
    if (!s) return 1;

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  WEIGHT INSPECTOR — %-35s ║\n", path);
    printf("║  Cycle: %d, Alive: checking...                     ║\n", s->cycle);

    // Collect alive agents with trades
    AgentFitness fit[MAX_AGENTS];
    int n = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (s->agents[i].alive && s->agents[i].trades > 0) {
            fit[n].id = i;
            fit[n].win_rate = s->agents[i].win_rate_ema;
            fit[n].pnl = s->agents[i].total_pnl;
            fit[n].capital = s->agents[i].capital;
            fit[n].trades = s->agents[i].trades;
            n++;
        }
    }
    qsort(fit, n, sizeof(AgentFitness), cmp_fitness_desc);
    printf("║  Alive with trades: %-25d                 ║\n", n);

    // Show top 5 agents' weights
    int show = n < 5 ? n : 5;
    printf("║  ┌─ TOP %d AGENT WEIGHTS ─────────────────────────┐  ║\n", show);

    for (int a = 0; a < show; a++) {
        int id = fit[a].id;
        AgentState *ag = &s->agents[id];
        printf("║  │ Agent %-4d | WR: %5.2f%% | PnL: $%+7.2f | Trades: %-4d │  ║\n",
               id, ag->win_rate_ema * 100.0f, ag->total_pnl, ag->trades);
        printf("║  │ %-20s %8s %8s                        │  ║\n",
               "Feature", "Weight", "Bias");
        printf("║  │ %-20s %+8.4f                        │  ║\n", "(bias)", ag->genome.bias);
        for (int f = 0; f < N_FEATURES; f++) {
            printf("║  │ %-20s %+8.4f                                │  ║\n",
                   FEATURE_NAMES[f], ag->genome.feat_weight[f]);
        }
    }

    // Show mean weights across all agents with trades
    printf("║  ┌─ POPULATION MEAN WEIGHTS (%d agents) ───────┐  ║\n", n);
    float mean_w[N_FEATURES] = {0}, mean_bias = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!s->agents[i].alive || s->agents[i].trades == 0) continue;
        mean_bias += s->agents[i].genome.bias;
        for (int f = 0; f < N_FEATURES; f++)
            mean_w[f] += s->agents[i].genome.feat_weight[f];
    }
    mean_bias /= n;
    printf("║  │ %-20s %+8.4f                        │  ║\n", "(bias)", mean_bias);
    for (int f = 0; f < N_FEATURES; f++) {
        mean_w[f] /= n;
        printf("║  │ %-20s %+8.4f                                │  ║\n",
               FEATURE_NAMES[f], mean_w[f]);
    }

    printf("╚══════════════════════════════════════════════════╝\n");
    unmap_state(s, size, fd);
    return 0;
}

// ── Apply mode: transfer weights from source → target ──
static int cmd_apply(const char *target_path, const char *source_path, int count) {
    int fd_t, fd_s;
    size_t size_t, size_s;

    RoomState *target = map_state(target_path, &fd_t, &size_t);
    if (!target) return 1;
    RoomState *source = map_state(source_path, &fd_s, &size_s);
    if (!source) { unmap_state(target, size_t, fd_t); return 1; }

    // Collect source agents sorted by win_rate
    AgentFitness src_fit[MAX_AGENTS];
    int n_src = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (source->agents[i].alive && source->agents[i].trades > 0) {
            src_fit[n_src].id = i;
            src_fit[n_src].win_rate = source->agents[i].win_rate_ema;
            src_fit[n_src].pnl = source->agents[i].total_pnl;
            src_fit[n_src].capital = source->agents[i].capital;
            src_fit[n_src].trades = source->agents[i].trades;
            n_src++;
        }
    }
    qsort(src_fit, n_src, sizeof(AgentFitness), cmp_fitness_desc);

    if (n_src == 0) {
        printf("No source agents with trades found.\n");
        unmap_state(target, size_t, fd_t);
        unmap_state(source, size_s, fd_s);
        return 1;
    }

    // Determine how many target agents to update
    int n_target = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (target->agents[i].alive) n_target++;
    }

    int to_update = (count <= 0 || count > n_target) ? n_target : count;
    int transfer_count = n_src < to_update ? n_src : to_update;

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  TRANSFER LEARNING — WEIGHTS APPLY              ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  Source: %-42s ║\n", source_path);
    printf("║  Target: %-42s ║\n", target_path);
    printf("║  Source agents w/ trades: %-22d ║\n", n_src);
    printf("║  Target alive agents:     %-22d ║\n", n_target);
    printf("║  Transferring top:        %-22d ║\n", transfer_count);

    // Build target agent list (alive, sorted by win_rate for mapping)
    AgentFitness tgt_fit[MAX_AGENTS];
    int n_tgt = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (target->agents[i].alive) {
            tgt_fit[n_tgt].id = i;
            tgt_fit[n_tgt].win_rate = target->agents[i].win_rate_ema;
            tgt_fit[n_tgt].trades = target->agents[i].trades;
            n_tgt++;
        }
    }

    // Map: worst target agents get best source weights (boost the laggards)
    // Sort target ascending (worst first)
    qsort(tgt_fit, n_tgt, sizeof(AgentFitness), cmp_fitness_desc);
    // Reverse — worst first
    for (int i = 0; i < n_tgt / 2; i++) {
        AgentFitness tmp = tgt_fit[i];
        tgt_fit[i] = tgt_fit[n_tgt - 1 - i];
        tgt_fit[n_tgt - 1 - i] = tmp;
    }

    int applied = 0;
    float total_src_pnl = 0;
    for (int i = 0; i < transfer_count && i < n_tgt; i++) {
        int src_id = src_fit[i].id;  // Best source agents
        int tgt_id = tgt_fit[i].id;  // Worst target agents

        // Copy feat_weight and bias
        memcpy(target->agents[tgt_id].genome.feat_weight,
               source->agents[src_id].genome.feat_weight,
               N_FEATURES * sizeof(float));
        target->agents[tgt_id].genome.bias = source->agents[src_id].genome.bias;

        total_src_pnl += source->agents[src_id].total_pnl;
        applied++;
    }

    printf("║  Applied to:              %-22d ║\n", applied);
    printf("║  Source total PnL transferred: $%-12.2f     ║\n", total_src_pnl);

    // Show first 5 transfers
    printf("║  ┌─ SAMPLE TRANSFERS ───────────────────────────┐  ║\n");
    for (int i = 0; i < (transfer_count < 5 ? transfer_count : 5); i++) {
        int src_id = src_fit[i].id;
        int tgt_id = tgt_fit[i].id;
        printf("║  │ Tgt %-4d ← Src %-4d | Src WR: %5.2f%% | PnL: $%+7.2f │  ║\n",
               tgt_id, src_id,
               source->agents[src_id].win_rate_ema * 100.0f,
               source->agents[src_id].total_pnl);
    }

    printf("╚══════════════════════════════════════════════════╝\n");

    unmap_state(target, size_t, fd_t);
    unmap_state(source, size_s, fd_s);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s info <state_file>\n", argv[0]);
        fprintf(stderr, "  %s apply <target_state> <source_state> [agent_count]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "info") == 0) {
        return cmd_info(argv[2]);
    } else if (strcmp(argv[1], "apply") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s apply <target> <source> [count]\n", argv[0]);
            return 1;
        }
        int count = argc > 4 ? atoi(argv[4]) : 1000;
        return cmd_apply(argv[2], argv[3], count);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
}
