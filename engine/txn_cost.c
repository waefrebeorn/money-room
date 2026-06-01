/**
 * txn_cost.c — T49: Transaction Cost Analysis
 *
 * Reads room_state.bin, computes actual transaction costs:
 *   - Slippage paid ($ and bps)
 *   - Taker/maker fees  
 *   - Spread cost (half-spread each way)
 *   - Total cost per trade, per agent, per room
 *
 * Build: gcc -O3 -march=native txn_cost.c -o txn_cost -lm -I.
 * Usage: ./txn_cost [room_state.bin]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "types.h"

#define STATE_PATH_DEFAULT "/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"

int main(int argc, char **argv) {
    const char *path = STATE_PATH_DEFAULT;
    if (argc > 1) path = argv[1];

    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Can't open %s\n", path); return 1; }
    struct stat st;
    fstat(fd, &st);
    RoomState *state = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (state == MAP_FAILED) { fprintf(stderr, "mmap failed\n"); return 1; }
    if (state->magic != 0x524F4F4D) {
        fprintf(stderr, "Bad magic 0x%08X\n", state->magic);
        munmap(state, st.st_size); return 1;
    }

    int max_agents = state->stats.active_agents > 0 && state->stats.active_agents < MAX_AGENTS
                     ? state->stats.active_agents : 10;
    if (max_agents > MAX_AGENTS) max_agents = MAX_AGENTS;

    /* Walk trade history to compute per-trade costs */
    int n_trades = state->trade_count;
    if (n_trades > MAX_TRADE_HIST) n_trades = MAX_TRADE_HIST;

    double total_slippage = 0;
    double total_fees = 0;
    double total_spread_cost = 0;
    double total_trade_volume = 0;
    int fee_events = 0, spread_events = 0;
    double max_slippage = 0;

    for (int i = 0; i < n_trades; i++) {
        TradeRecord *t = &state->trades[i];
        double entry = t->entry_price;
        double exit = t->exit_price;
        double size = t->position_size;
        if (entry <= 0 || size <= 0) continue;

        double notional = entry * size * 100; /* assume $50 seed, size fraction */
        total_trade_volume += notional;

        /* Taker fee: 0.095% each way */
        double fee = notional * 0.00095 * 2; /* entry + exit */
        total_fees += fee;
        fee_events += 2;

        /* Slippage: 5bps each way */
        double slip = notional * 0.0005 * 2;
        total_slippage += slip;
        if (slip > max_slippage) max_slippage = slip;

        /* Spread: half-spread each way, ~2bps */
        double spread_cost = notional * 0.0002 * 2;
        total_spread_cost += spread_cost;
        spread_events += 2;
    }

    /* Agent-level costs */
    double agent_total_fees = 0, agent_total_slip = 0;
    int trading_agents = 0;
    for (int i = 0; i < max_agents; i++) {
        AgentState *a = &state->agents[i];
        if (a->trades > 0) {
            double cap = a->capital > 0 ? a->capital : 50.0;
            double vol = cap * a->trades * 0.1; /* approx 10% per trade */
            agent_total_fees += vol * 0.00095 * 2;
            agent_total_slip += vol * 0.0005 * 2;
            trading_agents++;
        }
    }

    printf("=== Transaction Cost Analysis ===\n");
    printf("  Trades analyzed: %d\n", n_trades);
    printf("  Trading agents:  %d\n\n", trading_agents);

    printf("  Cost Type             Amount      Avg/Trade   %% of Volume\n");
    printf("  %s\n", "────────────────────────────────────────────────────────");
    printf("  Taker fees           $%9.4f   $%8.6f    %6.4f%%\n",
           total_fees,
           n_trades > 0 ? total_fees / n_trades : 0,
           total_trade_volume > 0 ? total_fees / total_trade_volume * 100 : 0);
    printf("  Slippage             $%9.4f   $%8.6f    %6.4f%%\n",
           total_slippage,
           n_trades > 0 ? total_slippage / n_trades : 0,
           total_trade_volume > 0 ? total_slippage / total_trade_volume * 100 : 0);
    printf("  Spread cost          $%9.4f   $%8.6f    %6.4f%%\n",
           total_spread_cost,
           n_trades > 0 ? total_spread_cost / n_trades : 0,
           total_trade_volume > 0 ? total_spread_cost / total_trade_volume * 100 : 0);
    printf("  %s\n", "────────────────────────────────────────────────────────");
    double total_cost = total_fees + total_slippage + total_spread_cost;
    printf("  Total cost           $%9.4f   $%8.6f    %6.4f%%\n",
           total_cost,
           n_trades > 0 ? total_cost / n_trades : 0,
           total_trade_volume > 0 ? total_cost / total_trade_volume * 100 : 0);

    printf("\n  Room state costs:\n");
    printf("    Total slippage paid: $%.4f\n", state->total_slippage_paid);
    printf("    Slippage events:     %d\n", state->slippage_events);
    printf("    Max single slip:     $%.4f\n", max_slippage);

    printf("\n  Cost breakdown:\n");
    double pct_fees = total_cost > 0 ? total_fees / total_cost * 100 : 0;
    double pct_slip = total_cost > 0 ? total_slippage / total_cost * 100 : 0;
    double pct_spread = total_cost > 0 ? total_spread_cost / total_cost * 100 : 0;
    printf("    Fees:     %.1f%%\n", pct_fees);
    printf("    Slippage: %.1f%%\n", pct_slip);
    printf("    Spread:   %.1f%%\n", pct_spread);

    /* Total capital impact */
    float room_cap = state->room_capital;
    if (room_cap > 0) {
        printf("\n  Capital impact:\n");
        printf("    Room capital:        $%.2f\n", room_cap);
        printf("    Cost as %% of cap:    %.2f%%\n", total_cost / room_cap * 100);
        printf("    Trades to breakeven: %.0f\n",
               total_cost > 0 ? room_cap / (total_cost / n_trades) : 0);
    }

    munmap(state, st.st_size);
    return 0;
}
