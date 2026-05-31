/**
 * room_engine.c — The Room Main Loop
 * L0: Orchestrates all 6 layers, <100ms per cycle.
 * Reads market data → computes features → runs vote → allocates capital → evolves
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include <dirent.h>
#include "types.h"
#include "nested_ht_infer.h"

// ── v2: Declared in room_vote.c ──
void init_genome_weights(Genome *g);

// ── Pace control ──
// In paper mode, 5ms between cycles for fast bulk historical runs
// In live mode, 1s between cycles to match real-time data
#define PAPER_PACE_NS    5000000LL   // 5ms for paper mode
#define LIVE_PACE_NS     1000000000LL // 1s for live mode
#define ROOM_DIR   "/home/wubu2/.hermes/pm_logs/c_room"
#ifdef PAPER_MODE
#define STATE_PATH ROOM_DIR "/room_state_paper.bin"
#else
#define STATE_PATH ROOM_DIR "/room_state.bin"
#endif
#define FEED_PATH  ROOM_DIR "/market_feed.json"
#define LOG_PATH   ROOM_DIR "/room_log.csv"

// ── Globals ──
static RoomState *state = NULL;
static int state_fd = -1;
static volatile int running = 1;

// ── Nested model inference ──
#define NESTED_BUF_SIZE 50
static NestedModelCollection *g_nested = NULL;
static double g_nested_price_buf[NESTED_BUF_SIZE];
static int g_nested_buf_len = 0;
static int g_nested_buf_idx = 0;
static double g_prev_volume = 0.0;
static double g_nested_prediction = 0.5;  // latest cascade prediction (0-1)

// ── Forward decls ──
RoomError room_feeds_load(MarketTick *tick);
RoomError room_features_compute(const MarketTick *tick, FeatureVector *fv, const RoomState *s);
RoomError room_vote_run(AgentState *agents, int n, const FeatureVector *fv, VoteRecord *votes, int *count);
RoomError room_capital_apply(VoteRecord *votes, int count, AgentState *agents, int n, TradeRecord *trades, int start_offset, int *new_count, int64_t window_ts);
RoomError room_capital_resolve(TradeRecord *trades, int *tcount, 
                               const MarketTick *resolution_tick,
                               float prev_close,
                               AgentState *agents,
                               int max_trades,
                               FeatureImportance *importance);
RoomError room_darwin_evolve(AgentState *agents, int n, int cycle, DarwinRecord *rec);
RoomError room_darwin_compute_diversity(const AgentState *agents, int n, RoomStats *stats);
void       room_bridge_write(RoomState *state);

// ── Nested model: compute 17-dim features and run inference ──
static double compute_nested_prediction(const MarketTick *tick) {
    if (!g_nested) return 0.5;
    
    // Push price into ring buffer
    g_nested_price_buf[g_nested_buf_idx] = tick->close;
    g_nested_buf_idx = (g_nested_buf_idx + 1) % NESTED_BUF_SIZE;
    if (g_nested_buf_len < NESTED_BUF_SIZE) g_nested_buf_len++;
    
    if (g_nested_buf_len < 10) return 0.5; // Need warmup
    
    // Build linearized price array
    double px[NESTED_BUF_SIZE];
    for (int i = 0; i < g_nested_buf_len; i++) {
        int idx = (g_nested_buf_idx - g_nested_buf_len + i + NESTED_BUF_SIZE) % NESTED_BUF_SIZE;
        px[i] = g_nested_price_buf[idx];
    }
    
    // Compute 17-dim feature vector (matches nested_ht training)
    double feats[17] = {0};
    double price = tick->close;
    
    // feats[0-4]: returns at 1,3,5,10,20 periods
    if (g_nested_buf_len >= 2) feats[0] = (price - px[g_nested_buf_len-2]) / fmax(px[g_nested_buf_len-2], 0.001);
    if (g_nested_buf_len >= 4) feats[1] = (price - px[g_nested_buf_len-4]) / fmax(px[g_nested_buf_len-4], 0.001);
    if (g_nested_buf_len >= 6) feats[2] = (price - px[g_nested_buf_len-6]) / fmax(px[g_nested_buf_len-6], 0.001);
    if (g_nested_buf_len >= 11) feats[3] = (price - px[g_nested_buf_len-11]) / fmax(px[g_nested_buf_len-11], 0.001);
    if (g_nested_buf_len >= 21) feats[4] = (price - px[g_nested_buf_len-21]) / fmax(px[g_nested_buf_len-21], 0.001);
    
    // feats[5]: volatility
    feats[5] = tick->btc_30d_volatility / 100.0;
    
    // feats[6]: HL range / close
    double hl_range = tick->high - tick->low;
    feats[6] = (price > 0.001) ? hl_range / price : 0.0;
    
    // feats[7]: volume ratio (approximate)
    feats[7] = 1.0;
    
    // feats[8]: volume momentum
    feats[8] = (g_prev_volume > 0.001) ? tick->volume / g_prev_volume : 1.0;
    g_prev_volume = tick->volume;
    
    // feats[9]: price position in range
    feats[9] = (hl_range > 0.001) ? (price - tick->low) / hl_range : 0.5;
    
    // feats[10]: gap
    double prev_close = (g_nested_buf_len >= 2) ? px[g_nested_buf_len-2] : price;
    feats[10] = (prev_close > 0.001) ? (tick->open - prev_close) / prev_close : 0.0;
    
    // feats[11]: cascade (start at 0.5 for independent, will be updated)
    feats[11] = 0.5;
    
    // feats[12-16]: macro features
    feats[12] = tick->sp500 / 1000.0;
    feats[13] = tick->vix;
    feats[14] = 0; // fedfunds
    feats[15] = 0; // cpi
    feats[16] = 0; // t10y2y
    
    // Run cascade through levels (L0→L1→...→L5)
    double cascade = 0.5;
    for (int l = 0; l < g_nested->n_levels; l++) {
        if (!g_nested->mlp_models[l] && !g_nested->lr_models[l]) continue;
        
        // Set cascade feature at slot 11
        feats[11] = cascade;
        
        double pred = 0.5;
        if (g_nested->mlp_models[l]) {
            pred = nested_predict(g_nested, l, feats, cascade);
        } else if (g_nested->lr_models[l]) {
            // Use LR directly
            LRModel *lr = g_nested->lr_models[l];
            double xs[17];
            memcpy(xs, feats, 17 * sizeof(double));
            standardize_x(xs, lr->mean, lr->std, lr->d);
            pred = lr_predict_raw(lr, xs);
        }
        cascade = pred;
    }
    
    return cascade;
}

// ── Signal handler ──
static volatile int kill_switch_engaged = 0;
static void handle_sig(int sig) {
    if (sig == SIGUSR1) {
        kill_switch_engaged = 1;
        fprintf(stderr, "\n[KILL SWITCH] ENGAGED via SIGUSR1 — liquidating all positions and shutting down.\n");
    }
    running = 0;
}

// ── Init agents with random genomes ──
static void init_agents(AgentState *agents, int n) {
    srand(42); // Deterministic seed for reproducibility
    float start_cap = 50.0f; // Each agent gets $50
    for (int i = 0; i < n; i++) {
        agents[i].alive = true;
        agents[i].capital = start_cap;
        agents[i].starting_capital = start_cap;
        agents[i].trades = 0;
        agents[i].wins = 0;
        agents[i].losses = 0;
        agents[i].total_pnl = 0.0f;
        agents[i].max_drawdown = 0.0f;
        agents[i].peak_capital = start_cap;
        agents[i].consecutive_losses = 0;
        agents[i].win_rate_ema = 0.5f;
        agents[i].last_trade_window = -1;
        // C10: Initialize conviction tracking
        agents[i].conv_hi_wins = 0;
        agents[i].conv_hi_total = 0;
        agents[i].conv_lo_wins = 0;
        agents[i].conv_lo_total = 0;
        // C19: Initialize weight diversity
        agents[i].weight_mag = 0;
        
        // Random genome within bounds
        agents[i].genome.position_size     = 0.01f + (float)rand() / RAND_MAX * 0.49f;
        agents[i].genome.conviction_threshold = 0.01f + (float)rand() / RAND_MAX * 0.29f;  // Lower initial threshold
        agents[i].genome.risk_tolerance    = (float)rand() / RAND_MAX;
        agents[i].genome.lie_sensitivity   = 0.10f + (float)rand() / RAND_MAX * 0.88f;
        agents[i].genome.herd_antipathy    = (float)rand() / RAND_MAX;
        agents[i].genome.stop_loss_pct     = 0.01f + (float)rand() / RAND_MAX * 0.24f;
        agents[i].genome.take_profit_pct   = 0.01f + (float)rand() / RAND_MAX * 0.59f;
        agents[i].genome.min_edge_pct      = 0.5f + (float)rand() / RAND_MAX * 49.5f;
        agents[i].genome.time_horizon      = 0.1f + (float)rand() / RAND_MAX * 9.9f;
        agents[i].genome.mean_reversion_bias = -1.0f + (float)rand() / RAND_MAX * 2.0f;
        // ── v2: Initialize learned weights ──
        init_genome_weights(&agents[i].genome);
        // Aggressive sign diversity: each weight gets randomly flipped per agent
        for (int w = 0; w < N_FEATURES; w++) {
            float base = agents[i].genome.feat_weight[w];
            // 40% chance to flip sign — creates strong directional diversity
            if ((float)rand() / RAND_MAX < 0.4f) base = -base;
            // Further perturb by random magnitude
            agents[i].genome.feat_weight[w] = base + ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
        }
        agents[i].genome.bias = ((float)rand() / RAND_MAX - 0.5f) * 0.3f;
        agents[i].genome.learning_rate = 0.005f + (float)rand() / RAND_MAX * 0.015f;
        // ── P22: Initialize regime-specific weights ──
        for (int r = 0; r < N_REGS; r++) {
            for (int w = 0; w < N_FEATURES; w++) {
                float base = agents[i].genome.feat_weight[w];
                if ((float)rand() / RAND_MAX < 0.4f) base = -base;
                agents[i].genome.regime_weight[r][w] = base + ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
            }
            agents[i].genome.regime_bias[r] = agents[i].genome.bias + ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
        }
        // ── v2: Initialize hidden state to zero ──
        memset(agents[i].hidden, 0, sizeof(agents[i].hidden));
        agents[i].last_conviction = 0.0f;
        memset(agents[i].last_features, 0, sizeof(agents[i].last_features));
    }
}

// ── Load state from mmap or create fresh ──
static RoomError load_or_init_state(void) {
    // Ensure dir exists
    mkdir(ROOM_DIR, 0755);
    
    state_fd = open(STATE_PATH, O_RDWR | O_CREAT, 0644);
    if (state_fd < 0) {
        perror("open state");
        return ERR_MMAP_FAIL;
    }
    
    // Size the file
    size_t sz = sizeof(RoomState);
    if (ftruncate(state_fd, sz) < 0) {
        perror("ftruncate");
        close(state_fd);
        return ERR_MMAP_FAIL;
    }
    
    state = (RoomState *)mmap(NULL, sz, PROT_READ | PROT_WRITE,
                               MAP_SHARED, state_fd, 0);
    if (state == MAP_FAILED) {
        perror("mmap");
        close(state_fd);
        return ERR_MMAP_FAIL;
    }
    
    // Check if already initialized
    if (state->magic != STATE_MAGIC) {
        memset(state, 0, sz);
        state->magic = STATE_MAGIC;
        init_agents(state->agents, MAX_AGENTS);
        
        // ── Load multi-market trained genomes if available ──
        // Seeds a subset of agents with genomes optimized for different market types
        const char *mm_dir = "/home/wubu2/money-room/data/multi_market";
        DIR *mm_d = opendir(mm_dir);
        if (mm_d) {
            struct dirent *mm_e;
            int m_idx = 0;
            int agents_per_market = MAX_AGENTS / 20;  // ~500 agents per market type
            if (agents_per_market < 1) agents_per_market = 1;
            
            while ((mm_e = readdir(mm_d)) != NULL && m_idx < 20) {
                // Match *.bin files
                size_t nlen = strlen(mm_e->d_name);
                if (nlen < 5 || strcmp(mm_e->d_name + nlen - 4, ".bin") != 0) continue;
                if (strcmp(mm_e->d_name, ".") == 0 || strcmp(mm_e->d_name, "..") == 0) continue;
                
                char mm_path[512];
                snprintf(mm_path, sizeof(mm_path), "%s/%s", mm_dir, mm_e->d_name);
                
                FILE *mm_f = fopen(mm_path, "rb");
                if (!mm_f) continue;
                
                Genome trained_genome;
                int market_type = 0;
                if (fread(&trained_genome, sizeof(Genome), 1, mm_f) == 1) {
                    // Try to read market type (may not exist in older files)
                    fread(&market_type, sizeof(int), 1, mm_f);
                    
                    // Seed agents with this genome
                    int start = m_idx * agents_per_market;
                    int end = start + agents_per_market;
                    if (end > MAX_AGENTS) end = MAX_AGENTS;
                    
                    for (int i = start; i < end; i++) {
                        // Copy the trained genome
                        memcpy(&state->agents[i].genome, &trained_genome, sizeof(Genome));
                        // Add some noise for diversity
                        for (int w = 0; w < N_FEATURES; w++) {
                            float noise = ((float)(rand() % 2001 - 1000)) / 10000.0f;
                            state->agents[i].genome.feat_weight[w] += noise;
                        }
                        state->agents[i].genome.bias += ((float)(rand() % 2001 - 1000)) / 10000.0f;
                    }
                    printf("[ROOM] Loaded %s genome into agents %d-%d (market_type=%d)\n",
                           mm_e->d_name, start, end - 1, market_type);
                }
                fclose(mm_f);
                m_idx++;
            }
            closedir(mm_d);
            printf("[ROOM] Multi-market genomes loaded: %d types\n", m_idx);
        }
        state->stats.active_agents = MAX_AGENTS;
        state->stats.capital_current = 50.0f * MAX_AGENTS;
        state->stats.capital_peak = 50.0f * MAX_AGENTS;
        state->room_capital = 50.0f;  // Real $50 seed
        state->room_capital_peak = 50.0f;
        state->prev_room_capital = 50.0f;
        state->room_trade.resolved_at = -1; // Mark as init
        // ── T17: Circuit breaker defaults ──
        state->circuit_breaker_cycles = 0;
        state->circuit_breaker_count = 0;
        state->max_drawdown_pct = 0.20f;       // 20% max drawdown
        state->circuit_cooldown_cycles = 100;   // Cool down for 100 cycles
        state->max_consecutive_losses = 10;     // Trip after 10 consecutive losses
        state->consec_room_losses = 0;
        state->circuit_breaker_peak = 50.0f;
        state->daily_pnl = 0.0f;
        state->daily_loss_streak = 0;
        // ── T18: Position limits defaults ──
        state->max_position_pct_room = 0.02f;      // Max 2% of room total per agent
        state->max_total_exposure_pct = 0.25f;      // Max 25% of total capital at risk
        state->current_total_exposure = 0.0f;
        state->peak_total_exposure = 0.0f;
        // ── T19: Trade rate limit defaults ──
        state->max_trades_per_cycle = 5000;      // Max 5000 new trades per cycle
        state->trades_deferred = 0;
        state->total_trades_deferred = 0;
        // ── T20: Slippage tracking defaults ──
        state->total_slippage_paid = 0.0f;
        state->slippage_events = 0;
        printf("[ROOM] Initialized %d agents, $50 room seed\n", MAX_AGENTS);
        printf("[ROOM] CB: consec_room_losses=%d max_consecutive_losses=%d circuit_breaker_cycles=%d\n",
               state->consec_room_losses, state->max_consecutive_losses, state->circuit_breaker_cycles);
    } else {
        printf("[ROOM] Restored %d agents from state\n", state->stats.active_agents);
        // ── T001: Validate state on restore — reset if corrupted ──
        if (state->trade_count > MAX_TRADE_HIST || state->trade_count < 0) {
            printf("[ROOM] Corrupted trade_count=%d, resetting to 0\n", state->trade_count);
            state->trade_count = 0;
        }
        if (state->consec_room_losses > 10000) {
            state->consec_room_losses = 0;
        }
    }
    
    return ERR_OK;
}

// ── Nanosecond clock ──
static int64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ════════════════════════════════════════════════════════
//  MAIN LOOP
// ════════════════════════════════════════════════════════
int main(void) {
    printf("[ROOM] Starting... sizeof(RoomState)=%zu expected_file=%zu\n", 
           sizeof(RoomState), sizeof(RoomState));
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGUSR1, handle_sig);
    
    RoomError err = load_or_init_state();
    if (err != ERR_OK) return 1;
    
    // ── Load nested model weights ──
    const char *weights_path = "/home/wubu2/.hermes/pm_logs/nested_ht/weights.json";
    g_nested = load_nested_weights(weights_path);
    if (g_nested) {
        printf("[ROOM] Nested models loaded: %d levels\n", g_nested->n_levels);
        for (int i = 0; i < g_nested->n_levels; i++) {
            if (g_nested->mlp_models[i])
                printf("  L%d: %d-min MLP (d=%d,h=%d)\n", i, g_nested->res_minutes[i],
                       g_nested->mlp_models[i]->d, g_nested->mlp_models[i]->h);
            if (g_nested->lr_models[i])
                printf("  L%d: %d-min LR (d=%d)\n", i, g_nested->res_minutes[i],
                       g_nested->lr_models[i]->d);
        }
    } else {
        printf("[ROOM] WARN: No nested weights at %s (room will run without)\n", weights_path);
    }
    
    printf("[ROOM] Engine starting. Target: <100ms/cycle\n");
    // ── Safety: Force-clear circuit breaker on startup ──
    state->circuit_breaker_cycles = 0;
    state->consec_room_losses = 0;
    state->circuit_breaker_count = 0;
    // ── Safety: Force-clear trade_count if corrupted ──
    if (state->trade_count > MAX_TRADE_HIST || state->trade_count < 0) {
        printf("[ROOM] CORRUPTED trade_count=%d, FORCE RESET to 0\n", state->trade_count);
        state->trade_count = 0;
    }
    
    // ── Main loop ──
    FILE *log = fopen(LOG_PATH, "a");
    if (log) {
        fputs("cycle,window_ts,asset,votes,active,win_rate,sharpe,dd_pct,consensus_spread,room_pnl_pct,room_trades,room_wr,room_cap,slippage$\n", log);
        fclose(log);
    }
    
    // ── Boot-time hard reset of corruptable fields ──
    state->trade_count = 0;
    state->cycle = 0;
    state->vote_count = 0;
    state->consec_room_losses = 0;
    state->circuit_breaker_cycles = 0;
    state->circuit_breaker_count = 0;
    
    int idle_cycles = 0;
    float prev_close = 0.0f;  // Track for inter-candle comparison
    
    while (running) {
        int64_t cycle_start = ns_now();
        
        // ── L1: Load market feed ──
        MarketTick tick;
        err = room_feeds_load(&tick);
        if (err != ERR_OK) {
            // Retry once immediately — feed bridge may be mid-write
            struct timespec retry_ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms
            nanosleep(&retry_ts, NULL);
            err = room_feeds_load(&tick);
        }
        if (err != ERR_OK) {
            printf("[FEED] Load err=%d ts=%ld\n", err, (long)tick.window_ts);
            idle_cycles++;
            if (idle_cycles % 60 == 0) {
                printf("[ROOM] No data for %d cycles...\n", idle_cycles);
            }
            // In paper mode: after 100 idle cycles, assume data exhausted and exit
            if (idle_cycles > 100) {
                printf("[ROOM] Data exhausted (idle). Shutting down.\n");
                break;
            }
            // PAPER_MODE: short sleep, LIVE_MODE: 1s
#ifdef PAPER_MODE
            struct timespec ts = { .tv_sec = 0, .tv_nsec = PAPER_PACE_NS };
#else
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
#endif
            nanosleep(&ts, NULL);
            continue;
        }
        idle_cycles = 0;
        
        // Skip if we already processed this window
        if (tick.window_ts == state->stats.last_window_ts) {
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
            continue;
        }
        
    #ifdef MARKET_MODE
RoomError room_market_apply(VoteRecord *votes, int count,
                            AgentState *agents, int n,
                            TradeRecord *trades, int start_offset,
                            int *new_count, int64_t window_ts);
RoomError room_market_resolve(TradeRecord *trades, int *tcount,
                              const MarketTick *tick,
                              float prev_close,
                              AgentState *agents,
                              int max_trades);
void room_market_stats(RoomState *state);
#endif

    // ── Lock state for writing ──
        state->writing = 1;
        state->current_market = tick;
        
        // ── L2: Compute features ──
        err = room_features_compute(&tick, &state->features, state);
        if (err != ERR_OK) {
            state->writing = 0;
            continue;
        }
        
        // ── L2b: Compute nested cascade prediction ──
        g_nested_prediction = compute_nested_prediction(&tick);
        state->nested_prediction = (float)g_nested_prediction;
        if (state->cycle % 100 == 0 && g_nested) {
            double signal = (g_nested_prediction - 0.5) * 2.0;
            printf("[NESTED] cycle=%d pred=%.4f signal=%+.4f\n",
                   state->cycle, g_nested_prediction, signal);
        }
        
        // ── L3: Run vote ──
        int vote_count = 0;
        err = room_vote_run(state->agents, MAX_AGENTS, &state->features,
                            state->votes, &vote_count);
        state->vote_count = vote_count;

        // ── L3b: P15 Tailslayer hedging — detect tail risk, scale exposure ──
        {
            float tail = state->features.tail_risk_score;
            float prev_hedge = state->stats.hedge_factor;

            // Compute hedge factor: 1.0 when tail_risk < 0.3, scales down to 0.3 at tail_risk=1.0
            float hf;
            if (tail < 0.3f) {
                hf = 1.0f;  // Normal — no hedging
            } else if (tail < 0.7f) {
                hf = 1.0f - (tail - 0.3f) * 0.75f;  // Gradual: 1.0 -> 0.7
            } else {
                hf = 0.7f - (tail - 0.7f) * 1.33f;  // Aggressive: 0.7 -> 0.3
                if (hf < 0.3f) hf = 0.3f;
            }

            state->stats.tail_risk_score = tail;
            state->stats.hedge_factor = hf;

            if (hf < 1.0f && prev_hedge >= 1.0f) {
                printf("[TAIL] HEDGE ACTIVATED: tail=%.3f hedge=%.3f scaling positions by %.0f%%\n",
                       tail, hf, hf * 100.0f);
                state->stats.hedge_active_cycles = 0;
            } else if (hf >= 1.0f && prev_hedge < 1.0f) {
                printf("[TAIL] HEDGE DEACTIVATED: tail=%.3f normal trading resumed\n", tail);
            }

            if (hf < 1.0f && vote_count > 0) {
                state->stats.hedge_active_cycles++;
                // Beam-search ensemble: scale down all vote position_sizes by hedge_factor
                for (int i = 0; i < vote_count; i++) {
                    state->votes[i].position_size *= hf;
                }
            }
        }

        // ── P23: Volatility scaling — scale position sizes inversely with 30d BTC vol ──
        {
            float vol_pct = state->current_market.btc_30d_volatility;
            if (vol_pct < 1.0f) vol_pct = 50.0f;  // Default if no data
            // BTC typical 30d vol ~50-80%. Scale: 1.0 at 65%, 2.0 at 25%, 0.3 at 150%
            float vol_scalar = 65.0f / fmaxf(vol_pct, 10.0f);
            if (vol_scalar > 2.0f) vol_scalar = 2.0f;  // Max 2x in low vol
            if (vol_scalar < 0.3f) vol_scalar = 0.3f;  // Min 0.3x in high vol
            if (vote_count > 0 && fabsf(vol_scalar - 1.0f) > 0.05f) {
                for (int i = 0; i < vote_count; i++) {
                    state->votes[i].position_size *= vol_scalar;
                }
                if (state->cycle % 100 == 0)
                    printf("[VOL] vol=%.1f%% scalar=%.2f scaling %d votes\n",
                           vol_pct, vol_scalar, vote_count);
            }
        }

        // ── T17: Circuit breaker check ──
        // If in cooldown, skip all trading
        if (state->circuit_breaker_cycles > 0) {
            state->circuit_breaker_cycles--;
            if (state->circuit_breaker_cycles == 0) {
                // Cooldown complete — reset
                state->circuit_breaker_peak = state->room_capital;
                state->consec_room_losses = 0;
                printf("[CB] Cooldown complete. Resuming trading at peak=%.2f\n",
                       state->circuit_breaker_peak);
            } else if (state->circuit_breaker_cycles % 20 == 0) {
                printf("[CB] Cooling down: %d cycles remaining\n",
                       state->circuit_breaker_cycles);
            }
            // Update prev_close for the bridge but skip trading
            prev_close = tick.close;
            goto skip_trading;
        }

        // Check drawdown: if room capital dropped > max_drawdown_pct from peak
        if (state->room_capital > state->circuit_breaker_peak) {
            state->circuit_breaker_peak = state->room_capital;
        }
        float drawdown = state->circuit_breaker_peak > 0
            ? (state->circuit_breaker_peak - state->room_capital) / state->circuit_breaker_peak
            : 0.0f;
        if (drawdown > state->max_drawdown_pct && state->circuit_breaker_cycles == 0) {
            state->circuit_breaker_cycles = state->circuit_cooldown_cycles;
            state->circuit_breaker_count++;
            state->circuit_breaker_ts = tick.window_ts;
            printf("[CB] TRIGGERED! Drawdown=%.1f%% max=%.1f%%. "
                   "Room cap $%.2f from peak $%.2f. Cooldown=%d cycles.\n",
                   drawdown * 100, state->max_drawdown_pct * 100,
                   state->room_capital, state->circuit_breaker_peak,
                   state->circuit_cooldown_cycles);
            goto skip_trading;
        }

        // Check consecutive losses — guard: must have at least 1 loss
        if (state->consec_room_losses > 0 && state->consec_room_losses >= state->max_consecutive_losses) {
            state->circuit_breaker_cycles = state->circuit_cooldown_cycles / 2;
            state->circuit_breaker_count++;
            printf("[CB] TRIGGERED! %d consecutive losses. Cooling down %d cycles.\n",
                   state->consec_room_losses, state->circuit_cooldown_cycles / 2);
            goto skip_trading;
        }

        // ── Kill switch check (SIGUSR1) ──
        if (kill_switch_engaged) {
            printf("[KILL SWITCH] Liquidating %d open positions...\n", state->vote_count);
            // Close all open room trades at current price
            if (state->room_trade.resolved_at < 0) {
                // Force-resolve any open room trade as loss (emergency)
                float exit_px = tick.close;
                float entry_px = state->room_trade.entry_price;
                if (entry_px > 0) {
                    float move_pct = (exit_px - entry_px) / entry_px;
                    state->room_trade.won = (move_pct > 0) == state->room_trade.majority_up;
                    state->room_trade.pnl = state->room_trade.won ? state->room_trade.stake * 0.01f : -state->room_trade.stake * 0.01f;
                    state->room_capital += state->room_trade.pnl;
                    state->room_trade.exit_price = exit_px;
                    state->room_trade.resolved_at = tick.window_ts;
                    printf("[KILL SWITCH] Room trade liquidated: PnL=$%.2f\n", state->room_trade.pnl);
                }
            }
            printf("[KILL SWITCH] All positions closed. Shutting down.\n");
            break;
        }

        // ── Room Trade Execution (one per cycle, $50 seed) ──
        // Skip first 10K P2P trades for evolution warm-up.
        // Uses multi-stream expert selection: pick top 100 agents by WR,
        // their votes are diverse across different data streams.
        if (state->trade_count >= 10000 && vote_count > 0) {
            // Use top 100 agents' votes (diverse experts per stream)
            int top_n = 100;
            if (top_n > vote_count) top_n = vote_count;
            int step = vote_count / top_n;
            if (step < 1) step = 1;
            
            int yv = 0, nv = 0;
            for (int i = 0; i < vote_count; i += step) {
                if (state->votes[i].direction) yv++; else nv++;
            }
            
            if (yv != nv) {
                bool majority_up = yv > nv;
                bool room_direction = majority_up;
                
                // ── Nested cascade bias ──
                // Override room direction when nested model signal is confident enough
                // Model is trained to 55.7% WR on 4-hr BTC — overrides noisy 1-min agent votes
                float confidence = (float)(yv > nv ? yv : nv) / (float)(yv + nv);
                confidence = (confidence - 0.5f) * 2.0f;
                if (g_nested) {
                    double nest_signal = (g_nested_prediction - 0.5) * 2.0;  // -1 to 1
                    if (fabs(nest_signal) > 0.20) {  // threshold: model must be >60% confident
                        bool nest_up = g_nested_prediction > 0.5;
                        if (nest_up != room_direction) {
                            room_direction = nest_up;
                            confidence = (float)fabs(nest_signal);
                            if (confidence > 1.0f) confidence = 1.0f;
                        }
                    }
                }
                
                float stake = state->room_capital * (0.01f + confidence * 0.04f) * state->stats.hedge_factor;
                if (stake > state->room_capital * 0.05f) stake = state->room_capital * 0.05f;
                if (stake < 0.01f) stake = 0.01f;
                state->room_capital -= stake;
                // ── T20: Entry slippage on room trade ──
                float slip_cost = stake * (SLIPPAGE_BPS + stake * SLIPPAGE_VOL_SCALE) / 10000.0f;
                if (slip_cost > state->room_capital * 0.5f) slip_cost = state->room_capital * 0.5f;
                if (slip_cost > 0.001f) {
                    state->room_capital -= slip_cost;
                    state->total_slippage_paid += slip_cost;
                    state->slippage_events++;
                }
                state->room_trades++;
                state->room_trade.window_ts = tick.window_ts;
                state->room_trade.yes_votes = yv;
                state->room_trade.no_votes = nv;
                state->room_trade.total_votes = vote_count;
                state->room_trade.majority_up = room_direction;
                state->room_trade.conviction_spread = 1.0f - confidence;
                state->room_trade.stake = stake;
                state->room_trade.entry_price = tick.close;
                state->room_trade.exit_price = 0;
                state->room_trade.won = false;
                state->room_trade.pnl = 0;
                state->room_trade.resolved_at = 0;
            }
        }
        
        // ── L4a: Resolve room trade (if active from previous cycle) ──
        if (state->room_trade.resolved_at == 0 && prev_close > 0) {
            // Room trade resolves: exit when close > prev_close = yes_won
            bool up = state->room_trade.majority_up;
            bool room_won = (tick.close >= prev_close) == up;
            
            if (room_won) {
                // Winner: get stake back + profit (binary: 1:1 payout minus taker fee)
                float profit = state->room_trade.stake * (1.0f - TAKER_FEE);
                float gross_ret = state->room_trade.stake + profit;
                // ── T20: Exit slippage on room trade winner ──
                float exit_slip = gross_ret * (SLIPPAGE_BPS + gross_ret * SLIPPAGE_VOL_SCALE) / 10000.0f;
                state->room_capital += gross_ret - exit_slip;
                state->total_slippage_paid += exit_slip;
                state->slippage_events++;
                state->room_wins++;
                state->room_trade.won = true;
                state->room_trade.pnl = profit;
                state->consec_room_losses = 0;  // Reset on win
            } else {
                // Loser: lose stake + fee
                state->room_losses++;
                state->room_trade.won = false;
                state->room_trade.pnl = -(state->room_trade.stake * (1.0f + TAKER_FEE));
                state->room_capital += state->room_trade.pnl;  // capital already deducted
                state->consec_room_losses++;  // Track consecutive losses
            }
            state->room_trade.exit_price = tick.close;
            state->room_trade.resolved_at = tick.window_ts;
            
            if (state->room_capital > state->room_capital_peak)
                state->room_capital_peak = state->room_capital;
        }

        // ── P27: Concept drift detection — rolling WR on room trades ──
        {
            static int drift_buf[100];  // Ring buffer: 1=win, 0=loss
            static int drift_idx = 0, drift_count = 0;
            if (state->room_trade.won) drift_buf[drift_idx] = 1;
            else drift_buf[drift_idx] = 0;
            drift_idx = (drift_idx + 1) % 100;
            if (drift_count < 100) drift_count++;
            if (drift_count >= 50 && drift_count % 10 == 0) {
                int wins = 0;
                for (int i = 0; i < drift_count; i++) wins += drift_buf[i];
                float rolling_wr = (float)wins / drift_count;
                // Expected WR ~55%. If rolling WR drops below 40% over 50+ trades, flag drift
                if (rolling_wr < 0.40f && state->cycle % 100 == 0) {
                    printf("[DRIFT] Rolling WR=%.1f%% over %d trades — concept drift possible\n",
                           rolling_wr * 100, drift_count);
                } else if (rolling_wr > 0.60f && state->cycle % 100 == 0) {
                    printf("[DRIFT] Rolling WR=%.1f%% over %d trades — regime shift positive\n",
                           rolling_wr * 100, drift_count);
                }
            }
        }
        
        // ── L4a old: Resolve previous window's P2P agent trades ──
        {
            int prev_tcount = state->trade_count;
            // Only resolve if we have a previous close to compare against
            if (prev_close > 0) {
                room_capital_resolve(state->trades, &prev_tcount, &tick,
                                     prev_close,
                                     state->agents, MAX_TRADE_HIST,
                                     &state->feat_importance);
            }
            // ── T20: P2P exit slippage — deduct from resolved winners ──
            if (prev_close > 0) {
                for (int i = 0; i < state->trade_count && i < MAX_TRADE_HIST; i++) {
                    if (state->trades[i].resolved_at == tick.window_ts && state->trades[i].won) {
                        float payout = state->trades[i].position_size * (1.0f + state->trades[i].pnl_pct);
                        if (payout <= 0) continue;
                        float slip_pct = (SLIPPAGE_BPS + payout * SLIPPAGE_VOL_SCALE) / 10000.0f;
                        float slip_cost = payout * slip_pct;
                        if (slip_cost < 0.001f) continue;
                        int aid = state->trades[i].agent_id;
                        if (aid >= 0 && aid < MAX_AGENTS && state->agents[aid].capital >= slip_cost) {
                            state->agents[aid].capital -= slip_cost;
                            state->total_slippage_paid += slip_cost;
                            state->slippage_events++;
                        }
                    }
                }
            }
        }
        
        // ── L4b: Apply capital allocation for NEW trades ──
        // ── T18: Position limit enforcement ──
        // Compute total capital of alive agents for global position limits
        float total_alive_cap = 0.0f;
        for (int i = 0; i < MAX_AGENTS; i++) {
            if (state->agents[i].alive)
                total_alive_cap += state->agents[i].capital;
        }
        float total_exposure = 0.0f;
        for (int i = 0; i < vote_count; i++) {
            int aid = state->votes[i].agent_id;
            float agent_cap = state->agents[aid].capital;
            if (agent_cap <= 0) continue;

            // Computed stake
            float stake = state->votes[i].position_size * agent_cap;
            // Cap per-agent position to max_position_pct_room of total capital
            float max_stake = total_alive_cap * state->max_position_pct_room;
            if (stake > max_stake) {
                float new_pct = max_stake / agent_cap;
                printf("[LIMIT] Agent %d: stake $%.2f capped to $%.2f (%.2f%% of room)\n",
                       aid, stake, max_stake, state->max_position_pct_room * 100);
                state->votes[i].position_size = new_pct;
                stake = max_stake;
            }

            // Cap total exposure across all agents
            float new_exposure = total_exposure + stake;
            float max_exposure = total_alive_cap * state->max_total_exposure_pct;
            if (new_exposure > max_exposure) {
                float remaining = max_exposure - total_exposure;
                if (remaining <= 0) {
                    state->votes[i].position_size = 0; // Skip this vote
                    printf("[LIMIT] Agent %d: skipped (total exposure capped at %.1f%%)\n",
                           aid, state->max_total_exposure_pct * 100);
                    continue;
                }
                float new_pct = remaining / agent_cap;
                state->votes[i].position_size = new_pct;
                stake = remaining;
            }
            total_exposure += stake;
        }
        state->current_total_exposure = total_exposure;
        if (total_exposure > state->peak_total_exposure)
            state->peak_total_exposure = total_exposure;

        int new_trades = 0;
        err = room_capital_apply(state->votes, vote_count, state->agents, MAX_AGENTS,
                                 state->trades, state->trade_count, &new_trades,
                                 tick.window_ts);
        // ── T19: Trade rate limiting ──
        if (state->max_trades_per_cycle > 0 && new_trades > state->max_trades_per_cycle) {
            int deferred = new_trades - state->max_trades_per_cycle;
            state->trades_deferred = deferred;
            state->total_trades_deferred += deferred;
            // Roll back deferred trades: return capital to agents whose trades were deferred
            for (int i = state->trade_count + state->max_trades_per_cycle;
                 i < state->trade_count + new_trades && i < MAX_TRADE_HIST; i++) {
                int aid = state->trades[i].agent_id;
                state->agents[aid].capital += state->trades[i].position_size;
                state->agents[aid].trades--;
            }
            new_trades = state->max_trades_per_cycle;
            printf("[QUEUE] %d trades deferred (max %d/cycle). Total deferred: %d\n",
                   deferred, state->max_trades_per_cycle, state->total_trades_deferred);
        } else {
            state->trades_deferred = 0;
        }
        if (new_trades > 0) state->trade_count += new_trades;

        // ── T20: P2P entry slippage — deduct from each new trade's agent capital ──
        {
            int start = state->trade_count - new_trades;
            if (start < 0) start = 0;
            for (int i = start; i < state->trade_count && i < MAX_TRADE_HIST; i++) {
                float stake = state->trades[i].position_size;
                float slip_pct = (SLIPPAGE_BPS + stake * SLIPPAGE_VOL_SCALE) / 10000.0f;
                float slip_cost = stake * slip_pct;
                if (slip_cost < 0.001f) continue;
                int aid = state->trades[i].agent_id;
                if (aid >= 0 && aid < MAX_AGENTS && state->agents[aid].capital >= slip_cost) {
                    state->agents[aid].capital -= slip_cost;
                    state->total_slippage_paid += slip_cost;
                    state->slippage_events++;
                }
            }
        }

        // ── Save close for next cycle's resolution ──
        prev_close = tick.close;

        // ── L5: Darwin evolution (every 100 trades) ──
        if (state->trade_count > 0 && state->trade_count % 100 == 0) {
            room_darwin_evolve(state->agents, MAX_AGENTS, state->cycle, &state->darwin);
            // C19: Compute diversity metrics after evolution
            room_darwin_compute_diversity(state->agents, MAX_AGENTS, &state->stats);

            // ── C39: Size scaling — compound position sizes based on agent WR ──
            int scaled = 0;
            for (int i = 0; i < MAX_AGENTS; i++) {
                if (!state->agents[i].alive) continue;
                if (state->agents[i].trades < 20) continue;
                float wr = (float)state->agents[i].wins / state->agents[i].trades;
                float old = state->agents[i].genome.position_size;
                if (wr > 0.55f) {
                    // Winning agent: grow position 10%, cap at 0.50
                    state->agents[i].genome.position_size *= 1.10f;
                    if (state->agents[i].genome.position_size > 0.50f)
                        state->agents[i].genome.position_size = 0.50f;
                } else if (wr < 0.45f) {
                    // Losing agent: shrink position 10%, floor at 0.01
                    state->agents[i].genome.position_size *= 0.90f;
                    if (state->agents[i].genome.position_size < 0.01f)
                        state->agents[i].genome.position_size = 0.01f;
                }
                if (fabsf(state->agents[i].genome.position_size - old) > 0.001f)
                    scaled++;
            }
            if (scaled > 0)
                printf("[SCALE] Size-scaling active: %d/%d agents adjusted\n",
                       scaled, state->stats.active_agents);
        }

skip_trading:
        // ── Update stats ──
        state->cycle++;
        state->stats.last_window_ts = tick.window_ts;
        state->last_updated = ns_now();
        
        // Compute aggregate stats
        RoomStats *s = &state->stats;
        s->cycle_count = state->cycle;
        
        // Set initial capital on first cycle
        if (s->initial_capital <= 0) {
            s->initial_capital = 0;
            for (int i = 0; i < MAX_AGENTS; i++)
                s->initial_capital += state->agents[i].starting_capital;
        }
        
        float total_cap = 0.0f;
        float conv_sum = 0.0f;
        float peak = 0.0f;
        int alive_agents = 0;
        for (int i = 0; i < MAX_AGENTS; i++) {
            total_cap += state->agents[i].capital;  // ALL agents, dead or alive
            if (state->agents[i].alive) {
                alive_agents++;
                if (state->agents[i].capital > peak)
                    peak = state->agents[i].capital;
            }
        }
        s->capital_current = total_cap;
        s->active_agents = alive_agents;
        // Track room-level PnL (the real seed money)
        float room_pnl_pct = state->room_capital_peak > 0 ? 
            ((state->room_capital - 50.0f) / 50.0f) * 100.0f : 0;
        s->room_pnl_pct = room_pnl_pct;
        
        // Track per-cycle room return for Sharpe (based on room_capital)
        if (state->prev_room_capital > 0 && state->cycle > 1 && state->room_trades > 0) {
            float cycle_return = (state->room_capital - state->prev_room_capital) / state->prev_room_capital;
            s->cycle_returns[s->return_idx] = cycle_return;
            s->return_idx = (s->return_idx + 1) % 128;
            if (s->return_count < 128) s->return_count++;
        }
        state->prev_room_capital = state->room_capital;
        
        // Conviction sum for spread calculation
        for (int i = 0; i < vote_count && i < MAX_AGENTS; i++) {
            conv_sum += state->votes[i].conviction;
        }
        
        if (total_cap > s->capital_peak) s->capital_peak = total_cap;
        if (s->capital_peak > 0)
            s->max_drawdown = (s->capital_peak - total_cap) / s->capital_peak;
        
        // Win rate from aggregate
        int total_w = 0, total_l = 0;
        for (int i = 0; i < state->trade_count && i < MAX_TRADE_HIST; i++) {
            if (state->trades[i].won) total_w++;
            else total_l++;
        }
        s->trades_total = total_w + total_l;
        s->trades_won = total_w;
        s->trades_lost = total_l;
        if (s->trades_total > 0) s->win_rate = (float)total_w / s->trades_total;
        
        // Conviction spread
        if (vote_count > 1) {
            float mean = conv_sum / vote_count;
            float var = 0;
            for (int i = 0; i < vote_count; i++) {
                float d = state->votes[i].conviction - mean;
                var += d * d;
            }
            s->consensus_spread = sqrtf(var / vote_count);
        }
        s->voted_this_cycle = vote_count;
        s->avg_conviction = vote_count > 0 ? conv_sum / vote_count : 0;
        
        // Compute Sharpe ratio from cycle returns (annualized)
        if (s->return_count >= 3) {
            float mean_r = 0, var_r = 0;
            int n = s->return_count < 128 ? s->return_count : 128;
            for (int i = 0; i < n; i++)
                mean_r += s->cycle_returns[i];
            mean_r /= n;
            for (int i = 0; i < n; i++) {
                float d = s->cycle_returns[i] - mean_r;
                var_r += d * d;
            }
            float std_r = sqrtf(var_r / n);
            if (std_r > 1e-10f) {
                // Annualized for 1-min data: 525600 cycles/year
                float periods_per_year = 525600.0f;
                s->sharpe_ratio = (mean_r / std_r) * sqrtf(periods_per_year);
            }
        }
        
        // ── L6: Write to bridge ──
        state->writing = 0;
        room_bridge_write(state);
        
        // ── Log to CSV ──
        FILE *log = fopen(LOG_PATH, "a");
        if (log) {
            float room_wr = state->room_wins + state->room_losses > 0 ?
                (float)state->room_wins / (state->room_wins + state->room_losses) : 0;
            fprintf(log, "%d,%ld,%s,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.2f,%.4f\n",
                    state->cycle, tick.window_ts, tick.asset,
                    state->vote_count, s->active_agents,
                    s->win_rate, s->sharpe_ratio, s->max_drawdown,
                    s->consensus_spread, s->room_pnl_pct,
                    state->room_trades, room_wr, state->room_capital,
                    state->total_slippage_paid);
            fclose(log);
        }
        
        // ── Check timing ──
        int64_t elapsed = ns_now() - cycle_start;
        if (elapsed > 100000000LL) { // >100ms
            printf("[ROOM] WARN: cycle %d took %.1fms (>100ms target)\n",
                   state->cycle, elapsed / 1e6);
        } else if (state->cycle % 100 == 0) {
            printf("[ROOM] cycle=%d agents=%d votes=%d win_rate=%.1f%% cap=$%.4f time=%.1fms\n",
                   state->cycle, s->active_agents, vote_count,
                   s->win_rate * 100, total_cap, elapsed / 1e6);
        }
        
        // ── Pace: faster for paper mode ──
        int64_t sleep_ns = PAPER_PACE_NS - elapsed;
        if (sleep_ns < 0) sleep_ns = 0;
        if (sleep_ns > 0) {
            struct timespec ts = {
                .tv_sec = sleep_ns / 1000000000LL,
                .tv_nsec = sleep_ns % 1000000000LL
            };
            nanosleep(&ts, NULL);
        }
    }
    
    printf("\n[ROOM] Shutdown. %d cycles run, %d trades\n",
           state->cycle, state->trade_count);
    
    if (g_nested) {
        nested_free(g_nested);
        g_nested = NULL;
        printf("[ROOM] Nested models freed\n");
    }
    
    munmap(state, sizeof(RoomState));
    close(state_fd);
    return 0;
}
