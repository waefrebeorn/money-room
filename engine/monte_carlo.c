/**
 * monte_carlo.c — T46: Monte Carlo Simulation
 *
 * Resamples trade outcomes from room_state.bin to compute
 * confidence intervals on WR, Sharpe, PnL, drawdown.
 *
 * Build: gcc -O3 -march=native monte_carlo.c -o monte_carlo -lm -I.
 * Usage: ./monte_carlo [room_state.bin] [simulations] [sample_pct]
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
#define CACHE_DIR   "/home/wubu2/.hermes/montecarlo_cache"
#define MAX_SIMS    100000

typedef struct { double wr, sharpe, pnl, maxdd; } TrialResult;

/* qsort comparators */
static int cmp_wr(const void *a, const void *b) {
    double va = ((TrialResult*)a)->wr, vb = ((TrialResult*)b)->wr;
    if (va < vb) return -1; if (va > vb) return 1; return 0;
}
static int cmp_sharpe(const void *a, const void *b) {
    double va = ((TrialResult*)a)->sharpe, vb = ((TrialResult*)b)->sharpe;
    if (va < vb) return -1; if (va > vb) return 1; return 0;
}
static int cmp_pnl(const void *a, const void *b) {
    double va = ((TrialResult*)a)->pnl, vb = ((TrialResult*)b)->pnl;
    if (va < vb) return -1; if (va > vb) return 1; return 0;
}
static int cmp_maxdd(const void *a, const void *b) {
    double va = ((TrialResult*)a)->maxdd, vb = ((TrialResult*)b)->maxdd;
    if (va < vb) return -1; if (va > vb) return 1; return 0;
}

