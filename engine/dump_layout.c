
#include <stdio.h>
#include <stddef.h>
#include "types.h"

int main() {
    AgentState as;
    printf("sizeof(bool) = %zu\n", sizeof(bool));
    printf("sizeof(Genome) = %zu\n", sizeof(Genome));
    printf("sizeof(AgentState) = %zu\n", sizeof(AgentState));
    printf("\nGenome breakdown:\n");
    printf("  position_size:        offset %zu\n", offsetof(Genome, position_size));
    printf("  conviction_threshold: offset %zu\n", offsetof(Genome, conviction_threshold));
    printf("  risk_tolerance:       offset %zu\n", offsetof(Genome, risk_tolerance));
    printf("  time_horizon:         offset %zu\n", offsetof(Genome, time_horizon));
    printf("  mean_reversion_bias:  offset %zu\n", offsetof(Genome, mean_reversion_bias));
    printf("  feat_weight:          offset %zu (size %zu)\n", offsetof(Genome, feat_weight), sizeof(((Genome*)0)->feat_weight));
    printf("  bias:                 offset %zu\n", offsetof(Genome, bias));
    printf("  learning_rate:        offset %zu\n", offsetof(Genome, learning_rate));
    printf("  regime_weight:        offset %zu (size %zu)\n", offsetof(Genome, regime_weight), sizeof(((Genome*)0)->regime_weight));
    printf("  regime_bias:          offset %zu\n", offsetof(Genome, regime_bias));
    printf("\nAgentState breakdown:\n");
    printf("  alive:                offset %zu\n", offsetof(AgentState, alive));
    printf("  genome:               offset %zu (size %zu)\n", offsetof(AgentState, genome), sizeof(((AgentState*)0)->genome));
    printf("  capital:              offset %zu\n", offsetof(AgentState, capital));
    printf("  starting_capital:     offset %zu\n", offsetof(AgentState, starting_capital));
    printf("  trades:               offset %zu\n", offsetof(AgentState, trades));
    printf("  wins:                 offset %zu\n", offsetof(AgentState, wins));
    printf("  losses:               offset %zu\n", offsetof(AgentState, losses));
    printf("  total_pnl:            offset %zu\n", offsetof(AgentState, total_pnl));
    printf("  max_drawdown:         offset %zu\n", offsetof(AgentState, max_drawdown));
    printf("  peak_capital:         offset %zu\n", offsetof(AgentState, peak_capital));
    printf("  consecutive_losses:   offset %zu\n", offsetof(AgentState, consecutive_losses));
    printf("  win_rate_ema:         offset %zu\n", offsetof(AgentState, win_rate_ema));
    printf("  last_trade_window:    offset %zu\n", offsetof(AgentState, last_trade_window));
    printf("  hidden:               offset %zu\n", offsetof(AgentState, hidden));
    printf("  last_conviction:      offset %zu\n", offsetof(AgentState, last_conviction));
    printf("  last_features:        offset %zu (N=%d)\n", offsetof(AgentState, last_features), N_FEATURES);
    printf("  conv_hi_wins:         offset %zu\n", offsetof(AgentState, conv_hi_wins));
    printf("  weight_mag:           offset %zu\n", offsetof(AgentState, weight_mag));
    printf("\nRoomState breakdown:\n");
    printf("  sizeof(RoomState) = %zu\n", sizeof(RoomState));
    printf("  magic:                offset %zu\n", offsetof(RoomState, magic));
    printf("  cycle:                offset %zu\n", offsetof(RoomState, cycle));
    printf("  agents:               offset %zu\n", offsetof(RoomState, agents));
    return 0;
}
