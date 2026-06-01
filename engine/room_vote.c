/**
 * room_vote.c — L3: 10K Agent Voting Engine (v2)
 * Each agent is a learned linear model: signal = dot(feat_weight, features) + bias
 * Feature weights evolve via Darwin + online SGD after each trade.
 * Hidden state persists across trades for recurrent memory.
 *
 * v2 changes:
 * - Removed hardcoded feature weights — now per-agent learned weights
 * - Added hidden state (4 floats) per agent — updated each cycle
 * - SGD update happens in room_capital.c after trade resolution
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "types.h"

#define SIGMA_NORMALIZER  0.001f  // Amplify small 1-min BTC signals
#define SIGMOID_SCALE     2.5f   // Sharper sigmoid for conviction diversity

static inline float sigmoid(float x) {
    if (x < -10.0f) return 0.0f;
    if (x > 10.0f) return 1.0f;
    return 1.0f / (1.0f + expf(-x));
}

// P15: Tailslayer — beam-search scenario evaluation
// When tail risk is elevated, agent conviction must be proportionally higher to trade.
// This simulates evaluating multiple future scenarios and only acting if conviction
// survives the beam width (1+tail_risk). Acts as a beam-search ensemble filter.
static float tailslayer_threshold(float base_conviction_threshold, float tail_risk) {
    // Base threshold is scaled by (1 + tail_risk), so at tail_risk=1.0,
    // effective threshold is 2× normal. At tail_risk=0, threshold is unchanged.
    // Clamps at 0.80 max to prevent total gridlock.
    float threshold = base_conviction_threshold * (1.0f + tail_risk);
    if (threshold > 0.80f) threshold = 0.80f;
    return threshold;
}

/**
 * Compute agent signal as learned dot product + bias.
 * Each agent has its own feat_weight[N_FEATURES] and bias,
 * evolved by Darwin and refined by online SGD.
 */
static float compute_agent_signal(const Genome *g, const FeatureVector *fv) {
    float *features = (float*)fv;  // FeatureVector is 13 consecutive floats
    // ── P22: Use regime-specific weights ──
    int regime = (int)(fv->regime_indicator + 0.5f);  // Round to nearest 0,1,2
    if (regime < 0) regime = 0;
    if (regime >= N_REGS) regime = N_REGS - 1;
    float signal = g->regime_bias[regime];
    for (int i = 0; i < N_FEATURES; i++) {
        signal += g->regime_weight[regime][i] * features[i];
    }

    // ── Modulation by genome meta-params ──
    float horizon_w = fmaxf(0.1f, g->time_horizon) / 5.0f;
    signal *= horizon_w;

    // ── Regime gating ──
    if (fv->regime_indicator == 1.0f)
        signal *= 1.3f;
    else if (fv->regime_indicator == 0.0f)
        signal *= 0.7f;

    // ── Herd contrarian bias ──
    signal -= (fv->herd_consensus - 0.5f) * g->herd_antipathy * 0.20f;

    return signal;
}

/**
 * Update agent's recurrent hidden state each cycle.
 * Simple RNN step: h = 0.9 * h + 0.1 * tanh(signal)
 * This gives agents memory of past predictions.
 */
static void update_hidden_state(AgentState *agent, float signal) {
    float activation = tanhf(signal);
    for (int i = 0; i < 4; i++) {
        // Each hidden dim mixes prior state with current signal
        agent->hidden[i] = 0.9f * agent->hidden[i] + 0.1f * activation;
    }
}

/**
 * Initialize genome weights to sensible defaults (v1-compatible starting point).
 */
void init_genome_weights(Genome *g) {
    // Start with the v1 hardcoded weights as initial prior
    // Feature order: price_delta, micro_momentum, rsi_7, volume_surge,
    //                ema_fast, ema_slow, macd_hist, bollinger_pct,
    //                divergence_score, pump_score, regime_indicator,
    //                fear_greed_norm, herd_consensus
    float default_weights[N_FEATURES] = {
        0.15f,  // price_delta_pct
        0.10f,  // micro_momentum
        0.08f,  // rsi_7 (normalized /50)
        0.06f,  // volume_surge_ratio
        0.05f,  // ema_fast
        0.03f,  // ema_slow
        0.02f,  // macd_hist
        0.10f,  // bollinger_pct (applied as 0.5 - pct)
        0.04f,  // divergence_score
        -0.12f, // pump_score (negative = avoid crony)
        0.00f,  // regime_indicator (gating applied separately)
        -0.05f, // fear_greed_norm
        0.00f,  // herd_consensus (gating applied separately)
    };
    memcpy(g->feat_weight, default_weights, sizeof(default_weights));
    g->bias = 0.0f;
    g->learning_rate = 0.01f;
}

RoomError room_vote_run(AgentState *agents, int n,
                        const FeatureVector *fv,
                        VoteRecord *votes, int *count) {
    *count = 0;
    if (!agents || !fv || !votes) return ERR_NO_AGENTS;

    for (int i = 0; i < n; i++) {
        if (!agents[i].alive) continue;

        const Genome *g = &agents[i].genome;
        float raw = compute_agent_signal(g, fv);
        float z = raw / SIGMA_NORMALIZER;
        float conviction = sigmoid(z * SIGMOID_SCALE);
        bool direction = conviction >= 0.5f;

        // ── Update hidden state ──
        update_hidden_state(&agents[i], raw);

        float conv_strength = fabsf(conviction - 0.5f) * 2.0f;
        // P15: Tailslayer beam-search gating — scale threshold by tail risk
        float effective_threshold = tailslayer_threshold(g->conviction_threshold, fv->tail_risk_score);
        if (conv_strength < effective_threshold) continue;

        float edge = direction ? conviction : (1.0f - conviction);
        float edge_pct = (edge - 0.5f) * 200.0f;
        if (edge_pct < g->min_edge_pct) continue;

        // Store features + conviction for SGD update at resolution time
        memcpy(agents[i].last_features, (float*)fv, sizeof(float) * N_FEATURES);
        agents[i].last_conviction = conviction;
        agents[i].last_trade_window = -1;  // Will be set when trade is recorded

        VoteRecord *v = &votes[*count];
        v->agent_id = i;
        v->voted = true;
        v->direction = direction;
        v->conviction = conviction;
        v->position_size = g->position_size * g->risk_tolerance;

        // ── P24: Kelly criterion override — optimal bet sizing from win rate ──
        {
            float wr = agents[i].win_rate_ema;
            // Full Kelly for 1:1 payout: f* = 2*WR - 1
            float kelly_raw = 2.0f * wr - 1.0f;
            if (kelly_raw < 0.0f) kelly_raw = 0.0f;
            // Fractional Kelly (0.25) for safety — use genome's risk_tolerance as fraction
            float kelly_frac = g->risk_tolerance * 0.25f;
            float kelly_pos = kelly_raw * kelly_frac;
            // Cap at genome's max position size
            float max_pos = g->position_size * 0.5f;
            if (kelly_pos > max_pos) kelly_pos = max_pos;
            // Blend: use Kelly fraction when agent has enough trades for reliable WR
            if (agents[i].trades >= 20) {
                v->position_size = kelly_pos;
            }
        }

        (*count)++;
    }

    return ERR_OK;
}
