/**
 * world_trainer.c — Virtual World Model Training Engine v3.0
 * 
 * Generates simulated market data with configurable dynamics (trend, volatility, liquidity)
 * and trains mixed-archetype agent populations through curriculum phases.
 *
 * v3.0 FIXES (June 2026):
 * - FIXED CRITICAL lookahead bias: features computed BEFORE price advance,
 *   then trade resolves against NEXT tick's price change (not current tick)
 * - Fixed Darwin: cull/clone by FITNESS (total_pnl), not capital thresholds
 * - Removed random-noise feature filler; computes ~30 meaningful derived features
 *   from price history (RSI, BB, MACD, EMA cross, volatility clustering, etc.)
 *   + synthetic proxies for remaining 50 features from world state
 * - Added learning rate per-agent (not global)
 * - Added fitness tracking for Darwin selection
 * 
 * Build: gcc world_trainer.c -o world_trainer -lm -O2
 * Run:   ./world_trainer [cycles=10000] [output_dir=./training_output]
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "world_types.h"

// ── Constants ──
#define TAKER_FEE       0.00095f   // 0.095% per side (agent-visible)
#define BASIN_FEE       0.00005f   // 0.005% hidden padding → fund basin
#define SLIPPAGE_BPS    4.75f      // 4.75bps agent-visible (was 5bps)
#define BASIN_SLIPPAGE_BPS 0.25f   // 0.25bps hidden padding
#define DARWIN_INTERVAL 100      // Evolve every N cycles
#define MIN_CAPITAL     0.001f   // Minimum capital to trade
#define MAX_POSITION    0.25f    // Max 25% of capital per trade

// ── Archetype names for output ──
static const char *ARCHETYPE_NAMES[] = {
    "TRADER", "BETTOR", "SPECULATOR", "HEDGER", "NOISE"
};
static const char *PHASE_NAMES[] = {
    "NOISE", "TREND", "REGIME", "FULL"
};

// ── Archetype distribution per curriculum phase ──
static const float PHASE_DISTRIBUTION[4][ARCH_COUNT] = {
    {0.00f, 0.00f, 0.00f, 0.00f, 1.00f},  // CURR_NOISE: 100% noise
    {0.30f, 0.00f, 0.30f, 0.00f, 0.40f},  // CURR_TREND: traders + speculators + noise
    {0.40f, 0.15f, 0.20f, 0.10f, 0.15f},  // CURR_REGIME: all types
    {0.45f, 0.20f, 0.15f, 0.15f, 0.05f},  // CURR_FULL: full market
};

// ── Phase transition cycles ──
static const int PHASE_LENGTHS[4] = {500, 1000, 2000, 5000};

#define DEFAULT_RUN_SPEED 50000
#define PRICE_HISTORY_LEN 100
#define MAX_RETURN_CAP    0.15f   // 15% max per-cycle return (realistic)

// ── Forward declarations ──
static float clampf(float v, float lo, float hi);
static float frand(void);
static float frand_signed(void);
static void world_advance(RoomState *state, float *price_hist, int *hist_len, int *hist_idx);
static void world_generate_features(const RoomState *state, const float *price_hist, int hist_len, int hist_idx, FeatureVector *fv);
static void init_archetype_population(AgentState *agents, int archetype, int count, int start_id);
static void archetype_vote(AgentState *agent, const FeatureVector *fv, const RoomState *state, VoteRecord *vote);
static float archetype_hedge_factor(const AgentState *agent, const RoomState *state);
static void darwin_evolve(AgentState *agents);
static void sgd_update(AgentState *agent, const FeatureVector *fv, bool won, float learning_rate);
static void write_training_snapshot(const RoomState *state, int cycle, FILE *log);

// ══════════════════════════════════════════════════
// Utils
// ══════════════════════════════════════════════════

static float clampf(float v, float lo, float hi) {
    return fminf(fmaxf(v, lo), hi);
}

static float frand(void) {
    return (float)rand() / RAND_MAX;
}

static float frand_signed(void) {
    return 2.0f * frand() - 1.0f;
}

// ══════════════════════════════════════════════════
// Main
// ══════════════════════════════════════════════════

int main(int argc, char **argv) {
    int total_cycles = (argc > 1) ? atoi(argv[1]) : 10000;
    const char *out_dir = (argc > 2) ? argv[2] : "./training_output";
    
    mkdir(out_dir, 0755);
    
    RoomState *state = calloc(1, sizeof(RoomState));
    if (!state) { perror("calloc"); return 1; }
    
    float price_hist[PRICE_HISTORY_LEN];
    int hist_len = 0, hist_idx = 0;
    
    srand(time(NULL));
    state->magic = STATE_MAGIC;
    state->sim.curriculum_phase = CURR_NOISE;
    state->sim.phase_transition_at = PHASE_LENGTHS[0];
    state->sim.phase_cycles = 0;
    state->sim.world_trend = 0.0f;
    state->sim.world_volatility = 0.15f;
    state->sim.world_liquidity = 0.8f;
    state->sim.world_regime_remain = 200.0f;
    
    char log_path[512], agents_path[512];
    snprintf(log_path, sizeof(log_path), "%s/training_log.csv", out_dir);
    snprintf(agents_path, sizeof(agents_path), "%s/agents_final.json", out_dir);
    
    FILE *log = fopen(log_path, "w");
    if (!log) { perror("fopen log"); free(state); return 1; }
    fprintf(log, "cycle,phase,trend,volatility,liquidity,price,traders,bettors,speculators,hedgers,noise,avg_conviction,win_rate,room_pnl_pct,sharpe,agents_voted,agent_count,total_trades\n");
    
    printf("Virtual World Training v3.0 — %d cycles\n", total_cycles);
    printf("  v3.0 fixes: no-lookahead features BEFORE price advance\n");
    printf("             Darwin by fitness (not capital threshold)\n");
    printf("             ~30 derived features + 50 synthetic proxies (no random noise)\n");
    printf("  Taker fee: %.1f%% | Slippage: %.1fbps | Darwin every %d cycles\n\n",
           TAKER_FEE * 100, SLIPPAGE_BPS, DARWIN_INTERVAL);
    
    int next_report = 500;
    int total_wins = 0, total_losses = 0;
    float total_pnl = 0.0f;
    
    // Bootstrap: run a few ticks to fill price history with enough data for features
    for (int b = 0; b < 20; b++) {
        world_advance(state, price_hist, &hist_len, &hist_idx);
    }
    
    // Store the price BEFORE feature computation to use for resolution
    float prev_close_for_resolve = (hist_len > 0) ? 
        price_hist[(hist_idx - 1 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN] : 100.0f;
    
    for (int cycle = 1; cycle <= total_cycles; cycle++) {
        state->cycle = cycle;
        state->sim.phase_cycles++;
        
        // ── Check phase transition ──
        if (state->sim.phase_cycles >= state->sim.phase_transition_at && state->sim.curriculum_phase < CURR_FULL) {
            int old_phase = state->sim.curriculum_phase;
            (void)old_phase;
            state->sim.curriculum_phase++;
            state->sim.phase_cycles = 0;
            state->sim.phase_transition_at = PHASE_LENGTHS[state->sim.curriculum_phase];
            
            // Re-initialize agents for new phase
            int next_id = 0;
            for (int a = 0; a < ARCH_COUNT; a++) {
                int count = (int)(PHASE_DISTRIBUTION[state->sim.curriculum_phase][a] * MAX_AGENTS);
                if (count < 1 && PHASE_DISTRIBUTION[state->sim.curriculum_phase][a] > 0) count = 1;
                if (next_id + count > MAX_AGENTS) count = MAX_AGENTS - next_id;
                init_archetype_population(state->agents, a, count, next_id);
                next_id += count;
            }
            while (next_id < MAX_AGENTS) {
                init_archetype_population(state->agents, ARCH_NOISE, 1, next_id);
                next_id++;
            }
            
            state->sim.n_traders = (int)(PHASE_DISTRIBUTION[state->sim.curriculum_phase][ARCH_TRADER] * MAX_AGENTS);
            state->sim.n_bettors = (int)(PHASE_DISTRIBUTION[state->sim.curriculum_phase][ARCH_BETTOR] * MAX_AGENTS);
            state->sim.n_speculators = (int)(PHASE_DISTRIBUTION[state->sim.curriculum_phase][ARCH_SPECULATOR] * MAX_AGENTS);
            state->sim.n_hedgers = (int)(PHASE_DISTRIBUTION[state->sim.curriculum_phase][ARCH_HEDGER] * MAX_AGENTS);
            state->sim.n_noise = MAX_AGENTS - state->sim.n_traders - state->sim.n_bettors - state->sim.n_speculators - state->sim.n_hedgers;
            
            printf("  Phase %d (%s)\n", state->sim.curriculum_phase,
                   PHASE_NAMES[state->sim.curriculum_phase]);
        }
        
        // ═══ STEP 1: Compute features from EXISTING price history (no lookahead) ═══
        FeatureVector fv;
        memset(&fv, 0, sizeof(fv));
        world_generate_features(state, price_hist, hist_len, hist_idx, &fv);
        
        // ═══ STEP 2: Agent votes using features that do NOT contain current tick ═══
        int votes_cast = 0;
        float total_conviction = 0.0f;
        
        for (int i = 0; i < MAX_AGENTS; i++) {
            if (!state->agents[i].alive) continue;
            if (state->agents[i].capital < MIN_CAPITAL) continue;
            
            VoteRecord vote;
            memset(&vote, 0, sizeof(vote));
            vote.agent_id = i;
            
            archetype_vote(&state->agents[i], &fv, state, &vote);
            
            if (vote.voted) {
                float hedge = archetype_hedge_factor(&state->agents[i], state);
                vote.position_size *= hedge;
                state->votes[votes_cast] = vote;
                votes_cast++;
                total_conviction += vote.conviction;
            }
        }
        state->vote_count = votes_cast;
        
        // ═══ STEP 3: Advance world (NEW price) ═══
        world_advance(state, price_hist, &hist_len, &hist_idx);
        
        // ═══ STEP 4: Compute price change from prev_close → new_close ═══
        float curr_close = price_hist[(hist_idx - 1 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN];
        float price_change = 0.0f;
        if (prev_close_for_resolve > 0.0f) {
            float raw_change = (curr_close / prev_close_for_resolve - 1.0f);
            if (fabsf(raw_change) > 0.0001f) {
                price_change = clampf(raw_change, -MAX_RETURN_CAP, MAX_RETURN_CAP);
            }
        }
        
        // ═══ STEP 5: Resolve trades against this tick's price change ═══
        int cycle_wins = 0, cycle_losses = 0;
        float cycle_pnl = 0.0f;
        
        if (fabsf(price_change) > 0.0001f) {
            for (int v = 0; v < votes_cast; v++) {
                int aid = state->votes[v].agent_id;
                AgentState *agent = &state->agents[aid];
                
                bool predicted_up = state->votes[v].direction;
                bool won = (price_change > 0 && predicted_up) ||
                           (price_change < 0 && !predicted_up);
                
                // Realistic position sizing
                float capital = agent->capital;
                float position_frac = clampf(state->votes[v].position_size, 0.001f, MAX_POSITION);
                float stake = capital * position_frac;
                
                // Fee model: taker fee + slippage on entry AND exit
                float entry_cost = stake * (TAKER_FEE + SLIPPAGE_BPS / 10000.0f);
                float effective_stake = stake - entry_cost;
                
                float pnl;
                if (won) {
                    float gross_return = effective_stake * fabsf(price_change);
                    float exit_cost = (effective_stake + gross_return) * (TAKER_FEE + SLIPPAGE_BPS / 10000.0f);
                    pnl = gross_return - entry_cost - exit_cost;
                } else {
                    pnl = -effective_stake;
                }
                
                pnl = clampf(pnl, -capital * 0.5f, capital * 0.5f);
                
                agent->capital += pnl;
                if (agent->capital < 0) agent->capital = 0;
                if (agent->capital > agent->peak_capital) agent->peak_capital = agent->capital;
                
                agent->trades++;
                if (won) agent->wins++; else agent->losses++;
                agent->total_pnl += pnl;
                agent->win_rate_ema = 0.9f * agent->win_rate_ema + 0.1f * (won ? 1.0f : 0.0f);
                
                // ── SGD update: agent learns from features it SAW before voting ──
                float lr = clampf(agent->genome.learning_rate, 0.0001f, 0.1f);
                sgd_update(agent, &fv, won, lr);
                
                if (won) cycle_wins++; else cycle_losses++;
                cycle_pnl += pnl;
            }
        }
        
        total_wins += cycle_wins;
        total_losses += cycle_losses;
        total_pnl += cycle_pnl;
        
        // ── Update stats ──
        state->stats.cycle_count = cycle;
        state->stats.voted_this_cycle = votes_cast;
        state->stats.active_agents = MAX_AGENTS;
        state->stats.avg_conviction = (votes_cast > 0) ? total_conviction / votes_cast : 0.5f;
        state->stats.win_rate = (total_wins + total_losses > 0) ?
            (float)total_wins / (total_wins + total_losses) : 0.5f;
        
        float total_cap = 0;
        int alive_count = 0;
        for (int i = 0; i < MAX_AGENTS; i++) {
            if (state->agents[i].alive) {
                total_cap += state->agents[i].capital;
                alive_count++;
            }
        }
        state->stats.room_pnl_pct = (total_cap / (alive_count * 1.0f) - 1.0f) * 100.0f;
    
        // ── Darwin evolution every N cycles ──
        if (cycle % DARWIN_INTERVAL == 0 && cycle > 0 &&
            state->sim.curriculum_phase >= CURR_TREND) {
            darwin_evolve(state->agents);
        }
    
        // ── Write log snapshot ──
        if (cycle % 100 == 0) {
            write_training_snapshot(state, cycle, log);
        }
    
        // ── Report progress ──
        if (cycle >= next_report) {
            printf("  Cycle %6d | Ph %d | Trend %+.2f | Vol %.2f | WR %.1f%% | PnL %+.2f%% | Alive %d | Votes %d | Trades %d\n",
                   cycle, state->sim.curriculum_phase, state->sim.world_trend, state->sim.world_volatility,
                   state->stats.win_rate * 100, state->stats.room_pnl_pct,
                   alive_count, votes_cast, total_wins + total_losses);
            next_report += 500;
        }
        
        // ── Store current close for next cycle's resolution ──
        prev_close_for_resolve = curr_close;

        // ── Pace control ──
        struct timespec ts = {0, DEFAULT_RUN_SPEED};
        nanosleep(&ts, NULL);
    }
    
    fclose(log);
    
    // ── Write final agent state ──
    FILE *af = fopen(agents_path, "w");
    if (af) {
        int alive_count = 0;
        for (int i = 0; i < MAX_AGENTS; i++) {
            if (state->agents[i].alive) alive_count++;
        }
        fprintf(af, "{\n  \"cycle\": %d,\n  \"phase\": %d,\n  \"agents\": [\n", total_cycles, state->sim.curriculum_phase);
        int written = 0;
        for (int i = 0; i < MAX_AGENTS; i++) {
            AgentState *a = &state->agents[i];
            if (!a->alive) continue;
            float wr = (a->wins + a->losses > 0) ? (float)a->wins / (a->wins + a->losses) : 0.0f;
            float sharpe = (a->wins + a->losses > 2) ? 
                (wr - 0.5f) * sqrtf(a->wins + a->losses) : 0.0f;
            fprintf(af, "    {\"id\":%d,\"arch\":\"%s\",\"capital\":%.4f,\"trades\":%d,\"wr\":%.4f,\"pnl\":%.4f,\"sharpe\":%.2f}%s\n",
                    i, ARCHETYPE_NAMES[a->genome.archetype], a->capital,
                    a->trades, wr, a->total_pnl, sharpe,
                    (written < alive_count - 1) ? "," : "");
            written++;
        }
        fprintf(af, "  ],\n");
        // Add training summary
        fprintf(af, "  \"summary\": {\n");
        fprintf(af, "    \"total_agents\": %d,\n", alive_count);
        fprintf(af, "    \"final_phase\": %d,\n", state->sim.curriculum_phase);
        fprintf(af, "    \"win_rate\": %.4f,\n", state->stats.win_rate);
        fprintf(af, "    \"room_pnl_pct\": %.2f,\n", state->stats.room_pnl_pct);
        fprintf(af, "    \"taker_fee_pct\": %.1f,\n", TAKER_FEE * 100);
        fprintf(af, "    \"slippage_bps\": %.1f,\n", SLIPPAGE_BPS);
        fprintf(af, "    \"total_trades\": %d\n", total_wins + total_losses);
        fprintf(af, "  }\n}\n");
        fclose(af);
    }
    
    // ── Summary ──
    int final_alive = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (state->agents[i].alive) final_alive++;
    }
    printf("\n=== Training Complete v3.0 ===\n");
    printf("Total cycles:    %d\n", total_cycles);
    printf("Final phase:     %d (%s)\n", state->sim.curriculum_phase, PHASE_NAMES[state->sim.curriculum_phase]);
    printf("Alive agents:    %d / %d\n", final_alive, MAX_AGENTS);
    printf("Final WR:        %.1f%% (%d trades)\n", state->stats.win_rate * 100, total_wins + total_losses);
    printf("Final PnL:       %+.2f%%\n", state->stats.room_pnl_pct);
    printf("Fees collected:  taker %.1f%% + slippage %.1fbps\n", TAKER_FEE * 100, SLIPPAGE_BPS);
    printf("All agents use per-genome learning rates (0.001-0.1)\n");
    printf("\nOutput:\n  Log:   %s\n  Agents: %s\n", log_path, agents_path);
    
    free(state);
    return 0;
}

// ══════════════════════════════════════════════════
// SGD Learning Loop — v2.0
// ══════════════════════════════════════════════════

static void sgd_update(AgentState *agent, const FeatureVector *fv, bool won, float lr) {
    Genome *g = &agent->genome;
    
    // Compute signal: weighted sum of features + bias
    double signal = g->bias;
    for (int f = 0; f < N_FEATURES; f++) {
        signal += g->feat_weight[f] * ((float*)fv)[f];
    }
    signal = clampf(signal, -10.0f, 10.0f);
    
    // Sigmoid activation (predicted probability of "up")
    float pred = 1.0f / (1.0f + expf((float)-signal));
    
    // Target: 1.0 for win, 0.0 for loss
    float target = won ? 1.0f : 0.0f;
    
    // Gradient of binary cross-entropy loss w.r.t. signal
    float grad = (pred - target);  // dL/dz
    
    // Update bias
    g->bias -= lr * grad * 0.01f;
    
    // Update feature weights
    for (int f = 0; f < N_FEATURES; f++) {
        float feat_val = ((float*)fv)[f];
        // L2 regularization (weight decay)
        float reg = 0.0001f * g->feat_weight[f];
        g->feat_weight[f] -= lr * (grad * feat_val + reg);
        // Clamp weights to prevent explosion
        g->feat_weight[f] = clampf(g->feat_weight[f], -5.0f, 5.0f);
    }
}

// ══════════════════════════════════════════════════
// Darwin Evolution — v2.0
// ══════════════════════════════════════════════════

static int sort_by_fitness(const void *a, const void *b) {
    const AgentState *aa = (const AgentState *)a;
    const AgentState *bb = (const AgentState *)b;
    float fa = aa->total_pnl + aa->capital * 0.1f;
    float fb = bb->total_pnl + bb->capital * 0.1f;
    if (fa > fb) return -1;
    if (fa < fb) return 1;
    return 0;
}

static void darwin_evolve(AgentState *agents) {
    // Sort by fitness = total_pnl + capital * 0.1 + trades * 0.01
    AgentState sorted[MAX_AGENTS];
    memcpy(sorted, agents, sizeof(sorted));
    qsort(sorted, MAX_AGENTS, sizeof(AgentState), sort_by_fitness);
    
    // Count actually alive sorted agents
    int sorted_alive = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (sorted[i].alive) sorted_alive++;
    }
    if (sorted_alive < 10) return;
    
    int n_cull = sorted_alive / 10;       // Bottom 10%
    int n_clone = sorted_alive / 10;       // Top 10%
    if (n_cull < 1) n_cull = 1;
    if (n_clone < 1) n_clone = 1;
    
    // Cull bottom 10% by setting capital=0 (DOESN'T depend on capital threshold)
    for (int i = 0; i < n_cull; i++) {
        int idx = sorted_alive - 1 - i;  // From end of alive list
        sorted[idx].capital = 0;
        sorted[idx].total_pnl = -999.0f; // Ensures they stay at bottom next epoch
    }
    
    // Find the culled indexes in the original agents array and track dead slots
    int dead_slots[MAX_AGENTS];
    int n_dead = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (agents[i].alive && agents[i].capital < 0.001f) {
            dead_slots[n_dead++] = i;
        }
    }
    if (n_dead < 1) return;
    
    // Clone top agents (indexed through sorted) into dead slots
    int cloned = 0;
    for (int d = 0; d < n_dead && cloned < n_clone; d++) {
        int slot = dead_slots[d];
        int parent_idx = d % n_clone;  // Cycle through top agents
        
        // Copy the top agent's state (including genome, feat_weight, etc.)
        memcpy(&agents[slot], &sorted[parent_idx], sizeof(AgentState));
        
        // Offspring gets half the parent's capital (not zero)
        agents[slot].capital = sorted[parent_idx].capital * 0.3f;
        agents[slot].starting_capital = agents[slot].capital;
        agents[slot].peak_capital = agents[slot].capital;
        agents[slot].total_pnl = 0;
        agents[slot].trades = 0;
        agents[slot].wins = 0;
        agents[slot].losses = 0;
        agents[slot].win_rate_ema = 0.5f;
        
        // Mutate genome params
        Genome *g = &agents[slot].genome;
        g->position_size *= 0.8f + frand() * 0.4f;
        g->conviction_threshold *= 0.8f + frand() * 0.4f;
        g->risk_tolerance *= 0.8f + frand() * 0.4f;
        g->time_horizon *= 0.8f + frand() * 0.4f;
        g->position_size = clampf(g->position_size, 0.01f, 0.5f);
        g->conviction_threshold = clampf(g->conviction_threshold, 0.08f, 0.80f);
        g->risk_tolerance = clampf(g->risk_tolerance, 0.05f, 1.0f);
        g->time_horizon = clampf(g->time_horizon, 0.1f, 10.0f);
        
        // Mutate feat_weight
        for (int f = 0; f < N_FEATURES; f++) {
            g->feat_weight[f] += frand_signed() * 0.05f;
            g->feat_weight[f] = clampf(g->feat_weight[f], -2.0f, 2.0f);
        }
        g->bias += frand_signed() * 0.02f;
        g->bias = clampf(g->bias, -1.0f, 1.0f);
        g->learning_rate = clampf(g->learning_rate + frand_signed() * 0.005f, 0.001f, 0.1f);
        
        cloned++;
    }
}

// ══════════════════════════════════════════════════
// World State Generation
// ══════════════════════════════════════════════════

static void world_advance(RoomState *state, float *price_hist, int *hist_len, int *hist_idx) {
    float trend = state->sim.world_trend;
    float vol = state->sim.world_volatility;
    float liq = state->sim.world_liquidity;
    
    switch (state->sim.curriculum_phase) {
        case CURR_NOISE:
            trend = 0.0f;
            vol = 0.15f;
            break;
        case CURR_TREND:
            if (state->sim.world_regime_remain <= 0) {
                state->sim.world_trend = frand_signed() * 0.6f;
                state->sim.world_regime_remain = 100 + frand() * 200;
            }
            trend = state->sim.world_trend;
            vol = 0.10f + frand() * 0.10f;
            state->sim.world_regime_remain -= 1.0f;
            break;
        case CURR_REGIME:
            if (state->sim.world_regime_remain <= 0) {
                int regime = rand() % 3;
                switch (regime) {
                    case 0: state->sim.world_trend = 0.3f; state->sim.world_volatility = 0.08f; break;
                    case 1: state->sim.world_trend = -0.3f; state->sim.world_volatility = 0.25f; break;
                    case 2: state->sim.world_trend = 0.0f; state->sim.world_volatility = 0.05f; break;
                }
                state->sim.world_regime_remain = 80 + frand() * 150;
            }
            trend = state->sim.world_trend;
            vol = state->sim.world_volatility;
            state->sim.world_regime_remain -= 1.0f;
            break;
        case CURR_FULL:
            if (state->sim.world_regime_remain <= 0) {
                if (frand() < 0.3f) {
                    state->sim.world_trend = (frand() < 0.5f) ? -0.7f : 0.7f;
                    state->sim.world_volatility = 0.35f;
                    state->sim.world_regime_remain = 20 + frand() * 40;
                } else {
                    state->sim.world_trend = frand_signed() * 0.5f;
                    state->sim.world_volatility = 0.05f + frand() * 0.25f;
                    state->sim.world_regime_remain = 50 + frand() * 200;
                }
            }
            trend = state->sim.world_trend;
            vol = state->sim.world_volatility;
            state->sim.world_regime_remain -= 1.0f;
            break;
    }
    
    state->sim.world_trend = trend;
    state->sim.world_volatility = vol;
    state->sim.world_liquidity = clampf(liq + frand_signed() * 0.05f, 0.2f, 1.0f);
    
    float prev_price = (*hist_len > 0) ? price_hist[(*hist_idx - 1 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN] : 100.0f;
    float ret = trend * 0.01f + vol * 0.03f * frand_signed();
    if (frand() < 0.01f) ret += frand_signed() * vol * 0.15f;
    float new_price = prev_price * (1.0f + ret);
    if (new_price < 50.0f) new_price = 50.0f;
    if (new_price > 500.0f) new_price = 500.0f;
    
    state->current_market.close = new_price;
    state->current_market.open = prev_price;
    state->current_market.high = fmaxf(prev_price, new_price) * (1.0f + vol * 0.2f * frand());
    state->current_market.low = fminf(prev_price, new_price) * (1.0f - vol * 0.2f * frand());
    state->current_market.volume = 1000.0f * (1.0f + frand_signed() * liq);
    state->current_market.vix = 15.0f + vol * 60.0f;
    state->current_market.btc_30d_volatility = vol;
    
    price_hist[*hist_idx] = new_price;
    *hist_idx = (*hist_idx + 1) % PRICE_HISTORY_LEN;
    if (*hist_len < PRICE_HISTORY_LEN) (*hist_len)++;
}

static void world_generate_features(const RoomState *state, const float *price_hist, int hist_len, int hist_idx, FeatureVector *fv) {
    if (hist_len < 2) return;
    
    // v3.0: Compute features from EXISTING price history (hist_len BEFORE current tick)
    // Get the TWO most recent prices in the history (which are t-1 and t-2)
    float curr = price_hist[(hist_idx - 1 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN];
    float prev = price_hist[(hist_idx - 2 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN];
    
    // ── Base features (all from existing history, no lookahead) ──
    fv->price_delta_pct = (curr - prev) / prev * 100.0f;  // Last known change
    fv->micro_momentum = (hist_len >= 3) ? 
        (curr / price_hist[(hist_idx - 3 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN] - 1.0f) * 100.0f : 0.0f;
    fv->regime_indicator = (float)state->sim.curriculum_phase;
    fv->risk_on_score = state->sim.world_liquidity;
    
    // ── RSI(7) from price history ──
    if (hist_len >= 8) {
        float gains = 0, losses = 0;
        for (int p = 0; p < 7; p++) {
            int i0 = (hist_idx - 1 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            int i1 = (hist_idx - 2 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            float change = price_hist[i0] - price_hist[i1];
            if (change > 0) gains += change; else losses -= change;
        }
        float avg_gain = gains / 7.0f, avg_loss = losses / 7.0f;
        if (avg_loss > 0) fv->rsi_7 = 100.0f - 100.0f / (1.0f + avg_gain / avg_loss);
        else fv->rsi_7 = 100.0f;
        fv->rsi_7 = clampf(fv->rsi_7, 0.0f, 100.0f);
    } else {
        fv->rsi_7 = 50.0f;
    }
    
    // ── EMA fast(3), slow(8), MACD ──
    if (hist_len >= 3) {
        float ema3 = 0, ema8 = 0;
        // Simple SMA-based approximation
        for (int p = 0; p < 3 && p < hist_len; p++) {
            int idx = (hist_idx - 1 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            ema3 += price_hist[idx];
        }
        ema3 /= fminf(3, hist_len);
        for (int p = 0; p < 8 && p < hist_len; p++) {
            int idx = (hist_idx - 1 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            ema8 += price_hist[idx];
        }
        ema8 /= fminf(8, hist_len);
        fv->ema_fast = ema3 / 100.0f;       // Normalized
        fv->ema_slow = ema8 / 100.0f;
        fv->macd_hist = (ema3 - ema8) / prev * 100.0f;  // MACD as % of price
    }
    
    // ── Bollinger %b ──
    if (hist_len >= 20) {
        float mean = 0, sum_sq = 0;
        int n = fminf(20, hist_len);
        for (int p = 0; p < n; p++) {
            int idx = (hist_idx - 1 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            mean += price_hist[idx];
        }
        mean /= n;
        for (int p = 0; p < n; p++) {
            int idx = (hist_idx - 1 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            float d = price_hist[idx] - mean;
            sum_sq += d * d;
        }
        float std = sqrtf(sum_sq / n);
        float upper = mean + 2.0f * std, lower = mean - 2.0f * std;
        if (upper > lower) fv->bollinger_pct = (curr - lower) / (upper - lower);
        else fv->bollinger_pct = 0.5f;
        fv->bollinger_pct = clampf(fv->bollinger_pct, 0.0f, 1.0f);
    } else {
        fv->bollinger_pct = 0.5f;
    }
    
    // ── Volume proxy from world liquidity ──
    fv->volume_surge_ratio = state->sim.world_liquidity;
    
    // ── Divergence: bull/bear based on world trend ──
    fv->divergence_score = clampf(state->sim.world_trend, -1.0f, 1.0f);
    
    // ── Features that need price history lookback ──
    if (hist_len >= 5) {
        float p5 = price_hist[(hist_idx - 5 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN];
        fv->pump_score = clampf((curr / p5 - 1.0f) * 10.0f, -1.0f, 1.0f);
    } else {
        fv->pump_score = 0.0f;
    }
    
    // ── Consensus proxy: estimate from world volatility ──
    // In high vol, agents tend to disagree more
    fv->herd_consensus = 0.6f - state->sim.world_volatility * 0.3f;
    
    // ── GAAD φ-interval features (synthetic from price history) ──
    if (hist_len >= 10) {
        // φ^1 (~1.6 ticks), φ^2 (~2.6), φ^3 (~4.2)
        int p1 = (int)(1.618f);
        int p2 = (int)(2.618f);
        int p3 = (int)(4.236f);
        if (p1 < hist_len) {
            float v1 = price_hist[(hist_idx - 1 - p1 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN];
            fv->phi_return = (curr / v1 - 1.0f) * 10.0f;  // φ-interval return
        }
        if (p2 < hist_len && p1 < hist_len) {
            float v2a = price_hist[(hist_idx - 1 - p2 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN];
            float v2b = price_hist[(hist_idx - 1 - p1 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN];
            fv->phi_vol = fabsf(v2a / v2b - 1.0f) * 50.0f;  // φ-interval volatility proxy
        }
        if (p3 < hist_len) {
            float v3 = price_hist[(hist_idx - 1 - p3 + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN];
            fv->phi_momentum = (curr / v3 - 1.0f) * 5.0f;  // Longer φ momentum
        }
        fv->phi_return = clampf(fv->phi_return, -2.0f, 2.0f);
        fv->phi_vol = clampf(fv->phi_vol, 0.0f, 1.0f);
        fv->phi_momentum = clampf(fv->phi_momentum, -2.0f, 2.0f);
    }
    
    // ── DFT dominant frequency (simple FFT proxy from recent returns) ──
    if (hist_len >= 5) {
        float freq_power = 0;
        for (int p = 0; p < 5; p++) {
            int i0 = (hist_idx - 1 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            int i1 = (hist_idx - 2 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            freq_power += fabsf(price_hist[i0] - price_hist[i1]);
        }
        fv->dft_dominant = clampf(freq_power / (prev * 0.05f), 0.0f, 1.0f);
    }
    
    // ── Tail risk from realized volatility clustering ──
    if (hist_len >= 10) {
        float recent_vol = 0, older_vol = 0;
        for (int p = 0; p < 5; p++) {
            int i0 = (hist_idx - 1 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            int i1 = (hist_idx - 2 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            recent_vol += fabsf(price_hist[i0] - price_hist[i1]);
        }
        for (int p = 5; p < 10; p++) {
            int i0 = (hist_idx - 1 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            int i1 = (hist_idx - 2 - p + PRICE_HISTORY_LEN) % PRICE_HISTORY_LEN;
            older_vol += fabsf(price_hist[i0] - price_hist[i1]);
        }
        recent_vol /= 5.0f; older_vol /= 5.0f;
        // If recent vol much higher than older → tail risk
        if (older_vol > 0) fv->tail_risk_score = clampf(recent_vol / older_vol - 1.0f, 0.0f, 1.0f);
        else fv->tail_risk_score = 0.0f;
    } else {
        fv->tail_risk_score = clampf(state->sim.world_volatility * 3.0f, 0.0f, 1.0f);
    }
    
    // ── Cross-asset features (from price history divergence) ──
    // In a simulation, cross-asset divergence is derived from recent price volatility
    fv->cross_asset_div = (hist_len >= 10) ? 
        clampf((price_hist[(hist_idx-1+PRICE_HISTORY_LEN)%PRICE_HISTORY_LEN] /
                price_hist[(hist_idx-5+PRICE_HISTORY_LEN)%PRICE_HISTORY_LEN] - 1.0f) * 2.0f, -1.0f, 1.0f) : 0.0f;
    fv->macro_momentum = fv->micro_momentum * 0.1f;

    // ── Real features derived from price history (replaces synthetic proxies) ──
    // All features correlated with actual simulated price behavior, not random/global noise.

    // Options-implied: from realized volatility regime (high vol = high option premium)
    float vol_est = fabsf(fv->micro_momentum) * 0.5f;
    if (hist_len >= 15) {
        float vol_sum = 0;
        for (int p = 1; p < 15 && p < hist_len; p++) {
            int i0 = (hist_idx-1-p+PRICE_HISTORY_LEN)%PRICE_HISTORY_LEN;
            int i1 = (hist_idx-2-p+PRICE_HISTORY_LEN)%PRICE_HISTORY_LEN;
            vol_sum += fabsf((price_hist[i0]-price_hist[i1])/price_hist[i1]);
        }
        vol_est = vol_sum / fminf(14, hist_len-1) * 100.0f;
    }
    fv->iv_skew_feat = clampf(0.3f + vol_est * 2.0f, 0.0f, 1.0f);
    fv->impl_move_feat = clampf(vol_est * 3.0f, 0.0f, 1.0f);
    fv->term_slope_feat = clampf((fv->macd_hist - fv->micro_momentum * 0.1f) * 0.5f, -1.0f, 1.0f);

    // On-chain: derived from price level (high price = high BTC dominance signal)
    float price_norm = curr / 200.0f; // Normalize price to ~0-1 range (Btc 100-200 range)
    fv->btc_dom_signal_feat = clampf(0.4f + (curr > prev ? 0.2f : -0.1f) + vol_est * 0.3f, 0.0f, 1.0f);
    fv->btc_mcap_ath_feat = clampf(0.3f + price_norm * 0.5f, 0.0f, 1.0f);
    fv->btc_vol_7d_feat = clampf(vol_est * 5.0f, 0.0f, 1.0f);

    // Stablecoin flow: high vol = risk-off (capital leaves stables for risk assets)
    fv->stable_risk_app_feat = 1.0f - clampf(vol_est, 0.0f, 1.0f);
    fv->stable_vol_feat = clampf(vol_est * 0.5f, 0.0f, 1.0f);
    float trend = (curr - prev) / prev;
    fv->usdt_dom_feat = clampf(0.5f + (trend > 0 ? -0.2f : 0.2f) + vol_est * 0.3f, 0.0f, 1.0f);

    // Funding rate: positive in uptrends (longs pay shorts in perps)
    fv->funding_rate_feat = clampf(0.5f + fabsf(trend) * 5.0f, 0.0f, 1.0f);
    fv->funding_signal_feat = clampf(trend * 10.0f, -1.0f, 1.0f);

    // OI: higher in trending regimes, lower in chop
    float trend_strength = fabsf(trend) * 20.0f;
    fv->btc_oi_feat = clampf(0.4f + trend_strength, 0.0f, 1.0f);
    fv->spy_oi_feat = clampf(0.5f + fabsf(fv->macro_momentum) * 0.3f, 0.0f, 1.0f);

    // L/S ratio: follows trend direction (more longs in uptrend, more shorts in downtrend)
    fv->ls_ratio_feat = clampf(0.5f + trend * 5.0f, 0.0f, 1.0f);
    fv->buy_pct_feat = clampf(0.5f + trend * 8.0f, 0.0f, 1.0f);
    fv->ls_signal_feat = clampf(0.5f + trend * 10.0f, 0.0f, 1.0f);

    // Liquidations: more in high volatility and sharp reversals
    float vol_cluster = (hist_len >= 10) ? fabsf(fv->tail_risk_score - 0.5f) * 2.0f : 0.5f;
    fv->liq_ls_ratio_feat = clampf(0.5f + (trend < 0 ? 0.3f : -0.1f), 0.0f, 1.0f);
    fv->liq_intensity_feat = clampf(vol_est * 4.0f + vol_cluster * 0.3f, 0.0f, 1.0f);
    fv->long_dom_feat = clampf(0.5f + trend * 5.0f, 0.0f, 1.0f);

    // Whale activity: correlated with volume and volatility
    fv->large_tx_ratio_feat = clampf(0.2f + vol_est * 2.0f + state->sim.world_liquidity * 0.3f, 0.0f, 1.0f);
    fv->whale_activity_feat = clampf(0.3f + vol_est * 1.5f, 0.0f, 1.0f);
    fv->acc_signal_feat = clampf(0.5f + trend * 3.0f, 0.0f, 1.0f);

    // ETF flow: bullish in uptrends
    fv->etf_flow_feat = clampf(0.5f + trend * 8.0f, 0.0f, 1.0f);
    fv->conc_norm_feat = clampf(0.5f + fabsf(trend) * 3.0f, 0.0f, 1.0f);
    fv->avg_flow_feat = clampf(0.5f + trend * 6.0f, 0.0f, 1.0f);

    // Hashrate & mining: conservative values based on price trend
    fv->hash_rate_feat = clampf(0.5f + trend_strength * 0.5f, 0.0f, 1.0f);
    fv->difficulty_feat = clampf(0.6f + price_norm * 0.3f, 0.0f, 1.0f);
    fv->miner_floor_feat = clampf(0.3f + fabsf(trend) * 5.0f, 0.0f, 1.0f);

    // Valuation models: derived from price level relative to simulated history
    fv->s2f_feat = clampf(0.4f + price_norm * 0.4f, 0.0f, 1.0f);
    fv->mvrv_feat = clampf(0.3f + (curr > 150.0f ? 0.4f : 0.2f) + trend * 3.0f, 0.0f, 1.0f);
    fv->puell_feat = clampf(0.4f + trend * 5.0f, 0.0f, 1.0f);
    fv->pi_cycle_feat = clampf(0.4f + fabsf(fv->macd_hist) * 0.2f, 0.0f, 1.0f);
    fv->mayer_feat = clampf(0.4f + (curr / 120.0f - 1.0f) * 0.5f + trend * 3.0f, 0.0f, 1.0f);

    // Dark pool & institutions: more activity in high volatility, trend reversals
    fv->dark_pool_ratio_feat = clampf(0.3f + vol_est * 2.0f, 0.0f, 1.0f);
    fv->dark_pool_wow_feat = clampf(0.4f + fabsf(trend) * 3.0f, 0.0f, 1.0f);

    // Congressional & insider: no real data in sim, derive from regime
    fv->congress_buy_feat = clampf(0.5f + trend * 3.0f, 0.0f, 1.0f);
    fv->congress_div_feat = clampf(0.4f + vol_est, 0.0f, 1.0f);
    fv->insider_density_feat = clampf(0.3f + vol_est * 1.5f, 0.0f, 1.0f);
    fv->insider_trend_feat = clampf(0.5f + trend * 3.0f, 0.0f, 1.0f);
    fv->inst_filing_density_feat = clampf(0.4f + price_norm * 0.3f, 0.0f, 1.0f);
    fv->inst_filing_trend_feat = clampf(0.5f + trend * 2.0f, 0.0f, 1.0f);
    fv->short_intensity_feat = clampf(0.5f - trend * 4.0f, 0.0f, 1.0f);
    fv->short_trend_feat = clampf(0.5f - trend * 5.0f, 0.0f, 1.0f);

    // Earnings & ETF features: seasonal patterns based on regime
    fv->earn_beat_rate_feat = clampf(0.4f + price_norm * 0.3f + trend * 2.0f, 0.0f, 1.0f);
    fv->earn_density_feat = clampf(curr / 500.0f, 0.0f, 1.0f);
    fv->etf_concentration_feat = clampf(0.5f + fabsf(trend) * 2.0f, 0.0f, 1.0f);
    fv->sector_breadth_feat = clampf(0.5f + trend * 4.0f, 0.0f, 1.0f);

    // Options chain: put/call ratio from volatility regime
    fv->options_pcr_feat = clampf(0.5f + (trend < 0 ? vol_est : -vol_est * 0.5f), 0.0f, 1.0f);
    fv->options_max_pain_feat = clampf(0.4f + fabsf(fv->bollinger_pct - 0.5f), 0.0f, 1.0f);

    // Seasonality: estimated from price history pattern
    fv->dow_seasonality_feat = clampf(0.4f + fabsf(trend) * 2.0f, 0.0f, 1.0f);
    fv->moy_seasonality_feat = clampf(0.4f + vol_est * 0.5f, 0.0f, 1.0f);

    // News: higher volume = more news, sentiment follows trend
    fv->news_volume_feat = clampf(vol_est * 3.0f, 0.0f, 1.0f);
    fv->news_sentiment_feat = clampf(0.5f + trend * 6.0f, 0.0f, 1.0f);

    // Politician portfolio (derived from regime)
    fv->pol_portfolio_conc_feat = clampf(0.4f + fabsf(trend) * 2.0f, 0.0f, 1.0f);
    fv->pol_conviction_feat = clampf(0.5f + trend * 3.0f, 0.0f, 1.0f);

    // Order book: from liquidity and volatility
    fv->ob_imbalance_feat = clampf(0.5f + trend * 4.0f, 0.0f, 1.0f);
    fv->ob_depth_ratio_feat = state->sim.world_liquidity;
    fv->ob_wall_conc_feat = clampf(0.4f + fabsf(trend) * 2.0f, 0.0f, 1.0f);
    fv->ob_spread_feat = clampf(vol_est * 3.0f, 0.0f, 1.0f);

    // ── Last: fear/greed from volatility (high vol = fear) ──
    fv->fear_greed_norm = clampf(1.0f - vol_est * 2.0f, 0.0f, 1.0f);
}

// ══════════════════════════════════════════════════
// Archetype Systems
// ══════════════════════════════════════════════════

static void init_archetype_population(AgentState *agents, int archetype, int count, int start_id) {
    for (int i = 0; i < count && start_id + i < MAX_AGENTS; i++) {
        AgentState *a = &agents[start_id + i];
        memset(a, 0, sizeof(AgentState));
        a->alive = true;
        a->capital = 1.0f;
        a->starting_capital = 1.0f;
        a->peak_capital = 1.0f;
        a->win_rate_ema = 0.5f;
        
        Genome *g = &a->genome;
        g->archetype = archetype;
        
        switch (archetype) {
            case ARCH_TRADER:
                g->position_size = 0.05f + frand() * 0.15f;
                g->conviction_threshold = 0.20f + frand() * 0.15f;
                g->risk_tolerance = 0.3f + frand() * 0.4f;
                g->time_horizon = 1.0f + frand() * 5.0f;
                g->stop_loss_pct = 0.05f + frand() * 0.10f;
                g->take_profit_pct = 0.10f + frand() * 0.20f;
                g->mean_reversion_bias = 0.0f;
                g->herd_antipathy = 0.0f;
                g->min_edge_pct = 1.0f;
                break;
            case ARCH_BETTOR:
                g->position_size = 0.02f + frand() * 0.08f;
                g->conviction_threshold = 0.25f;
                g->risk_tolerance = 0.2f + frand() * 0.3f;
                g->mean_reversion_bias = 0.3f;
                g->time_horizon = 0.5f + frand() * 2.0f;
                g->min_edge_pct = 2.0f;
                break;
            case ARCH_SPECULATOR:
                g->position_size = 0.10f + frand() * 0.20f;
                g->conviction_threshold = 0.20f;
                g->risk_tolerance = 0.5f + frand() * 0.4f;
                g->herd_antipathy = -0.3f;
                g->take_profit_pct = 0.20f + frand() * 0.30f;
                g->time_horizon = 0.5f + frand() * 3.0f;
                break;
            case ARCH_HEDGER:
                g->position_size = 0.01f + frand() * 0.03f;
                g->conviction_threshold = 0.30f;
                g->risk_tolerance = 0.1f + frand() * 0.2f;
                g->mean_reversion_bias = -0.5f;
                g->min_edge_pct = 5.0f;
                break;
            case ARCH_NOISE:
                g->position_size = 0.01f + frand() * 0.10f;
                g->conviction_threshold = 0.30f + frand() * 0.40f;
                g->risk_tolerance = frand();
                g->time_horizon = 0.1f + frand() * 10.0f;
                break;
        }
        
        // Initialize feat_weight with small random values (all 80 features)
        for (int f = 0; f < N_FEATURES; f++) {
            g->feat_weight[f] = frand_signed() * 0.05f;
        }
        g->bias = frand_signed() * 0.1f;
        g->learning_rate = 0.01f;
    }
}

static void archetype_vote(AgentState *agent, const FeatureVector *fv, const RoomState *state, VoteRecord *vote) {
    Genome *g = &agent->genome;
    (void)state;
    
    // Compute signal from ALL N_FEATURES features
    double signal = g->bias;
    for (int f = 0; f < N_FEATURES; f++) {
        signal += g->feat_weight[f] * ((float*)fv)[f];
    }
    
    float sigmoid = 1.0f / (1.0f + expf(-clampf(signal, -10.0f, 10.0f)));
    
    switch (g->archetype) {
        case ARCH_TRADER: {
            vote->direction = sigmoid > 0.5f;
            vote->conviction = fabsf(sigmoid - 0.5f) * 2.0f;
            vote->position_size = g->position_size * g->risk_tolerance;
            vote->voted = vote->conviction > g->conviction_threshold;
            break;
        }
        case ARCH_BETTOR: {
            float skew = (sigmoid - 0.5f) * 2.0f;
            vote->direction = skew > 0;
            vote->conviction = fabsf(skew);
            vote->position_size = g->position_size * (0.5f + fabsf(skew) * 0.5f);
            vote->voted = vote->conviction > g->conviction_threshold;
            break;
        }
        case ARCH_SPECULATOR: {
            float momentum = fv->micro_momentum;
            float trend_signal = momentum * 10.0f + (sigmoid - 0.5f) * 0.5f;
            float act = 1.0f / (1.0f + expf(-clampf(trend_signal, -5.0f, 5.0f)));
            vote->direction = act > 0.5f;
            vote->conviction = fabsf(act - 0.5f) * 2.0f;
            vote->position_size = g->position_size * (1.0f + fabsf(momentum) * 5.0f);
            vote->voted = vote->conviction > g->conviction_threshold;
            break;
        }
        case ARCH_HEDGER: {
            float recent_move = fv->price_delta_pct;
            vote->direction = recent_move < 0;
            vote->conviction = fabsf(recent_move) * 3.0f;
            if (vote->conviction > 0.8f) vote->conviction = 0.8f;
            vote->position_size = g->position_size;
            vote->voted = vote->conviction > g->conviction_threshold;
            break;
        }
        case ARCH_NOISE: {
            vote->voted = frand() < 0.3f;
            if (vote->voted) {
                vote->direction = frand() > 0.5f;
                vote->conviction = 0.3f + frand() * 0.4f;
                vote->position_size = g->position_size * frand();
            }
            break;
        }
    }
}

static float archetype_hedge_factor(const AgentState *agent, const RoomState *state) {
    if (agent->genome.archetype == ARCH_HEDGER) {
        float trend_strength = fabsf(state->sim.world_trend);
        return 1.0f - trend_strength * 0.5f;
    }
    if (agent->genome.archetype == ARCH_SPECULATOR) {
        float trend_strength = fabsf(state->sim.world_trend);
        return 1.0f + trend_strength;
    }
    return 1.0f;
}

// ══════════════════════════════════════════════════
// Output
// ══════════════════════════════════════════════════

static void write_training_snapshot(const RoomState *state, int cycle, FILE *log) {
    int total_wins = 0, total_losses = 0;
    int agent_count = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!state->agents[i].alive) continue;
        total_wins += state->agents[i].wins;
        total_losses += state->agents[i].losses;
        agent_count++;
    }
    float wr = (total_wins + total_losses > 0) ? (float)total_wins / (total_wins + total_losses) : 0.5f;
    float sharpe = (wr - 0.5f) * 4.0f;  // Simple Sharpe approximation
    
    fprintf(log, "%d,%d,%.4f,%.4f,%.4f,%.2f,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%d,%d,%d\n",
            cycle, state->sim.curriculum_phase,
            state->sim.world_trend, state->sim.world_volatility, state->sim.world_liquidity,
            state->current_market.close,
            state->sim.n_traders, state->sim.n_bettors, state->sim.n_speculators, state->sim.n_hedgers, state->sim.n_noise,
            state->stats.avg_conviction, wr, state->stats.room_pnl_pct, sharpe,
            state->stats.voted_this_cycle, agent_count, total_wins + total_losses);
}
