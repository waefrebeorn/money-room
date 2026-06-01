/**
 * room_capital.c — L4: Peer-to-Peer Capital Allocation
 * 
 * Zero-sum P2P matching. YES votes matched vs NO votes.
 * Winners split losers' stake. Fees deducted from matched portion only.
 * 
 * Flow per cycle:
 *   1. Count YES total stake vs NO total stake
 *   2. Match min(YES, NO) — unmatched portion never leaves agent's capital
 *   3. Deduct matched_stake + fee from each agent's capital
 *   4. On resolution: winners get matched_stake back + share of loser pool
 *                     Losers lose their matched_stake entirely
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "types.h"

typedef struct {
    int agent_id;
    float stake;
    float conviction;
} Staker;

// ════════════════════════════════════════════════════════
//  MATCH VOTES — pair YES vs NO agents, execute trades
//  CRITICAL: Only matched_stake is deducted from capital.
//  Unmatched surplus was never deducted — so NEVER returned.
//  This preserves zero-sum property.
// ════════════════════════════════════════════════════════
RoomError room_capital_apply(VoteRecord *votes, int count,
                             AgentState *agents, int n_unused,
                             TradeRecord *trades, int start_offset,
                             int *new_count, int64_t window_ts) {
    (void)n_unused;
    *new_count = 0;
    if (count < 2) return ERR_OK;

    int max_new = MAX_TRADE_HIST - start_offset;
    if (max_new <= 0) return ERR_OK;

    Staker *yes = (Staker *)malloc(count * sizeof(Staker));
    Staker *no = (Staker *)malloc(count * sizeof(Staker));
    if (!yes || !no) { free(yes); free(no); return ERR_FILE_READ; }

    int ny = 0, nn = 0;
    float yes_total = 0, no_total = 0;

    // ── Pass 1: collect stakes (no capital deduction yet) ──
    for (int i = 0; i < count; i++) {
        int aid = votes[i].agent_id;
        AgentState *a = &agents[aid];
        if (!a->alive || a->capital <= 0) continue;

        float stake = votes[i].position_size * a->capital;
        float max_loss = a->capital * 0.05f;
        if (stake > max_loss) stake = max_loss;
        if (stake > a->capital * 0.5f) stake = a->capital * 0.5f;
        if (stake < MIN_TRADE_STAKE) continue;  // T97: skip tiny trades
        if (stake <= 0) continue;

        if (votes[i].direction) {
            yes[ny].agent_id = aid;
            yes[ny].stake = stake;
            yes[ny].conviction = votes[i].conviction;
            yes_total += stake;
            ny++;
        } else {
            no[nn].agent_id = aid;
            no[nn].stake = stake;
            no[nn].conviction = votes[i].conviction;
            no_total += stake;
            nn++;
        }
    }

    if (ny == 0 || nn == 0) { free(yes); free(no); return ERR_OK; }
    if (yes_total <= 0 || no_total <= 0) { free(yes); free(no); return ERR_OK; }

    // ── Pass 2: match ──
    float matched = fminf(yes_total, no_total);
    float yes_ratio = matched / yes_total;
    float no_ratio = matched / no_total;

    // ── Pass 3: deduct matched_stake + fee, write trade records ──
    // NOTE: Unmatched surplus stays in agent's capital (never deducted).
    // We only deduct what's actually matched.
    int trade_idx = start_offset;
    
    for (int i = 0; i < ny && trade_idx < start_offset + max_new; i++) {
        float matched_stake = yes[i].stake * yes_ratio;  // Portion at risk
        float fee = matched_stake * TAKER_FEE;
        // NO surplus return — unmatched portion was never deducted

        AgentState *a = &agents[yes[i].agent_id];
        a->capital -= (matched_stake + fee);
        if (a->capital < 0) a->capital = 0;  // C1: capital floor
        a->trades++;
        a->last_trade_window = (int)window_ts;

        trades[trade_idx].window_ts = window_ts;
        trades[trade_idx].agent_id = yes[i].agent_id;
        trades[trade_idx].direction = true;
        trades[trade_idx].position_size = matched_stake;
        trades[trade_idx].entry_price = 0.5f;
        trades[trade_idx].exit_price = 0;
        trades[trade_idx].pnl_pct = 0;
        trades[trade_idx].won = false;
        trades[trade_idx].resolved_at = 0;
        strncpy(trades[trade_idx].asset, "ROOM", 7);
        trade_idx++;
    }

    for (int i = 0; i < nn && trade_idx < start_offset + max_new; i++) {
        float matched_stake = no[i].stake * no_ratio;
        float fee = matched_stake * TAKER_FEE;

        AgentState *a = &agents[no[i].agent_id];
        a->capital -= (matched_stake + fee);
        if (a->capital < 0) a->capital = 0;  // C1: capital floor
        a->trades++;
        a->last_trade_window = (int)window_ts;

        trades[trade_idx].window_ts = window_ts;
        trades[trade_idx].agent_id = no[i].agent_id;
        trades[trade_idx].direction = false;
        trades[trade_idx].position_size = matched_stake;
        trades[trade_idx].entry_price = 0.5f;
        trades[trade_idx].exit_price = 0;
        trades[trade_idx].pnl_pct = 0;
        trades[trade_idx].won = false;
        trades[trade_idx].resolved_at = 0;
        strncpy(trades[trade_idx].asset, "ROOM", 7);
        trade_idx++;
    }

    *new_count = trade_idx - start_offset;
    free(yes);
    free(no);
    return ERR_OK;
}

// ════════════════════════════════════════════════════════
//  RESOLVE — settle matched trades when market resolves
//  Uses close > prev_close (inter-candle direction).
//  Winners get matched_stake back + share of loser pool.
//  Losers' matched_stake stays gone (already deducted).
//  Zero-sum minus MATCH_FEE on loser pool.
// ════════════════════════════════════════════════════════
RoomError room_capital_resolve(TradeRecord *trades, int *tcount,
                               const MarketTick *resolution_tick,
                               float prev_close,
                               AgentState *agents,
                               int max_trades,
                               FeatureImportance *importance) {
    int n = *tcount < max_trades ? *tcount : max_trades;
    if (n == 0) return ERR_OK;

    bool yes_won = resolution_tick->close >= prev_close;

    // Collect unresolved trades from windows before current tick
    float yes_pool = 0, no_pool = 0;
    int yes_count = 0, no_count = 0;

    for (int i = 0; i < n; i++) {
        if (trades[i].resolved_at != 0) continue;
        if (trades[i].window_ts >= resolution_tick->window_ts) continue;

        if (trades[i].direction) {
            yes_pool += trades[i].position_size;
            yes_count++;
        } else {
            no_pool += trades[i].position_size;
            no_count++;
        }
    }

    if (yes_count == 0 && no_count == 0) return ERR_OK;

    float loser_pool = yes_won ? no_pool : yes_pool;
    float winner_pool = yes_won ? yes_pool : no_pool;
    float total_payout = loser_pool * (1.0f - MATCH_FEE);  // Fee on loser pool only

    for (int i = 0; i < n; i++) {
        if (trades[i].resolved_at != 0) continue;
        if (trades[i].window_ts >= resolution_tick->window_ts) continue;

        bool is_winner = trades[i].direction == yes_won;
        int aid = trades[i].agent_id;

        if (aid >= 0 && aid < MAX_AGENTS) {
            if (is_winner) {
                float share = winner_pool > 0 ? trades[i].position_size / winner_pool : 0;
                // Return matched_stake + share of loser pool (net of match fee)
                float payout = trades[i].position_size + total_payout * share;
                agents[aid].capital += payout;
                agents[aid].wins++;
                agents[aid].consecutive_losses = 0;
                // C10: Conviction accuracy tracking
                if (agents[aid].last_conviction > 0.7f) {
                    agents[aid].conv_hi_wins++;
                    agents[aid].conv_hi_total++;
                } else if (agents[aid].last_conviction < 0.3f) {
                    agents[aid].conv_lo_wins++;
                    agents[aid].conv_lo_total++;
                }
                trades[i].won = true;
                trades[i].pnl_pct = total_payout * share / trades[i].position_size;
            } else {
                // Loser's matched_stake stays deducted — transferred to winners
                agents[aid].losses++;
                agents[aid].consecutive_losses++;
                // C10: Conviction accuracy tracking (losses)
                if (agents[aid].last_conviction > 0.7f) {
                    agents[aid].conv_hi_total++;
                } else if (agents[aid].last_conviction < 0.3f) {
                    agents[aid].conv_lo_total++;
                }
                trades[i].won = false;
                trades[i].pnl_pct = -1.0f;
                if (agents[aid].consecutive_losses >= 6)
                    agents[aid].alive = false;
            }

            agents[aid].total_pnl += trades[i].pnl_pct * trades[i].position_size;
            if (agents[aid].capital > agents[aid].peak_capital)
                agents[aid].peak_capital = agents[aid].capital;

            float dd = (agents[aid].peak_capital - agents[aid].capital) / agents[aid].peak_capital;
            if (dd > agents[aid].max_drawdown)
                agents[aid].max_drawdown = dd;

            float wr = trades[i].won ? 1.0f : 0.0f;
            agents[aid].win_rate_ema = agents[aid].win_rate_ema * 0.9f + wr * 0.1f;

            // ── v2: Online SGD update ──
            // REINFORCE: w += lr * (actual - predicted) * feature
            // actual = 1.0 (won) or 0.0 (lost)
            // predicted = last_conviction (what agent predicted before trade)
            int sgd_regime = (int)(agents[aid].last_features[11] + 0.5f);
            if (sgd_regime < 0) sgd_regime = 0;
            if (sgd_regime >= N_REGS) sgd_regime = N_REGS - 1;
            if (agents[aid].last_conviction > 0.0f) {
                float error = (trades[i].won ? 1.0f : 0.0f) - agents[aid].last_conviction;
                float lr = agents[aid].genome.learning_rate;
                // Scale learning rate by importance of this trade
                float importance = trades[i].position_size / (agents[aid].capital + 1.0f);
                float step = lr * error * fmaxf(importance, 0.01f);
                // Clamp step to prevent wild updates
                if (step > 0.1f) step = 0.1f;
                if (step < -0.1f) step = -0.1f;
                // Update each feature weight (P22: per-regime, based on regime at trade time)
                for (int fi = 0; fi < N_FEATURES; fi++) {
                    agents[aid].genome.regime_weight[sgd_regime][fi] += step * agents[aid].last_features[fi];
                    if (agents[aid].genome.regime_weight[sgd_regime][fi] > 1.0f)
                        agents[aid].genome.regime_weight[sgd_regime][fi] = 1.0f;
                    if (agents[aid].genome.regime_weight[sgd_regime][fi] < -1.0f)
                        agents[aid].genome.regime_weight[sgd_regime][fi] = -1.0f;
                }
                // Update regime bias too
                agents[aid].genome.regime_bias[sgd_regime] += step * 1.0f;
            }

            // ── P16: Feature importance tracking ──
            // For each feature, track if its contribution aligned with the signal
            // direction and whether the trade won. Uses per-regime weights (P22).
            if (importance) {
                for (int fi = 0; fi < N_FEATURES; fi++) {
                    float contrib = agents[aid].last_features[fi] * agents[aid].genome.regime_weight[sgd_regime][fi];
                    if (fabsf(contrib) > 1e-6f) {
                        if (contrib > 0) {
                            // Feature pushed signal UP
                            if (trades[i].won)
                                importance->pos_contrib_wins[fi] += 1.0f;
                            importance->pos_contrib_total[fi]++;
                        } else {
                            // Feature pushed signal DOWN
                            if (trades[i].won)
                                importance->neg_contrib_wins[fi] += 1.0f;
                            importance->neg_contrib_total[fi]++;
                        }
                    }
                }
            }
        }

        trades[i].exit_price = resolution_tick->close;
        trades[i].resolved_at = resolution_tick->window_ts;
        
        // ── T16: Log resolved trade to CSV for post-hoc audit ──
        FILE *tlog = fopen("/home/wubu2/.hermes/pm_logs/c_room/trade_log.csv", "a");
        if (tlog) {
            // Write header on first open (file didn't exist before open in "a" mode)
            // We check file existence by seeing if position after fopen is at start
            // In "a" mode, ftell returns 0 for a new file, >0 for existing
            long pos = ftell(tlog);
            if (pos == 0) {
                fprintf(tlog, "ts,agent_id,direction,size,entry_price,exit_price,won,pnl_pct,resolved_at,asset\n");
            }
            fprintf(tlog, "%ld,%d,%s,%.4f,%.4f,%.4f,%s,%.6f,%ld,%s\n",
                    (long)trades[i].window_ts,
                    trades[i].agent_id,
                    trades[i].direction ? "YES" : "NO",
                    trades[i].position_size,
                    trades[i].entry_price,
                    trades[i].exit_price,
                    trades[i].won ? "WIN" : "LOSS",
                    trades[i].pnl_pct,
                    (long)trades[i].resolved_at,
                    trades[i].asset);
            fclose(tlog);
        }
    }

    return ERR_OK;
}
