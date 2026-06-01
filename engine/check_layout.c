// check_layout.c — print struct sizes for debug
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define N_FEATURES 48
#define MAX_AGENTS 10000
#define MAX_ASSETS 8
#define MAX_TRADE_HIST 1000000

typedef struct {
    float weights[18];
    float bias;
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
    float    last_features[18];
    float    conv_hi_wins;
    float    conv_hi_total;
    float    conv_lo_wins;
    float    conv_lo_total;
    float    weight_mag;
} AgentState;

int main() {
    printf("sizeof(Genome)      = %zu\n", sizeof(Genome));
    printf("sizeof(AgentState)  = %zu\n", sizeof(AgentState));
    printf("sizeof(TradeRecord) = %zu\n", sizeof(TradeRecord));
    printf("MAX_AGENTS × AgentState = %zu\n", MAX_AGENTS * sizeof(AgentState));
    printf("MAX_TRADE_HIST × TradeRecord = %zu\n", MAX_TRADE_HIST * sizeof(TradeRecord));
    printf("AgentState + Trade arrays = %zu\n",
           MAX_AGENTS * sizeof(AgentState) + MAX_TRADE_HIST * sizeof(TradeRecord));
    printf("sizeof(RoomState) estimated ≈ %zu\n",
           sizeof(AgentState)*MAX_AGENTS + sizeof(TradeRecord)*MAX_TRADE_HIST + 2000);
    return 0;
}
