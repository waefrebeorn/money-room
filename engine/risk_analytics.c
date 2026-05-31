/**
 * risk_analytics.c — C20: Profit Factor, C21: Monte Carlo VaR, C22: Expected Shortfall
 * Reads room_state.bin and trade history to compute risk metrics.
 *
 * Compile: gcc -O2 -o risk_analytics risk_analytics.c -lm -I.
 * Run: ./risk_analytics path/to/room_state.bin
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "types.h"

static RoomState *map_state(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return NULL; }
    size_t sz = sizeof(RoomState);
    RoomState *s = (RoomState*)mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (s == MAP_FAILED) { perror("mmap"); return NULL; }
    return s;
}

static int cmp_float_desc(const void *a, const void *b) {
    float fa = *(const float*)a, fb = *(const float*)b;
    if (fa < fb) return 1;
    if (fa > fb) return -1;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "room_state.bin";
    RoomState *state = map_state(path);
    if (!state) return 1;

    printf("═══ RISK ANALYTICS ═══\n");
    printf("Cycle: %d  Trades: %d  Agents: %d/%d\n\n",
           state->cycle, state->trade_count,
           state->stats.active_agents, MAX_AGENTS);

    // ── C20: Profit Factor per room (gross win / gross loss) ──
    printf("═══ C20: PROFIT FACTOR ═══\n");
    float gross_win = 0, gross_loss = 0;
    int resolved_trades = 0;
    int n_trades = state->trade_count < MAX_TRADE_HIST ? state->trade_count : MAX_TRADE_HIST;
    for (int i = 0; i < n_trades; i++) {
        if (state->trades[i].resolved_at == 0) continue;
        resolved_trades++;
        if (state->trades[i].won) {
            gross_win += state->trades[i].position_size * (1.0f + state->trades[i].pnl_pct);
        } else {
            gross_loss += state->trades[i].position_size;
        }
    }

    // Per-agent profit factor
    float ag_gross_win = 0, ag_gross_loss = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!state->agents[i].alive) continue;
        if (state->agents[i].total_pnl > 0) ag_gross_win += state->agents[i].total_pnl;
        else ag_gross_loss += fabsf(state->agents[i].total_pnl);
    }

    printf("  Gross win (trades):     $%.2f\n", gross_win);
    printf("  Gross loss (trades):    $%.2f\n", gross_loss);
    printf("  Profit factor (trades): %.4f  %s\n",
           gross_loss > 0 ? gross_win / gross_loss : gross_win > 0 ? INFINITY : 0,
           gross_loss > 0 && gross_win / gross_loss > 1.5 ? "✅ Above 1.5 target" :
           gross_loss > 0 && gross_win / gross_loss > 1.0 ? "✅ Profitable" : "⚠️ Below breakeven");
    printf("  Resolved trades:        %d\n", resolved_trades);
    printf("  Profit factor (agents): %.4f\n",
           ag_gross_loss > 0 ? ag_gross_win / ag_gross_loss : 0);

    // ── C21: Monte Carlo VaR (simulate random trade sequences) ──
    printf("\n═══ C21: MONTE CARLO VaR ═══\n");
    // Collect trade outcomes (PnL%)
    int sample_count = 0;
    float *trade_samples = malloc(n_trades * sizeof(float));
    if (trade_samples) {
        for (int i = 0; i < n_trades; i++) {
            if (state->trades[i].resolved_at > 0) {
                trade_samples[sample_count++] = state->trades[i].pnl_pct;
            }
        }
        if (sample_count > 100) {
            int N_SIMS = 10000;
            int portfolio_size = 100; // Simulate trading 100 agents
            float *sim_returns = malloc(N_SIMS * sizeof(float));

            srand(42);
            for (int sim = 0; sim < N_SIMS; sim++) {
                float total_ret = 0;
                for (int t = 0; t < portfolio_size; t++) {
                    int idx = rand() % sample_count;
                    total_ret += trade_samples[idx];
                }
                sim_returns[sim] = total_ret / portfolio_size; // Average return per trade
            }

            qsort(sim_returns, N_SIMS, sizeof(float), cmp_float_desc);

            // VaR at 95%, 99% confidence
            int var95_idx = (int)(N_SIMS * 0.05);
            int var99_idx = (int)(N_SIMS * 0.01);
            if (var95_idx >= N_SIMS) var95_idx = N_SIMS - 1;
            if (var99_idx >= N_SIMS) var99_idx = N_SIMS - 1;

            // Expected shortfall (C22): average of worst 5%/1% outcomes
            float es95_sum = 0, es99_sum = 0;
            for (int i = 0; i < var95_idx; i++) es95_sum += sim_returns[i];
            for (int i = 0; i < var99_idx; i++) es99_sum += sim_returns[i];

            printf("  Samples:        %d trade outcomes\n", sample_count);
            printf("  Simulations:    %d (portfolio of %d trades each)\n", N_SIMS, portfolio_size);
            printf("\n  VaR 95%%:        %.4f (%.2f%% loss per trade worst 5%%)\n",
                   sim_returns[var95_idx], sim_returns[var95_idx] * 100);
            printf("  VaR 99%%:        %.4f (%.2f%% loss per trade worst 1%%)\n",
                   sim_returns[var99_idx], sim_returns[var99_idx] * 100);

            printf("\n═══ C22: EXPECTED SHORTFALL ═══\n");
            printf("  ES 95%%:         %.4f (avg loss in worst 5%%)\n", es95_sum / var95_idx);
            printf("  ES 99%%:         %.4f (avg loss in worst 1%%)\n", es99_sum / var99_idx);
            printf("\n  Profit factor:  %.4f (C20)\n",
                   gross_loss > 0 ? gross_win / gross_loss : 0);
            printf("  VaR 95%%:        %.4f (C21)\n", sim_returns[var95_idx]);
            printf("  ES 95%%:         %.4f (C22)\n", es95_sum / var95_idx);

            // Risk-adjusted verdict
            float pf = gross_loss > 0 ? gross_win / gross_loss : 0;
            printf("\n═══ VERDICT ═══\n");
            printf("  C20 Profit Factor: %.2f %s\n", pf, pf > 1.5 ? "✅ >1.5 target" : pf > 1.0 ? "✅ Profitable" : "⚠️ Below 1.0");
            printf("  C21 VaR 95%%:       %.2f%% %s\n", sim_returns[var95_idx] * 100,
                   sim_returns[var95_idx] > -0.05f ? "✅ Acceptable" : "⚠️ High downside");
            printf("  C22 ES 95%%:        %.2f%%\n", (es95_sum / var95_idx) * 100);
            printf("  Interpretation: The worst 5%% of trading days lose %.2f%% on average\n",
                   (es95_sum / var95_idx) * 100);

            free(sim_returns);
        } else {
            printf("  Not enough resolved trades (%d < 100)\n", sample_count);
        }
        free(trade_samples);
    }

    // ── C25-C34: Already covered by T17-T20 and P23 ──
    printf("\n═══ C25-C34: RISK LIMITS (ALREADY COVERED) ═══\n");
    printf("  C25 position limit per agent  → T18 (2%% of room capital)\n");
    printf("  C26 concentration limit       → T18 (25%% total exposure)\n");
    printf("  C28 leverage limit            → T18 (position caps)\n");
    printf("  C29 circuit breaker           → T17 (20%% drawdown, 10 consec losses)\n");
    printf("  C30 cool-down                 → T17 (100/50 cycle cooldowns)\n");
    printf("  C31 trailing stop             → Handled by take_profit_pct genome param\n");
    printf("  C33 volatility adaptation     → P23 (volatility scaling)\n");
    printf("  C32 time stop                 → time_horizon genome param\n");
    printf("  C27 correlation limit         → diversity metric (C10)\n");
    printf("  C34 gap risk protection       → price_delta_pct feature\n");

    munmap(state, sizeof(RoomState));
    return 0;
}
