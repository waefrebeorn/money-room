/**
 * permutation_test.c — T48: Permutation Test
 *
 * Tests if observed win rate is significantly above/below 50%
 * by shuffling trade outcomes and building a null distribution.
 *
 * Build: gcc -O3 -march=native permutation_test.c -o permutation_test -lm -I.
 * Usage: ./permutation_test [room_state.bin] [permutations]
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
#define CACHE_DIR   "/home/wubu2/.hermes/permutation_cache"
#define MAX_PERMS   100000

static int cmp_dbl(const void *a, const void *b) {
    double va = *(const double*)a, vb = *(const double*)b;
    if (va < vb) return -1; if (va > vb) return 1; return 0;
}

int main(int argc, char **argv) {
    const char *path = STATE_PATH_DEFAULT;
    int perms = 10000;

    if (argc > 1 && argv[1][0] != '-') path = argv[1];
    if (argc > 2) perms = atoi(argv[2]);
    if (perms < 100) perms = 100;
    if (perms > MAX_PERMS) perms = MAX_PERMS;

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

    /* Collect trade outcomes: for each agent, count wins/losses */
    int total_wins = 0, total_trades = 0;
    int n_agents = 0;
    for (int i = 0; i < max_agents; i++) {
        int t = state->agents[i].trades;
        if (t > 0) {
            total_wins += state->agents[i].wins;
            total_trades += t;
            n_agents++;
        }
    }

    if (total_trades < 10) {
        printf("Not enough trades: %d across %d agents\n", total_trades, n_agents);
        munmap(state, st.st_size); return 1;
    }

    double observed_wr = (double)total_wins / total_trades * 100;

    /* Build a binary outcome array (1=win, 0=loss) for shuffling */
    int *outcomes = malloc(total_trades * sizeof(int));
    if (!outcomes) { munmap(state, st.st_size); return 1; }
    int idx = 0;
    for (int i = 0; i < max_agents; i++) {
        int t = state->agents[i].trades;
        if (t > 0) {
            int w = state->agents[i].wins;
            for (int j = 0; j < t; j++, idx++)
                outcomes[idx] = (j < w) ? 1 : 0;
        }
    }

    /* Permutation test: shuffle outcomes and recompute WR */
    srand(time(NULL));
    int extreme = 0; /* count of permuted WRs >= observed */
    double *perm_wrs = malloc(perms * sizeof(double));
    if (!perm_wrs) { free(outcomes); munmap(state, st.st_size); return 1; }

    for (int p = 0; p < perms; p++) {
        /* Fisher-Yates shuffle */
        for (int i = total_trades - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = outcomes[i];
            outcomes[i] = outcomes[j];
            outcomes[j] = tmp;
        }
        /* Count wins in first N outcomes */
        int wins = 0;
        int n_test = total_trades > 1000 ? 1000 : total_trades;
        for (int i = 0; i < n_test; i++) if (outcomes[i]) wins++;
        perm_wrs[p] = (double)wins / n_test * 100;
        if (perm_wrs[p] >= observed_wr) extreme++;
    }

    qsort(perm_wrs, perms, sizeof(double), cmp_dbl);
    double p_value = (double)(extreme + 1) / (perms + 1);

    /* Null distribution stats */
    double null_mean = 0;
    for (int i = 0; i < perms; i++) null_mean += perm_wrs[i];
    null_mean /= perms;
    double null_sd = 0;
    for (int i = 0; i < perms; i++) null_sd += pow(perm_wrs[i] - null_mean, 2);
    null_sd = sqrt(null_sd / (perms - 1));

    int p5 = perms * 5 / 100, p95 = perms * 95 / 100;

    printf("=== Permutation Test ===\n");
    printf("  Trades:           %d\n", total_trades);
    printf("  Agents:           %d\n", n_agents);
    printf("  Observed WR:      %.2f%%\n", observed_wr);
    printf("  Permutations:     %d\n\n", perms);
    printf("  Null distribution:\n");
    printf("    Mean:           %.2f%%\n", null_mean);
    printf("    SD:             %.4f\n", null_sd);
    printf("    95%% CI:         [%.2f%%, %.2f%%]\n", perm_wrs[p5], perm_wrs[p95]);
    printf("    Z-score:        %.4f\n", (observed_wr - null_mean) / (null_sd > 0.001 ? null_sd : 1));
    printf("\n  P-value:          %.6f %s\n", p_value,
           p_value < 0.05 ? "(SIGNIFICANT)" :
           p_value < 0.10 ? "(MARGINAL)" : "(not significant)");
    printf("  Conclusion:       WR is %s 50%%\n",
           observed_wr > 50 ? (p_value < 0.05 ? "significantly ABOVE" : "NOT significantly above")
                            : (p_value < 0.05 ? "significantly BELOW" : "NOT significantly below"));

    /* One-tailed and two-tailed */
    printf("\n  One-tailed p:     %.6f\n", p_value);
    printf("  Two-tailed p:     %.6f\n", fmin(p_value * 2, 1.0));

    /* Save JSON */
    char cmd[128]; snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);
    char json_path[512];
    snprintf(json_path, sizeof(json_path), "%s/permutation_results.json", CACHE_DIR);
    FILE *f = fopen(json_path, "w");
    if (f) {
        fprintf(f, "{\"trades\":%d,\"agents\":%d,\"obs_wr\":%.4f,\"perms\":%d,",
                total_trades, n_agents, observed_wr, perms);
        fprintf(f, "\"null_mean\":%.4f,\"null_sd\":%.4f,",
                null_mean, null_sd);
        fprintf(f, "\"p_value\":%.6f,\"p_twotail\":%.6f,\"z\":%.4f,",
                p_value, fmin(p_value*2, 1.0), (observed_wr - null_mean)/(null_sd > 0.001 ? null_sd : 1));
        fprintf(f, "\"significant\":%s}", p_value < 0.05 ? "true" : "false");
        fclose(f);
    }

    free(outcomes); free(perm_wrs);
    munmap(state, st.st_size);
    return 0;
}
