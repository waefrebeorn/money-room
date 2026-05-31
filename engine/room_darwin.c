/**
 * room_darwin.c — L5: Darwinian Evolution Engine
 * Bottom 10% culled every 100 trades, top 10% cloned + mutated.
 * Crony filter: agents that trusted crony pump signals get culled first.
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "types.h"

// ── Genome mutation bounds ──
typedef struct {
    float min, max;
    float mut_scale; // Mutation step size as fraction of range
} ParamRange;

static const ParamRange PARAM_RANGES[] = {
    { 0.01f, 0.50f, 0.05f },  // position_size
    { 0.01f, 0.70f, 0.08f },  // conviction_threshold
    { 0.00f, 1.00f, 0.10f },  // risk_tolerance
    { 0.10f, 0.98f, 0.08f },  // lie_sensitivity
    { 0.00f, 1.00f, 0.12f },  // herd_antipathy
    { 0.01f, 0.25f, 0.03f },  // stop_loss_pct
    { 0.01f, 0.60f, 0.05f },  // take_profit_pct
    { 0.50f, 50.0f, 5.0f },  // min_edge_pct
    { 0.10f, 10.0f, 0.80f },  // time_horizon
    { -1.0f, 1.00f, 0.15f },  // mean_reversion_bias
};

#define N_PARAMS (sizeof(PARAM_RANGES) / sizeof(PARAM_RANGES[0]))

// ── Fast clamp ──
static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ── Mutate a single float param ──
static float mutate_param(float val, float min, float max, float scale, float rate) {
    float delta = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale * rate;
    return clampf(val + delta, min, max);
}

// ── Mutate one genome ──
static void mutate_genome(Genome *g, float mutation_rate) {
    g->position_size       = mutate_param(g->position_size,       PARAM_RANGES[0].min,  PARAM_RANGES[0].max,  PARAM_RANGES[0].mut_scale,  mutation_rate);
    g->conviction_threshold = mutate_param(g->conviction_threshold, PARAM_RANGES[1].min, PARAM_RANGES[1].max, PARAM_RANGES[1].mut_scale, mutation_rate);
    g->risk_tolerance      = mutate_param(g->risk_tolerance,      PARAM_RANGES[2].min,  PARAM_RANGES[2].max,  PARAM_RANGES[2].mut_scale,  mutation_rate);
    g->lie_sensitivity     = mutate_param(g->lie_sensitivity,     PARAM_RANGES[3].min,  PARAM_RANGES[3].max,  PARAM_RANGES[3].mut_scale,  mutation_rate);
    g->herd_antipathy      = mutate_param(g->herd_antipathy,      PARAM_RANGES[4].min,  PARAM_RANGES[4].max,  PARAM_RANGES[4].mut_scale,  mutation_rate);
    g->stop_loss_pct       = mutate_param(g->stop_loss_pct,       PARAM_RANGES[5].min,  PARAM_RANGES[5].max,  PARAM_RANGES[5].mut_scale,  mutation_rate);
    g->take_profit_pct     = mutate_param(g->take_profit_pct,     PARAM_RANGES[6].min,  PARAM_RANGES[6].max,  PARAM_RANGES[6].mut_scale,  mutation_rate);
    g->min_edge_pct        = mutate_param(g->min_edge_pct,        PARAM_RANGES[7].min,  PARAM_RANGES[7].max,  PARAM_RANGES[7].mut_scale,  mutation_rate);
    g->time_horizon        = mutate_param(g->time_horizon,        PARAM_RANGES[8].min,  PARAM_RANGES[8].max,  PARAM_RANGES[8].mut_scale,  mutation_rate);
    g->mean_reversion_bias = mutate_param(g->mean_reversion_bias, PARAM_RANGES[9].min,  PARAM_RANGES[9].max,  PARAM_RANGES[9].mut_scale,  mutation_rate);
    // ── v2: Mutate learned feature weights ──
    float w_scale = 0.15f;  // Weight mutation magnitude
    for (int i = 0; i < N_FEATURES; i++) {
        g->feat_weight[i] = mutate_param(g->feat_weight[i], -1.0f, 1.0f, w_scale, mutation_rate);
    }
    g->bias = mutate_param(g->bias, -1.0f, 1.0f, 0.10f, mutation_rate);
    g->learning_rate = mutate_param(g->learning_rate, 0.001f, 0.1f, 0.02f, mutation_rate);
    // ── P22: Mutate regime-specific weights for all 3 regimes ──
    for (int r = 0; r < N_REGS; r++) {
        for (int i = 0; i < N_FEATURES; i++) {
            g->regime_weight[r][i] = mutate_param(g->regime_weight[r][i], -1.0f, 1.0f, w_scale, mutation_rate);
        }
        g->regime_bias[r] = mutate_param(g->regime_bias[r], -1.0f, 1.0f, 0.10f, mutation_rate);
    }
}

// ── Copy genome ──
static void copy_genome(Genome *dst, const Genome *src) {
    memcpy(dst, src, sizeof(Genome));
}

// ── Agent comparison for sorting (descending by win_rate_ema) ──
static int cmp_agents_desc(const void *a, const void *b) {
    const AgentState *aa = (const AgentState *)a;
    const AgentState *bb = (const AgentState *)b;
    // Dead agents go to bottom
    if (aa->alive && !bb->alive) return -1;
    if (!aa->alive && bb->alive) return 1;
    if (!aa->alive && !bb->alive) return 0;
    // Sort by win_rate_ema descending
    if (aa->win_rate_ema > bb->win_rate_ema) return -1;
    if (aa->win_rate_ema < bb->win_rate_ema) return 1;
    return 0;
}

// ════════════════════════════════════════════════════════
//  EVOLVE — Per-market-type: cull bottom 10%, clone+mutate top 10%
// ════════════════════════════════════════════════════════
RoomError room_darwin_evolve(AgentState *agents, int n, int cycle, DarwinRecord *rec, const int *agent_market) {
    if (n < 100) return ERR_NO_AGENTS;
    
    rec->epoch = cycle / 100; // Every 100 trades = 1 epoch
    rec->mutation_rate = fmaxf(0.05f, 0.3f - rec->epoch * 0.01f); // Decays
    rec->culled = 0;
    rec->cloned = 0;
    
    // Count market types present
    int mt_counts[N_MARKET_TYPES];
    memset(mt_counts, 0, sizeof(mt_counts));
    for (int i = 0; i < n; i++) {
        if (agents[i].alive) {
            int mt = (agent_market && agent_market[i] < N_MARKET_TYPES) ? agent_market[i] : 0;
            mt_counts[mt]++;
        }
    }
    
    // Cull bottom 10% per market type
    for (int mt = 0; mt < N_MARKET_TYPES; mt++) {
        if (mt_counts[mt] < 10) continue;  // Skip tiny groups
        
        // Collect alive agents of this market type
        int nmt = mt_counts[mt];
        AgentState *mt_agents = (AgentState *)malloc(nmt * sizeof(AgentState));
        if (!mt_agents) continue;
        int *mt_idx = (int *)malloc(nmt * sizeof(int));  // Map back to original indices
        if (!mt_idx) { free(mt_agents); continue; }
        
        int idx = 0;
        for (int i = 0; i < n; i++) {
            if (agents[i].alive) {
                int amt = (agent_market && agent_market[i] < N_MARKET_TYPES) ? agent_market[i] : 0;
                if (amt == mt) {
                    mt_agents[idx] = agents[i];
                    mt_idx[idx] = i;
                    idx++;
                }
            }
        }
        
        // Sort by win_rate_ema descending
        qsort(mt_agents, nmt, sizeof(AgentState), cmp_agents_desc);
        
        // Cull bottom 10% of this market type
        int cull_count = nmt / 10;
        if (cull_count < 1) cull_count = 1;
        
        float redistribution_pool = 0;
        int culled = 0;
        for (int i = nmt - 1; i >= 0 && culled < cull_count; i--) {
            if (!mt_agents[i].alive) continue;
            redistribution_pool += mt_agents[i].capital;
            mt_agents[i].alive = false;
            culled++;
            rec->culled++;
        }
        
        // Clone top 10% within same market type to fill culled slots
        int clone_from = nmt / 10;
        if (clone_from < 1) clone_from = 1;
        float clone_capital = culled > 0 ? redistribution_pool / culled : 1.0f;
        
        int cloned = 0;
        for (int i = nmt - 1; i >= 0 && cloned < culled; i--) {
            if (mt_agents[i].alive) continue;
            int src = rand() % clone_from;
            copy_genome(&mt_agents[i].genome, &mt_agents[src].genome);
            mutate_genome(&mt_agents[i].genome, rec->mutation_rate);
            mt_agents[i].alive = true;
            mt_agents[i].capital = clone_capital;
            mt_agents[i].starting_capital = mt_agents[i].capital;
            mt_agents[i].trades = 0;
            mt_agents[i].wins = 0;
            mt_agents[i].losses = 0;
            mt_agents[i].total_pnl = 0;
            mt_agents[i].max_drawdown = 0;
            mt_agents[i].peak_capital = mt_agents[i].capital;
            mt_agents[i].consecutive_losses = 0;
            mt_agents[i].win_rate_ema = 0.5f;
            mt_agents[i].last_trade_window = -1;
            cloned++;
            rec->cloned++;
        }
        
        // Write back to original indices
        for (int i = 0; i < nmt; i++) {
            agents[mt_idx[i]] = mt_agents[i];
        }
        
        free(mt_idx);
        free(mt_agents);
    }
    
    // ── Repopulation: if too many dead overall ──
    int alive = 0;
    for (int i = 0; i < n; i++) {
        if (agents[i].alive) alive++;
    }
    if (alive < 100) {
        float dead_pool = 0;
        int dead_count = 0;
        for (int i = 0; i < n; i++) {
            if (!agents[i].alive) { dead_pool += agents[i].capital; dead_count++; }
        }
        float repop_cap = dead_count > 0 ? dead_pool / dead_count : 1.0f;
        
        int repop = 0;
        for (int i = 0; i < n && repop < alive; i++) {
            if (!agents[i].alive) {
                int src = rand() % alive;
                int si = 0;
                for (int j = 0; j < n; j++) {
                    if (agents[j].alive) {
                        if (si == src) {
                            // Clone from same market type when possible
                            int our_mt = (agent_market && agent_market[i] < N_MARKET_TYPES) ? agent_market[i] : 0;
                            int src_mt = (agent_market && agent_market[j] < N_MARKET_TYPES) ? agent_market[j] : 0;
                            copy_genome(&agents[i].genome, &agents[j].genome);
                            // If cross-type, mutate more to adapt
                            if (our_mt != src_mt) {
                                mutate_genome(&agents[i].genome, rec->mutation_rate * 3.0f);
                            } else {
                                mutate_genome(&agents[i].genome, rec->mutation_rate * 2.0f);
                            }
                            agents[i].alive = true;
                            agents[i].capital = repop_cap;
                            agents[i].starting_capital = agents[i].capital;
                            agents[i].trades = 0;
                            agents[i].wins = 0;
                            agents[i].losses = 0;
                            agents[i].total_pnl = 0;
                            agents[i].max_drawdown = 0;
                            agents[i].peak_capital = agents[i].capital;
                            agents[i].consecutive_losses = 0;
                            agents[i].win_rate_ema = 0.5f;
                            agents[i].last_trade_window = -1;
                            break;
                        }
                        si++;
                    }
                }
                repop++;
            }
        }
    }
    
    return ERR_OK;
}

// ── C19: Compute population weight diversity metrics ──
// Updates RoomStats with weight_diversity and genome_diversity.
// Should be called after Darwin evolve (or periodically).
RoomError room_darwin_compute_diversity(const AgentState *agents, int n, RoomStats *stats) {
    if (n < 2) return ERR_NO_AGENTS;
    
    // 1. Compute L2 norm of feat_weight per agent (weight_mag)
    float mag_sum = 0, mag_sq_sum = 0;
    int mag_count = 0;
    
    for (int i = 0; i < n; i++) {
        if (!agents[i].alive) continue;
        float mag = 0;
        for (int r = 0; r < N_REGS; r++) {
            for (int f = 0; f < N_FEATURES; f++) {
                mag += agents[i].genome.regime_weight[r][f] * agents[i].genome.regime_weight[r][f];
            }
        }
        mag = sqrtf(mag / N_REGS);  // Average across regimes
        mag_sum += mag;
        mag_sq_sum += mag * mag;
        mag_count++;
    }
    
    if (mag_count > 1) {
        float mean = mag_sum / mag_count;
        float variance = mag_sq_sum / mag_count - mean * mean;
        stats->weight_diversity = variance > 0 ? sqrtf(variance) : 0;
    } else {
        stats->weight_diversity = 0;
    }
    
    // 2. Genome diversity: mean pairwise distance in genome parameter space
    // Sample up to 100 agents for performance
    int sample = mag_count < 100 ? mag_count : 100;
    int step = mag_count / sample;
    float total_dist = 0;
    int pairs = 0;
    
    for (int i = 0; i < n && pairs < 500; i += step) {
        if (!agents[i].alive) continue;
        for (int j = i + step; j < n && pairs < 500; j += step) {
            if (!agents[j].alive) continue;
            
            float dist = 0;
            dist += fabsf(agents[i].genome.position_size - agents[j].genome.position_size);
            dist += fabsf(agents[i].genome.conviction_threshold - agents[j].genome.conviction_threshold);
            dist += fabsf(agents[i].genome.risk_tolerance - agents[j].genome.risk_tolerance);
            dist += fabsf(agents[i].genome.lie_sensitivity - agents[j].genome.lie_sensitivity);
            dist += fabsf(agents[i].genome.herd_antipathy - agents[j].genome.herd_antipathy);
            dist += fabsf(agents[i].genome.time_horizon - agents[j].genome.time_horizon);
            dist += fabsf(agents[i].genome.mean_reversion_bias - agents[j].genome.mean_reversion_bias);
            
            total_dist += dist;
            pairs++;
        }
    }
    
    stats->genome_diversity = pairs > 0 ? total_dist / pairs : 0;
    
    return ERR_OK;
}
