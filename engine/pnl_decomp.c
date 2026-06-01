/**
 * pnl_decomp.c — T50: PnL Decomposition
 *
 * Decomposes agent PnL into direction, sizing, and timing components.
 *
 * Build: gcc -O3 pnl_decomp.c -o pnl_decomp -lm -I.
 * Usage: ./pnl_decomp [room_state.bin]
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

    int n_trading = 0;
    double total_direction_pnl = 0, total_sizing_pnl = 0;
    double total_timing_pnl = 0;
    double total_pnl = 0;

    /* WR tier tracking */
    int tiers[3] = {0};
    int tier_trades[3] = {0};
    int tier_wins[3] = {0};

    for (int i = 0; i < max_agents; i++) {
        AgentState *a = &state->agents[i];
        if (a->trades < 5) continue;
        n_trading++;

        /* Direction PnL: WR vs 50% */
        double wr = (double)a->wins / a->trades;
        double direction_pnl = (wr - 0.5) * a->trades * 0.02;
        total_direction_pnl += direction_pnl;

        /* Sizing PnL: position_size vs optimal Kelly (2*WR-1) */
        double kelly = fmax(0, 2 * wr - 1);
        double actual = a->genome.position_size;
        double sizing_pnl = (kelly - actual) * a->trades * 0.01;
        total_sizing_pnl += sizing_pnl;

        /* Timing PnL: conviction accuracy */
        double conv_ratio = a->conv_hi_total > 0
            ? (double)a->conv_hi_wins / a->conv_hi_total : wr;
        double timing_pnl = (conv_ratio - wr) * a->trades * 0.01;
        total_timing_pnl += timing_pnl;

        /* WR tier */
        int tier = wr < 0.45 ? 0 : (wr < 0.55 ? 1 : 2);
        tiers[tier]++;
        tier_trades[tier] += a->trades;
        tier_wins[tier] += a->wins;

        total_pnl += a->total_pnl;
    }

    printf("=== PnL Decomposition ===\n");
    printf("  Trading agents:  %d\n", n_trading);
    printf("  Total PnL:       $%.2f\n\n", total_pnl);

    printf("  %-20s %12s %12s\n", "Component", "Total", "Avg/Agent");
    printf("  %s\n", "────────────────────────────────────────────────");
    double pct_base = fabs(total_pnl) > 0.01 ? total_pnl : 1.0;

    printf("  %-20s %12.2f %12.2f (%5.1f%%)\n",
           "Direction", total_direction_pnl, total_direction_pnl / n_trading,
           total_direction_pnl / pct_base * 100);
    printf("  %-20s %12.2f %12.2f (%5.1f%%)\n",
           "Sizing", total_sizing_pnl, total_sizing_pnl / n_trading,
           total_sizing_pnl / pct_base * 100);
    printf("  %-20s %12.2f %12.2f (%5.1f%%)\n",
           "Timing/Conviction", total_timing_pnl, total_timing_pnl / n_trading,
           total_timing_pnl / pct_base * 100);

    double explained = total_direction_pnl + total_sizing_pnl + total_timing_pnl;
    printf("  %-20s %12s %12s\n", "───────────────", "────────────", "────────────");
    printf("  %-20s %12.2f %12.2f (%5.1f%%)\n",
           "Explained", explained, explained / n_trading,
           explained / pct_base * 100);
    printf("  %-20s %12.2f %12.2f (%5.1f%%)\n",
           "Unexplained", total_pnl - explained, (total_pnl - explained) / n_trading,
           (total_pnl - explained) / pct_base * 100);

    /* WR tier analysis */
    printf("\n  === Direction Accuracy by Tier ===\n");
    static const char *tn[] = {"Poor (<45%)", "Avg (45-55%)", "Good (>55%)"};
    printf("  %-15s %6s %8s %8s\n", "Tier", "Agents", "WR%%", "Trades");
    for (int t = 0; t < 3; t++) {
        double twr = tier_trades[t] > 0 ? (double)tier_wins[t] / tier_trades[t] * 100 : 0;
        printf("  %-15s %6d %7.2f%% %8d\n", tn[t], tiers[t], twr, tier_trades[t]);
    }

    /* Sizing analysis */
    printf("\n  === Sizing Analysis ===\n");
    double avg_size = 0, min_size = 1, max_size = 0;
    int sz_count = 0;
    for (int i = 0; i < max_agents; i++) {
        if (state->agents[i].trades < 5) continue;
        double s = state->agents[i].genome.position_size;
        avg_size += s;
        if (s < min_size) min_size = s;
        if (s > max_size) max_size = s;
        sz_count++;
    }
    if (sz_count > 0) {
        avg_size /= sz_count;
        printf("  Position size:    avg=%.4f  [%.4f, %.4f]\n", avg_size, min_size, max_size);
        printf("  Kelly-optimal:    (2*WR-1)  range [%.4f, %.4f]\n",
               fmax(0, 2*0.45-1), fmax(0, 2*0.55-1));
    }

    munmap(state, st.st_size);
    return 0;
}
