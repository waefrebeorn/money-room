/**
 * room_market.c — MARKET_MODE: Market Maker Engine
 * 
 * Replaces zero-sum P2P matching with market-maker trading.
 * Each agent trades against a market maker who sets bid/ask
 * prices from the live market feed:
 * 
 *   - If candle closes up (close > open):  market_prob = 0.55
 *   - If candle closes down (close < open): market_prob = 0.45
 *   - Spread = |ask - bid| = 10% (market maker edge)
 * 
 * Agent pays:    stake * price             (entry cost)
 * On win:        stake / entry_price       (1/price leverage)
 * On loss:       0 (stake forfeited to room)
 * 
 * Room capital grows from the spread + losers' stakes.
 * This breaks zero-sum — total grows with market alignment.
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "types.h"

// ── Market maker pricing ──
// Derives YES/NO probability from the live market feed
static float market_price(bool yes, const MarketTick *tick) {
    // If candle is bullish (close > open), higher probability of continuation
    float prob_up = 0.50f;
    float hl_range = tick->high - tick->low;
    if (hl_range > 0.001f) {
        // Position within the candle: where did close land?
        float close_pos = (tick->close - tick->low) / hl_range;
        // Map [0,1] to [0.45, 0.55] — market maker keeps 10% spread
        prob_up = 0.45f + close_pos * 0.10f;
    }
    return yes ? prob_up : (1.0f - prob_up);
}

// ════════════════════════════════════════════════════════
//  MARKET APPLY — Execute trades against market maker
//  Each agent buys YES or NO at market-implied prices.
//  Room holds the opposite position (market maker).
// ════════════════════════════════════════════════════════
RoomError room_market_apply(VoteRecord *votes, int count,
                            AgentState *agents, int n_unused,
                            TradeRecord *trades, int start_offset,
                            int *new_count, int64_t window_ts) {
    (void)n_unused;
    *new_count = 0;
    if (count < 1) return ERR_OK;

    int max_new = MAX_TRADE_HIST - start_offset;
    if (max_new <= 0) return ERR_OK;

    int trade_idx = start_offset;
    for (int i = 0; i < count && trade_idx < start_offset + max_new; i++) {
        int aid = votes[i].agent_id;
        AgentState *a = &agents[aid];
        if (!a->alive || a->capital <= 0) continue;

        // Determine position size (5% max per trade)
        float stake = votes[i].position_size * a->capital;
        float max_loss = a->capital * 0.05f;
        if (stake > max_loss) stake = max_loss;
        if (stake > a->capital * 0.5f) stake = a->capital * 0.5f;
        if (stake <= 0) continue;

        // NOTE: We don't have the MarketTick here yet — will get it from resolution
        // For now, derive from prev/next context; price stored on trade record
        // Use 0.5 as placeholder — real price applied at resolve
        float price = votes[i].direction ? 0.55f : 0.45f;

        // Deduct stake (entry cost for the position)
        float cost = stake * price;
        if (cost > a->capital) cost = a->capital;
        a->capital -= cost;
        if (a->capital < 0) a->capital = 0;
        a->trades++;
        a->last_trade_window = (int)window_ts;

        trades[trade_idx].window_ts = window_ts;
        trades[trade_idx].agent_id = aid;
        trades[trade_idx].direction = votes[i].direction;
        trades[trade_idx].position_size = stake;       // Notional exposure
        trades[trade_idx].entry_price = price;          // Price paid (market implied)
        trades[trade_idx].exit_price = 0;
        trades[trade_idx].pnl_pct = 0;
        trades[trade_idx].won = false;
        trades[trade_idx].resolved_at = 0;
        strncpy(trades[trade_idx].asset, "MKT", 7);
        trade_idx++;
    }

    *new_count = trade_idx - start_offset;
    return ERR_OK;
}

// ════════════════════════════════════════════════════════
//  MARKET RESOLVE — Settle market maker trades
//  Winners get: stake / entry_price (return of cost + profit)
//  Losers get: 0 (stake forfeited to room market maker)
//  Room capital grows from losers' forfeited stakes + spread.
// ════════════════════════════════════════════════════════
RoomError room_market_resolve(TradeRecord *trades, int *tcount,
                              const MarketTick *tick,
                              float prev_close,
                              AgentState *agents,
                              int max_trades,
                              float *room_capital) {
    int n = *tcount < max_trades ? *tcount : max_trades;
    if (n == 0) return ERR_OK;

    bool market_up = tick->close >= prev_close;

    // Collect unresolved trades from windows before current tick
    float loser_pool = 0;
    float correct_notional = 0;
    int correct_count = 0;

    for (int i = 0; i < n; i++) {
        if (trades[i].resolved_at != 0) continue;
        if (trades[i].window_ts >= tick->window_ts) continue;

        bool agent_up = trades[i].direction;
        bool won = (agent_up && market_up) || (!agent_up && !market_up);
        float stake = trades[i].position_size;
        float price = trades[i].entry_price;
        if (price <= 0) price = 0.5f;

        if (won) {
            // Payout = stake / entry_price (the 1/price leverage)
            float payout = stake / price;
            float profit = payout - stake;  // Net profit after returning stake
            correct_notional += stake;
            correct_count++;

            int aid = trades[i].agent_id;
            if (aid >= 0 && aid < MAX_AGENTS) {
                agents[aid].capital += payout;
                agents[aid].wins++;
                agents[aid].consecutive_losses = 0;
                trades[i].won = true;
                trades[i].pnl_pct = profit / payout;
                trades[i].exit_price = tick->close;
                trades[i].resolved_at = tick->window_ts;

                // SGD update (same as P2P resolve, P22: per-regime)
                int sgd_regime_mkt = (int)(agents[aid].last_features[11] + 0.5f);
                if (sgd_regime_mkt < 0) sgd_regime_mkt = 0;
                if (sgd_regime_mkt >= N_REGS) sgd_regime_mkt = N_REGS - 1;
                if (agents[aid].last_conviction > 0.0f) {
                    float error = 1.0f - agents[aid].last_conviction;
                    float lr = agents[aid].genome.learning_rate;
                    float step = lr * error * 0.01f;
                    if (step > 0.1f) step = 0.1f;
                    if (step < -0.1f) step = -0.1f;
                    for (int fi = 0; fi < N_FEATURES; fi++) {
                        agents[aid].genome.regime_weight[sgd_regime_mkt][fi] += step * agents[aid].last_features[fi];
                        if (agents[aid].genome.regime_weight[sgd_regime_mkt][fi] > 1.0f)
                            agents[aid].genome.regime_weight[sgd_regime_mkt][fi] = 1.0f;
                        if (agents[aid].genome.regime_weight[sgd_regime_mkt][fi] < -1.0f)
                            agents[aid].genome.regime_weight[sgd_regime_mkt][fi] = -1.0f;
                    }
                    agents[aid].genome.regime_bias[sgd_regime_mkt] += step * 1.0f;
                }
            }
        } else {
            // Loser: stake stays forfeited to the market maker (room)
            loser_pool += stake;
            if (room_capital) *room_capital += stake * (1.0f - MARKET_TAKER_FEE);

            int aid = trades[i].agent_id;
            if (aid >= 0 && aid < MAX_AGENTS) {
                agents[aid].losses++;
                agents[aid].consecutive_losses++;
                if (agents[aid].consecutive_losses >= 6)
                    agents[aid].alive = false;
                trades[i].won = false;
                trades[i].pnl_pct = -1.0f;
                trades[i].exit_price = tick->close;
                trades[i].resolved_at = tick->window_ts;
            }
        }

        // Track PnL and drawdown
        if (trades[i].resolved_at) {
            float pnl = trades[i].won ?
                (trades[i].position_size / trades[i].entry_price - trades[i].position_size) :
                -trades[i].position_size;
            int aid = trades[i].agent_id;
            if (aid >= 0 && aid < MAX_AGENTS) {
                agents[aid].total_pnl += pnl;
                if (agents[aid].capital > agents[aid].peak_capital)
                    agents[aid].peak_capital = agents[aid].capital;
                float dd = (agents[aid].peak_capital - agents[aid].capital) /
                           (agents[aid].peak_capital + 0.001f);
                if (dd > agents[aid].max_drawdown)
                    agents[aid].max_drawdown = dd;
                agents[aid].win_rate_ema = agents[aid].win_rate_ema * 0.9f +
                    (trades[i].won ? 1.0f : 0.0f) * 0.1f;
            }
        }
    }

    printf("[MARKET] resolved=%d correct=%d loser_pool=$%.2f room_cap=$%.2f\n",
           n, correct_count, loser_pool, room_capital ? *room_capital : 0);

    return ERR_OK;
}

// ════════════════════════════════════════════════════════
//  MARKET STATS — Compute market maker PnL
// ════════════════════════════════════════════════════════
void room_market_stats(RoomState *state) {
    if (!state) return;
    float mm_pnl = 50.0f - state->room_capital;  // Initial seed - current
    printf("[MARKET] room_cap=$%.2f mm_pnl=$%.2f trades=%d wr=%.1f%%\n",
           state->room_capital, mm_pnl,
           state->room_trades,
           (state->room_wins + state->room_losses) > 0 ?
               (float)state->room_wins / (state->room_wins + state->room_losses) * 100 : 0);
}
