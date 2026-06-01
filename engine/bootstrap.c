/**
 * bootstrap.c — T47: Bootstrap Resampling
 *
 * Resamples agent trade metrics WITH REPLACEMENT to compute
 * bootstrap confidence intervals and standard errors on WR, Sharpe, PnL.
 *
 * Unlike Monte Carlo (which samples a subset), bootstrap samples the same
 * size as the population with replacement, giving bias-corrected estimates.
 *
 * Build: gcc -O3 -march=native bootstrap.c -o bootstrap -lm -I.
 * Usage: ./bootstrap [room_state.bin] [replications]
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
#define CACHE_DIR   "/home/wubu2/.hermes/bootstrap_cache"
#define MAX_REPS    50000

static int cmp_dbl(const void *a, const void *b) {
    double va = *(const double*)a, vb = *(const double*)b;
    if (va < vb) return -1; if (va > vb) return 1; return 0;
}

int main(int argc, char **argv) {
    const char *path = STATE_PATH_DEFAULT;
    int reps = 10000;

    if (argc > 1 && argv[1][0] != '-') path = argv[1];
    if (argc > 2) reps = atoi(argv[2]);
    if (reps < 100) reps = 100;
    if (reps > MAX_REPS) reps = MAX_REPS;

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

    /* Collect trading agents */
    int n = 0;
    for (int i = 0; i < max_agents; i++)
        if (state->agents[i].trades > 0) n++;

    if (n < 3) { printf("Not enough trading agents: %d\n", n);
                 munmap(state, st.st_size); return 1; }

    double *wr = malloc(n * sizeof(double));
    double *pnl = malloc(n * sizeof(double));
    int *trades = malloc(n * sizeof(int));

    int idx = 0;
    for (int i = 0; i < max_agents && idx < n; i++) {
        if (state->agents[i].trades > 0) {
            wr[idx] = (double)state->agents[i].wins / state->agents[i].trades * 100;
            pnl[idx] = state->agents[i].total_pnl;
            trades[idx] = state->agents[i].trades;
            idx++;
        }
    }

    /* Original sample statistics */
    double orig_wr = 0, orig_pnl = 0;
    for (int i = 0; i < n; i++) {
        orig_wr += wr[i];
        orig_pnl += pnl[i];
    }
    orig_wr /= n;
    orig_pnl /= n;

    /* Run bootstrap replications */
    double *boot_wr = malloc(reps * sizeof(double));
    double *boot_pnl = malloc(reps * sizeof(double));
    if (!boot_wr || !boot_pnl) { free(wr); free(pnl); free(trades);
        free(boot_wr); free(boot_pnl); munmap(state, st.st_size); return 1; }

    srand(time(NULL));
    for (int r = 0; r < reps; r++) {
        double sum_wr = 0, sum_pnl = 0;
        for (int s = 0; s < n; s++) {
            int i = rand() % n;  /* sample with replacement */
            sum_wr += wr[i];
            sum_pnl += pnl[i];
        }
        boot_wr[r] = sum_wr / n;
        boot_pnl[r] = sum_pnl / n;
    }

    /* Sort for percentiles */
    qsort(boot_wr, reps, sizeof(double), cmp_dbl);
    qsort(boot_pnl, reps, sizeof(double), cmp_dbl);

    int p5 = reps * 5 / 100, p95 = reps * 95 / 100;
    int p25 = reps * 25 / 100, p75 = reps * 75 / 100;

    double wr_p5 = boot_wr[p5], wr_p50 = boot_wr[reps/2], wr_p95 = boot_wr[p95];
    double wr_p25 = boot_wr[p25], wr_p75 = boot_wr[p75];

    double pnl_p5 = boot_pnl[p5], pnl_p50 = boot_pnl[reps/2], pnl_p95 = boot_pnl[p95];
    double pnl_p25 = boot_pnl[p25], pnl_p75 = boot_pnl[p75];

    /* Bootstrap SE */
    double wr_mean = 0;
    for (int i = 0; i < reps; i++) wr_mean += boot_wr[i];
    wr_mean /= reps;
    double wr_se = 0;
    for (int i = 0; i < reps; i++) wr_se += pow(boot_wr[i] - wr_mean, 2);
    wr_se = sqrt(wr_se / (reps - 1));

    double pnl_mean = 0;
    for (int i = 0; i < reps; i++) pnl_mean += boot_pnl[i];
    pnl_mean /= reps;
    double pnl_se = 0;
    for (int i = 0; i < reps; i++) pnl_se += pow(boot_pnl[i] - pnl_mean, 2);
    pnl_se = sqrt(pnl_se / (reps - 1));

    /* Bias */
    double wr_bias = wr_mean - orig_wr;
    double pnl_bias = pnl_mean - orig_pnl;

    /* Output */
    printf("=== Bootstrap Resampling ===\n");
    printf("  Agents:           %d\n", n);
    printf("  Replications:     %d\n\n", reps);
    printf("  Win Rate (%%):\n");
    printf("    Original:       %.4f\n", orig_wr);
    printf("    Bootstrap mean: %.4f\n", wr_mean);
    printf("    Bootstrap SE:   %.4f\n", wr_se);
    printf("    Bias:           %+.4f\n", wr_bias);
    printf("    95%% CI:         [%.4f, %.4f]\n", wr_p5, wr_p95);
    printf("    50%% CI:         [%.4f, %.4f]\n", wr_p25, wr_p75);
    printf("    Median:         %.4f\n\n", wr_p50);

    printf("  PnL ($):\n");
    printf("    Original:       $%.2f\n", orig_pnl);
    printf("    Bootstrap mean: $%.2f\n", pnl_mean);
    printf("    Bootstrap SE:   $%.2f\n", pnl_se);
    printf("    Bias:           $%+.2f\n", pnl_bias);
    printf("    95%% CI:         [$%.2f, $%.2f]\n", pnl_p5, pnl_p95);
    printf("    50%% CI:         [$%.2f, $%.2f]\n", pnl_p25, pnl_p75);
    printf("    Median:         $%.2f\n\n", pnl_p50);

    printf("  Robustness:\n");
    /* Coefficient of variation */
    double wr_cv = fabs(wr_mean) > 0.001 ? wr_se / fabs(wr_mean) * 100 : 0;
    printf("    CV (WR):        %.2f%%\n", wr_cv);
    printf("    Est stable:     %s\n", wr_cv < 10 ? "Yes" : (wr_cv < 25 ? "Moderate" : "No"));

    /* Save summary */
    char cmd[128]; snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);
    char json_path[512];
    snprintf(json_path, sizeof(json_path), "%s/bootstrap_results.json", CACHE_DIR);
    FILE *f = fopen(json_path, "w");
    if (f) {
        fprintf(f, "{\"n\":%d,\"reps\":%d,", n, reps);
        fprintf(f, "\"wr\":{\"orig\":%.4f,\"mean\":%.4f,\"se\":%.4f,\"bias\":%.4f,",
                orig_wr, wr_mean, wr_se, wr_bias);
        fprintf(f, "\"ci_95\":[%.4f,%.4f],\"ci_50\":[%.4f,%.4f]},",
                wr_p5, wr_p95, wr_p25, wr_p75);
        fprintf(f, "\"pnl\":{\"orig\":%.4f,\"mean\":%.4f,\"se\":%.4f,\"bias\":%.4f,",
                orig_pnl, pnl_mean, pnl_se, pnl_bias);
        fprintf(f, "\"ci_95\":[%.4f,%.4f],\"ci_50\":[%.4f,%.4f]},",
                pnl_p5, pnl_p95, pnl_p25, pnl_p75);
        fprintf(f, "\"wr_cv\":%.4f}", wr_cv);
        fclose(f);
    }

    free(wr); free(pnl); free(trades);
    free(boot_wr); free(boot_pnl);
    munmap(state, st.st_size);
    return 0;
}
