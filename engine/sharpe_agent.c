/*
 * sharpe_agent.c — Per-Agent Sharpe Ratio Calculator
 *
 * Reads room_state.bin, groups TradeRecords by agent_id,
 * computes per-agent Sharpe ratio = mean(pnl_pct) / std(pnl_pct).
 *
 * Build: gcc sharpe_agent.c -o sharpe_agent -lm -O2
 * Usage: ./sharpe_agent [room_state.bin path] [min_trades]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

// ── Types (mirrored from types.h, trimmed to what we need) ──
#define PHI 1.618033988749895f
#define INV_PHI 0.618033988749895f
#define TWO_PI 6.283185307179586f
#define ROOM_AGENTS  10000
#define MAX_AGENTS    ROOM_AGENTS
#define N_FEATURES    48
#define MAX_ASSETS    8
#define MAX_TRADE_HIST 1000000
#define STATE_MAGIC   0x524F4F4D

typedef struct {
    float weights[N_FEATURES];
    float bias;
    // Genome params
    float min_edge;
    float max_risk;
    float conviction_bias;
    float time_horizon;
    float vol_scale;
    float take_profit_pct;
    float stop_loss_pct;
    int   max_consecutive_trades;
    float ma_period_ratio;
    float momentum_decay;
} Genome;

typedef struct {
    int64_t  window_ts;
    char     asset[8];
    int      agent_id;
    bool     direction;
    float    position_size;
    float    entry_price;
    float    exit_price;
    float    pnl_pct;
    bool     won;
    int64_t  resolved_at;
} TradeRecord;

typedef struct {
    // Feature importance tracking
    char     name[32];
    float    pos_contrib_wins;
    float    pos_contrib_total;
    float    neg_contrib_wins;
    float    neg_contrib_total;
} FeatureImportanceEntry;

#define MAX_FEATURES 32
typedef struct {
    int              count;
    FeatureImportanceEntry entries[MAX_FEATURES];
} FeatureImportance;

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
} AgentState;

typedef struct {
    uint32_t magic;
    uint32_t state_size;
    int64_t  last_updated;
    int      cycle;
    // Market + features
    char     _pad0[512];
    // Vote + agents
    int      vote_count;
    char     _pad1[64];
    AgentState agents[MAX_AGENTS];
    char     _pad2[1024];
    int      trade_count;
    TradeRecord trades[MAX_TRADE_HIST];
    char     _pad3[4096];
} RoomState;

// ── Per-agent aggregate trade data ──
typedef struct {
    int   agent_id;
    int   n_trades;
    int   wins;
    int   losses;
    double sum_pnl;
    double sum_pnl_sq;
    double max_pnl_pct;
    double min_pnl_pct;
} AgentTradeAgg;

// ── Agent trade return series (for sorting) ──
typedef struct {
    int    agent_id;
    int    n_trades;
    double mean_return;
    double std_return;
    double sharpe;
    double win_rate;
    double avg_pnl_pct;
} AgentSharpe;

int agent_agg_cmp(const void *a, const void *b) {
    double diff = ((const AgentTradeAgg*)a)->sum_pnl - ((const AgentTradeAgg*)b)->sum_pnl;
    return (diff > 0) ? -1 : (diff < 0) ? 1 : 0;
}

int sharpe_cmp_desc(const void *a, const void *b) {
    double diff = ((const AgentSharpe*)a)->sharpe - ((const AgentSharpe*)b)->sharpe;
    return (diff > 0) ? -1 : (diff < 0) ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "room_state.bin";
    int min_trades = argc > 2 ? atoi(argv[2]) : 5;

    // Open and mmap the state file
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return 1; }

    size_t file_size = (size_t)st.st_size;
    char *map = (char*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    // Parse header
    uint32_t magic, state_size;
    memcpy(&magic, map, 4);
    memcpy(&state_size, map + 4, 4);

    if (magic != STATE_MAGIC) {
        fprintf(stderr, "Bad magic: 0x%08X (expected 0x%08X)\n", magic, STATE_MAGIC);
        munmap(map, file_size); close(fd); return 1;
    }

    if (state_size > 0 && state_size < sizeof(RoomState)) {
        fprintf(stderr, "State size %u < RoomState %zu — struct mismatch\n",
                state_size, sizeof(RoomState));
        munmap(map, file_size); close(fd); return 1;
    }

    // Cast directly — handle old state files (state_size==0) and current
    RoomState *state = (RoomState*)map;

    int cycle = state->cycle;
    int trade_count = state->trade_count;
    if (trade_count > MAX_TRADE_HIST) trade_count = MAX_TRADE_HIST;

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  PER-AGENT SHARPE RATIO ANALYZER                ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  State file: %-40s ║\n", path);
    printf("║  Cycle:      %-40d ║\n", cycle);
    printf("║  Trade recs: %-40d ║\n", trade_count);
    printf("║  Min trades: %-40d ║\n", min_trades);
    printf("╠══════════════════════════════════════════════════╣\n");

    // Phase 1: Aggregate trade data per agent
    int max_agents = 0;
    AgentTradeAgg *aggs = calloc(MAX_AGENTS, sizeof(AgentTradeAgg));

    for (int i = 0; i < trade_count; i++) {
        TradeRecord *t = &state->trades[i];
        if (t->agent_id < 0 || t->agent_id >= MAX_AGENTS) continue;

        AgentTradeAgg *a = &aggs[t->agent_id];
        if (a->n_trades == 0) {
            a->agent_id = t->agent_id;
            a->min_pnl_pct = 1e10;
            a->max_pnl_pct = -1e10;
        }

        double r = (double)t->pnl_pct;
        a->n_trades++;
        a->sum_pnl += r;
        a->sum_pnl_sq += r * r;
        if (t->won) a->wins++;
        else a->losses++;
        if (r > a->max_pnl_pct) a->max_pnl_pct = r;
        if (r < a->min_pnl_pct) a->min_pnl_pct = r;
        if (t->agent_id > max_agents) max_agents = t->agent_id;
    }

    // Count qualifying agents
    int n_qualifying = 0;
    for (int i = 0; i <= max_agents; i++) {
        if (aggs[i].n_trades >= min_trades) n_qualifying++;
    }

    if (n_qualifying == 0) {
        printf("║  ⚠  No agents with ≥%d trades found                    ║\n", min_trades);
        printf("║  (trade_log.csv has few trades — needs accumulation)   ║\n");
        printf("╚══════════════════════════════════════════════════╝\n");
        free(aggs);
        munmap(map, file_size); close(fd);
        return 0;
    }

    // Phase 2: Compute Sharpe per qualifying agent
    AgentSharpe *results = calloc(n_qualifying, sizeof(AgentSharpe));
    int idx = 0;
    int worst_idx = -1;
    double worst_sharpe = 1e10;
    int best_idx = -1;
    double best_sharpe = -1e10;
    double sharpe_sum = 0;

    for (int i = 0; i <= max_agents; i++) {
        if (aggs[i].n_trades < min_trades) continue;

        AgentTradeAgg *a = &aggs[i];
        AgentSharpe *r = &results[idx];
        r->agent_id = a->agent_id;
        r->n_trades = a->n_trades;
        r->mean_return = a->sum_pnl / a->n_trades;
        r->win_rate = (double)a->wins / a->n_trades;
        r->avg_pnl_pct = r->mean_return * 100;

        // Std of returns
        double variance = (a->sum_pnl_sq / a->n_trades) - (r->mean_return * r->mean_return);
        if (variance < 0) variance = 0;
        r->std_return = sqrt(variance);

        // Sharpe = mean / std (trade-level, risk-free ≈ 0 for intra-cycle trades)
        if (r->std_return > 1e-12) {
            r->sharpe = r->mean_return / r->std_return;
        } else {
            r->sharpe = r->mean_return > 0 ? 10.0 : -10.0; // Near-zero variance → extreme
        }

        sharpe_sum += r->sharpe;
        if (r->sharpe > best_sharpe) { best_sharpe = r->sharpe; best_idx = idx; }
        if (r->sharpe < worst_sharpe) { worst_sharpe = r->sharpe; worst_idx = idx; }
        idx++;
    }

    // Sort by Sharpe desc
    qsort(results, n_qualifying, sizeof(AgentSharpe), sharpe_cmp_desc);

    // Phase 3: Compute room-level metrics
    double mean_sharpe = sharpe_sum / n_qualifying;
    double sharpe_sum_sq = 0;
    for (int i = 0; i < n_qualifying; i++) {
        double d = results[i].sharpe - mean_sharpe;
        sharpe_sum_sq += d * d;
    }
    double sharpe_std = sqrt(sharpe_sum_sq / n_qualifying);

    double pos_sharpe_count = 0, neg_sharpe_count = 0;
    double pos_sharpe_sum = 0, neg_sharpe_sum = 0;
    for (int i = 0; i < n_qualifying; i++) {
        if (results[i].sharpe > 0) { pos_sharpe_count++; pos_sharpe_sum += results[i].sharpe; }
        else { neg_sharpe_count++; neg_sharpe_sum += results[i].sharpe; }
    }

    printf("║                                                      ║\n");
    printf("║  ┌─ ROOM SUMMARY ─────────────────────────────────┐  ║\n");
    printf("║  │ Agents with ≥%d trades:  %-6d                   │  ║\n", min_trades, n_qualifying);
    printf("║  │ Mean Sharpe:              %+7.4f               │  ║\n", mean_sharpe);
    printf("║  │ Std Sharpe:               %7.4f                │  ║\n", sharpe_std);
    printf("║  │ Positive Sharpe agents:   %-6d (%5.1f%%)       │  ║\n", (int)pos_sharpe_count, 100.0*pos_sharpe_count/n_qualifying);
    printf("║  │ Negative Sharpe agents:   %-6d (%5.1f%%)       │  ║\n", (int)neg_sharpe_count, 100.0*neg_sharpe_count/n_qualifying);
    printf("║  │ Best Sharpe:              %+7.4f               │  ║\n", results[0].sharpe);
    printf("║  │ Worst Sharpe:             %+7.4f               │  ║\n", results[n_qualifying-1].sharpe);
    printf("║  └────────────────────────────────────────────────┘  ║\n");

    // Top 10
    int top_n = n_qualifying < 10 ? n_qualifying : 10;
    printf("║                                                      ║\n");
    printf("║  ┌─ TOP %d BY SHARPE ─────────────────────────┐  ║\n", top_n);
    printf("║  │ %-6s │ %-5s │ %-7s │ %-7s │ %-5s │ %-6s │  ║\n",
           "Agent", "Trades", "Sharpe", "MeanR%", "WR%", "AvgP%");
    printf("║  ├────────┼───────┼──────────┼──────────┼───────┼──────────┤  ║\n");
    for (int i = 0; i < top_n; i++) {
        printf("║  │ %-6d │ %-5d │ %+8.4f │ %+7.4f │ %4.1f │ %+6.3f │  ║\n",
               results[i].agent_id,
               results[i].n_trades,
               results[i].sharpe,
               results[i].avg_pnl_pct,
               results[i].win_rate * 100,
               results[i].mean_return * 100);
    }

    // Bottom 5
    int bot_n = n_qualifying < 5 ? n_qualifying : 5;
    printf("║                                                      ║\n");
    printf("║  ┌─ BOTTOM %d BY SHARPE ──────────────────────┐  ║\n", bot_n);
    for (int i = n_qualifying - bot_n; i < n_qualifying; i++) {
        printf("║  │ %-6d │ %-5d │ %+8.4f │ %+7.4f │ %4.1f │ %+6.3f │  ║\n",
               results[i].agent_id,
               results[i].n_trades,
               results[i].sharpe,
               results[i].avg_pnl_pct,
               results[i].win_rate * 100,
               results[i].mean_return * 100);
    }

    // Sharpe distribution buckets
    printf("║                                                      ║\n");
    printf("║  ┌─ SHARPE DISTRIBUTION ───────────────────────┐  ║\n");
    int buckets[7] = {0}; // < -1, -1 to -0.5, -0.5 to 0, 0 to 0.5, 0.5 to 1, 1 to 2, >2
    const char *bucket_labels[] = {"< -1.0", "-1.0:-0.5", "-0.5:0", "0:0.5", "0.5:1.0", "1.0:2.0", "> 2.0"};
    for (int i = 0; i < n_qualifying; i++) {
        double s = results[i].sharpe;
        if (s < -1.0) buckets[0]++;
        else if (s < -0.5) buckets[1]++;
        else if (s < 0) buckets[2]++;
        else if (s < 0.5) buckets[3]++;
        else if (s < 1.0) buckets[4]++;
        else if (s < 2.0) buckets[5]++;
        else buckets[6]++;
    }
    for (int i = 0; i < 7; i++) {
        int bar = (int)((double)buckets[i] / n_qualifying * 30);
        printf("║  │ %-10s │ %-5d %s│  ║\n",
               bucket_labels[i], buckets[i],
               bar > 0 ? "██████████████████████████████" + 30 - bar : "");
    }

    // Phase 4: Cross-reference with AgentState (capital, WR, total_pnl)
    printf("║                                                      ║\n");
    printf("║  ┌─ BEST SHARPE AGENT DETAIL (ID %d) ───────────┐  ║\n", results[0].agent_id);
    int aid = results[0].agent_id;
    AgentState *as = &state->agents[aid];
    printf("║  │ Capital: $%-10.2f  Trades: %-6d             │  ║\n", as->capital, as->trades);
    printf("║  │ Wins: %-5d  Losses: %-5d  WR: %5.2f%%        │  ║\n",
           as->wins, as->losses, as->trades > 0 ? 100.0 * as->wins / as->trades : 0);
    printf("║  │ Total PnL: $%-9.2f  Max DD: %6.2f%%          │  ║\n",
           as->total_pnl, as->capital > 0 ? (1 - as->capital / as->peak_capital) * 100 : 0);

    // Worst agent
    printf("║  ┌─ WORST SHARPE AGENT DETAIL (ID %d) ──────────┐  ║\n", results[n_qualifying-1].agent_id);
    aid = results[n_qualifying-1].agent_id;
    as = &state->agents[aid];
    printf("║  │ Capital: $%-10.2f  Trades: %-6d             │  ║\n", as->capital, as->trades);
    printf("║  │ Wins: %-5d  Losses: %-5d  WR: %5.2f%%        │  ║\n",
           as->wins, as->losses, as->trades > 0 ? 100.0 * as->wins / as->trades : 0);
    printf("║  │ Total PnL: $%-9.2f  Max DD: %6.2f%%          │  ║\n",
           as->total_pnl, as->capital > 0 ? (1 - as->capital / as->peak_capital) * 100 : 0);

    printf("╚══════════════════════════════════════════════════╝\n");

    free(aggs);
    free(results);
    munmap(map, file_size);
    close(fd);
    return 0;
}
