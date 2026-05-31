/**
 * room_bridge.c — L6: Python Dashboard Bridge
 * Exposes mmap'd RoomState for Python dashboard reader.
 * Python opens the same mmap file for zero-copy reads.
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "types.h"

#define STATE_PATH_BASE "/home/wubu2/.hermes/pm_logs/c_room"
#ifdef PAPER_MODE
#define STATE_PATH STATE_PATH_BASE "/room_state_paper.bin"
#else
#define STATE_PATH STATE_PATH_BASE "/room_state.bin"
#endif
#define JSON_SNAP  "/home/wubu2/.hermes/pm_logs/c_room/room_snapshot.json"

// ── Write a JSON snapshot for Python tools that can't read mmap ──
RoomError room_bridge_write(RoomState *state) {
    if (!state) return ERR_MMAP_FAIL;
    
    FILE *f = fopen(JSON_SNAP, "w");
    if (!f) return ERR_FILE_READ;
    
    fputs("{\n", f);
    
    // General
    fprintf(f, "\"magic\": %u,\n", state->magic);
    fprintf(f, "\"cycle\": %d,\n", state->cycle);
    fprintf(f, "\"last_updated\": %ld,\n", state->last_updated);
    
    // Current market
    fprintf(f, "\"market\": {\n");
    fprintf(f, "  \"asset\": \"%s\",\n", state->current_market.asset);
    fprintf(f, "  \"window_ts\": %ld,\n", state->current_market.window_ts);
    fprintf(f, "  \"open\": %.2f,\n", state->current_market.open);
    fprintf(f, "  \"high\": %.2f,\n", state->current_market.high);
    fprintf(f, "  \"low\": %.2f,\n", state->current_market.low);
    fprintf(f, "  \"close\": %.2f,\n", state->current_market.close);
    fprintf(f, "  \"volume\": %.2f,\n", state->current_market.volume);
    fprintf(f, "  \"pump_score\": %.4f,\n", state->current_market.pump_score);
    fprintf(f, "  \"fear_greed\": %.1f\n", state->current_market.fear_greed);
    fprintf(f, "},\n");
    
    // Features
    fprintf(f, "\"features\": {\n");
    fprintf(f, "  \"price_delta_pct\": %.4f,\n", state->features.price_delta_pct);
    fprintf(f, "  \"micro_momentum\": %.4f,\n", state->features.micro_momentum);
    fprintf(f, "  \"rsi_7\": %.1f,\n", state->features.rsi_7);
    fprintf(f, "  \"volume_surge_ratio\": %.2f,\n", state->features.volume_surge_ratio);
    fprintf(f, "  \"divergence_score\": %.4f,\n", state->features.divergence_score);
    fprintf(f, "  \"pump_score\": %.4f,\n", state->features.pump_score);
    fprintf(f, "  \"regime_indicator\": %.0f,\n", state->features.regime_indicator);
    fprintf(f, "  \"fear_greed_norm\": %.4f,\n", state->features.fear_greed_norm);
    fprintf(f, "  \"herd_consensus\": %.4f,\n", state->features.herd_consensus);
    fprintf(f, "  \"nested_prediction\": %.4f\n", state->nested_prediction);
    fprintf(f, "},\n");
    
    // Vote summary
    int up = 0, down = 0;
    float conv_sum = 0;
    for (int i = 0; i < state->vote_count; i++) {
        if (state->votes[i].direction) up++;
        else down++;
        conv_sum += state->votes[i].conviction;
    }
    fprintf(f, "\"vote_summary\": {\n");
    fprintf(f, "  \"total\": %d,\n", state->vote_count);
    fprintf(f, "  \"up\": %d,\n", up);
    fprintf(f, "  \"down\": %d,\n", down);
    fprintf(f, "  \"avg_conviction\": %.4f,\n", state->vote_count > 0 ? conv_sum / state->vote_count : 0);
    fprintf(f, "  \"consensus_spread\": %.4f\n", state->stats.consensus_spread);
    fprintf(f, "},\n");
    
    // Stats
    fprintf(f, "\"stats\": {\n");
    fprintf(f, "  \"active_agents\": %d,\n", state->stats.active_agents);
    fprintf(f, "  \"voted_this_cycle\": %d,\n", state->stats.voted_this_cycle);
    fprintf(f, "  \"trades_total\": %d,\n", state->stats.trades_total);
    fprintf(f, "  \"trades_won\": %d,\n", state->stats.trades_won);
    fprintf(f, "  \"trades_lost\": %d,\n", state->stats.trades_lost);
    fprintf(f, "  \"win_rate\": %.4f,\n", state->stats.win_rate);
    fprintf(f, "  \"sharpe_ratio\": %.4f,\n", state->stats.sharpe_ratio);
    fprintf(f, "  \"max_drawdown\": %.4f,\n", state->stats.max_drawdown);
    fprintf(f, "  \"capital_current\": %.4f,\n", state->stats.capital_current);
    fprintf(f, "  \"capital_peak\": %.4f,\n", state->stats.capital_peak);
    fprintf(f, "  \"room_pnl_pct\": %.4f,\n", state->stats.room_pnl_pct);
    fprintf(f, "  \"weight_diversity\": %.6f,\n", state->stats.weight_diversity);
    fprintf(f, "  \"genome_diversity\": %.6f\n", state->stats.genome_diversity);
    fputs("},\n", f);

    // ── P16: Feature Importance ──
    fprintf(f, "\"feature_importance\": [\n");
    const char *feat_names[N_FEATURES] = {
        "price_delta_pct", "micro_momentum", "rsi_7", "volume_surge_ratio",
        "ema_fast", "ema_slow", "macd_hist", "bollinger_pct",
        "divergence_score", "pump_score", "regime_indicator",
        "fear_greed_norm", "herd_consensus",
        "phi_return", "phi_vol", "phi_momentum",
        "dft_dominant", "tail_risk_score"
    };
    int first = 1;
    for (int i = 0; i < N_FEATURES; i++) {
        FeatureImportance *fi = &state->feat_importance;
        float pos_wr = fi->pos_contrib_total[i] > 0
            ? fi->pos_contrib_wins[i] / fi->pos_contrib_total[i] : 0.5f;
        float neg_wr = fi->neg_contrib_total[i] > 0
            ? fi->neg_contrib_wins[i] / fi->neg_contrib_total[i] : 0.5f;
        float importance = pos_wr - neg_wr;
        if (!first) fputs(",\n", f);
        first = 0;
        fprintf(f, "  {\"name\": \"%s\", \"pos_wr\": %.4f, \"neg_wr\": %.4f, \"importance\": %+.4f, \"pos_trades\": %d, \"neg_trades\": %d}",
                feat_names[i], pos_wr, neg_wr, importance,
                fi->pos_contrib_total[i], fi->neg_contrib_total[i]);
    }
    fputs("\n],\n", f);

    // Darwin
    fprintf(f, "\"darwin\": {\n");
    fprintf(f, "  \"epoch\": %d,\n", state->darwin.epoch);
    fprintf(f, "  \"culled\": %d,\n", state->darwin.culled);
    fprintf(f, "  \"cloned\": %d,\n", state->darwin.cloned);
    fprintf(f, "  \"mutation_rate\": %.4f\n", state->darwin.mutation_rate);
    fprintf(f, "},\n");
    
    // Top 10 agents by capital
    fprintf(f, "\"top_agents\": [\n");
    // Find top 10
    int top_ids[10] = {0};
    float top_caps[10] = {0};
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!state->agents[i].alive) continue;
        float cap = state->agents[i].capital;
        for (int j = 0; j < 10; j++) {
            if (cap > top_caps[j]) {
                // Shift down
                for (int k = 9; k > j; k--) {
                    top_ids[k] = top_ids[k-1];
                    top_caps[k] = top_caps[k-1];
                }
                top_ids[j] = i;
                top_caps[j] = cap;
                break;
            }
        }
    }
    for (int j = 0; j < 10 && top_caps[j] > 0; j++) {
        int aid = top_ids[j];
        fprintf(f, "  {\"id\": %d, \"capital\": %.6f, \"win_rate_ema\": %.4f, \"trades\": %d, \"wins\": %d, \"losses\": %d}%s\n",
                aid, state->agents[aid].capital, state->agents[aid].win_rate_ema,
                state->agents[aid].trades, state->agents[aid].wins, state->agents[aid].losses,
                j < 9 && top_caps[j+1] > 0 ? "," : "");
    }
    fprintf(f, "]\n");
    
    fputs("}\n", f);
    fclose(f);
    
    return ERR_OK;
}