int main(int argc, char **argv) {
    const char *path = STATE_PATH_DEFAULT;
    int n_sims = 10000;
    int sample_pct = 80;

    if (argc > 1 && argv[1][0] != '-') path = argv[1];
    if (argc > 2) n_sims = atoi(argv[2]);
    if (argc > 3) sample_pct = atoi(argv[3]);
    if (n_sims < 100) n_sims = 100;
    if (n_sims > MAX_SIMS) n_sims = MAX_SIMS;
    if (sample_pct < 10) sample_pct = 10;
    if (sample_pct > 100) sample_pct = 100;

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

    /* Count trading agents */
    int n_agents = 0;
    for (int i = 0; i < max_agents; i++)
        if (state->agents[i].trades > 0) n_agents++;

    if (n_agents < 5) {
        printf("Not enough trading agents: %d\n", n_agents);
        munmap(state, st.st_size); return 1;
    }

    int sample_size = n_agents * sample_pct / 100;
    if (sample_size < 3) sample_size = 3;
    if (sample_size > n_agents) sample_size = n_agents;

    /* Pre-compute agent metrics */
    double *agent_wr = malloc(n_agents * sizeof(double));
    double *agent_pnl = malloc(n_agents * sizeof(double));
    int *agent_trades = malloc(n_agents * sizeof(int));

    int idx = 0;
    for (int i = 0; i < max_agents && idx < n_agents; i++) {
        if (state->agents[i].trades > 0) {
            agent_wr[idx] = (double)state->agents[i].wins / state->agents[i].trades;
            agent_pnl[idx] = state->agents[i].total_pnl;
            agent_trades[idx] = state->agents[i].trades;
            idx++;
        }
    }

    /* Run Monte Carlo trials */
    TrialResult *trials = calloc(n_sims, sizeof(TrialResult));
    if (!trials) { free(agent_wr); free(agent_pnl); free(agent_trades);
                   munmap(state, st.st_size); return 1; }

    srand(time(NULL));
    for (int sim = 0; sim < n_sims; sim++) {
        double sum_wr = 0, sum_pnl = 0, sum_sharpe = 0;
        double peak = 0, trough = 0, running_pnl = 0;

        for (int s = 0; s < sample_size; s++) {
            int r = rand() % n_agents;
            sum_wr += agent_wr[r];
            sum_pnl += agent_pnl[r];
            double p = agent_wr[r];
            /* Approx Sharpe: Bernoulli(p) with even odds */
            double mean_ret = 2 * p - 1;
            double var_b = 4 * p * (1 - p);
            double sh = var_b > 0 ? (mean_ret / sqrt(var_b)) * sqrt(agent_trades[r]) : 0;
            sum_sharpe += sh;

            running_pnl += agent_pnl[r];
            if (running_pnl > peak) peak = running_pnl;
            double dd = (peak > 0) ? (peak - running_pnl) / peak : 0;
            if (dd > trough) trough = dd;
        }

        trials[sim].wr = sum_wr / sample_size * 100;
        trials[sim].sharpe = sum_sharpe / sample_size;
        trials[sim].pnl = sum_pnl;
        trials[sim].maxdd = trough * 100;
    }

    /* WR statistics */
    qsort(trials, n_sims, sizeof(TrialResult), cmp_wr);
    int p5_idx = n_sims * 5 / 100;
    int p95_idx = n_sims * 95 / 100;
    double wr_p5 = trials[p5_idx].wr, wr_p50 = trials[n_sims/2].wr, wr_p95 = trials[p95_idx].wr;
    double wr_mean = 0; for (int i = 0; i < n_sims; i++) wr_mean += trials[i].wr;
    wr_mean /= n_sims;
    double wr_var = 0; for (int i = 0; i < n_sims; i++) wr_var += pow(trials[i].wr - wr_mean, 2);
    wr_var /= n_sims;
    double wr_std = sqrt(wr_var);

    /* Sharpe statistics */
    qsort(trials, n_sims, sizeof(TrialResult), cmp_sharpe);
    double sh_p5 = trials[p5_idx].sharpe, sh_p50 = trials[n_sims/2].sharpe, sh_p95 = trials[p95_idx].sharpe;
    double sh_mean = 0; for (int i = 0; i < n_sims; i++) sh_mean += trials[i].sharpe;
    sh_mean /= n_sims;

    /* PnL statistics */
    qsort(trials, n_sims, sizeof(TrialResult), cmp_pnl);
    double pnl_p5 = trials[p5_idx].pnl, pnl_p50 = trials[n_sims/2].pnl, pnl_p95 = trials[p95_idx].pnl;
    double pnl_mean = 0; for (int i = 0; i < n_sims; i++) pnl_mean += trials[i].pnl;
    pnl_mean /= n_sims;

    /* MaxDD statistics */
    qsort(trials, n_sims, sizeof(TrialResult), cmp_maxdd);
    double dd_p5 = trials[p5_idx].maxdd, dd_p50 = trials[n_sims/2].maxdd, dd_p95 = trials[p95_idx].maxdd;

    /* Probability of profit */
    int prof_count = 0;
    for (int i = 0; i < n_sims; i++) if (trials[i].pnl > 0) prof_count++;

    /* ES 95% */
    double es_sum = 0;
    for (int i = 0; i < p5_idx; i++) es_sum += trials[i].pnl;

    /* VaR 99% */
    int p1_idx = n_sims * 1 / 100;

    /* Z-score vs 50% */
    double z = (wr_mean - 50.0) / (wr_std > 0.01 ? wr_std : 1.0);

    /* Output */
    printf("=== Monte Carlo Simulation ===\n");
    printf("  Agents (trading): %d\n", n_agents);
    printf("  Simulations:      %d\n", n_sims);
    printf("  Sample:           %d%% (%d agents/trial)\n\n", sample_pct, sample_size);
    printf("  %-10s %8s %8s %8s %8s %8s\n",
           "Metric", "P5", "P50", "P95", "Mean", "Std");
    printf("  %s\n", "────────────────────────────────────────────────────");
    printf("  %-10s %8.2f %8.2f %8.2f %8.2f %8.2f\n",
           "WR %%", wr_p5, wr_p50, wr_p95, wr_mean, wr_std);
    printf("  %-10s %8.4f %8.4f %8.4f %8.4f\n",
           "Sharpe", sh_p5, sh_p50, sh_p95, sh_mean);
    printf("  %-10s %8.2f %8.2f %8.2f %8.2f\n",
           "PnL $", pnl_p5, pnl_p50, pnl_p95, pnl_mean);
    printf("  %-10s %8.2f %8.2f %8.2f\n",
           "MaxDD %%", dd_p5, dd_p50, dd_p95);

    printf("\n  Risk Metrics\n");
    printf("  VaR 95%%:       $%.2f\n", pnl_p5);
    printf("  VaR 99%%:       $%.2f\n", trials[p1_idx].pnl);
    printf("  ES 95%%:        $%.2f\n", es_sum / (p5_idx > 0 ? p5_idx : 1));
    printf("  P(Profit):      %.1f%%\n", (double)prof_count / n_sims * 100);
    printf("  Z-score vs 50%%: %.2f %s\n", z,
           fabs(z) > 1.96 ? "(significant)" : "(not significant)");

    /* Save JSON */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);
    char json_path[512];
    snprintf(json_path, sizeof(json_path), "%s/montecarlo_results.json", CACHE_DIR);
    FILE *f = fopen(json_path, "w");
    if (f) {
        fprintf(f, "{\"simulations\":%d,\"agents\":%d,\"sample_pct\":%d,",
                n_sims, n_agents, sample_pct);
        fprintf(f, "\"wr\":[%.4f,%.4f,%.4f,%.4f,%.4f],", wr_p5, wr_p50, wr_p95, wr_mean, wr_std);
        fprintf(f, "\"sharpe\":[%.4f,%.4f,%.4f,%.4f],", sh_p5, sh_p50, sh_p95, sh_mean);
        fprintf(f, "\"pnl\":[%.4f,%.4f,%.4f,%.4f],", pnl_p5, pnl_p50, pnl_p95, pnl_mean);
        fprintf(f, "\"maxdd\":[%.4f,%.4f,%.4f],", dd_p5, dd_p50, dd_p95);
        fprintf(f, "\"var_95\":%.4f,\"prob_profit\":%.4f,\"z\":%.4f}",
                pnl_p5, (double)prof_count/n_sims, z);
        fclose(f);
    }

    free(trials); free(agent_wr); free(agent_pnl); free(agent_trades);
    munmap(state, st.st_size);
    return 0;
}
