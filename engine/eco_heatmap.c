/**
 * eco_heatmap.c — E41: Ecosystem Heatmap
 *
 * Reads room_state.bin, buckets agents by genome parameter decile,
 * reports WR/PnL/count per bucket for all 10 genome params.
 *
 * Build: gcc -O3 -march=native eco_heatmap.c -o eco_heatmap -lm
 * Usage: ./eco_heatmap [room_state.bin]
 *        ./eco_heatmap json [room_state.bin]   — JSON output for web frontend
 *        ./eco_heatmap stats [room_state.bin]   — aggregate ecosystem stats
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stddef.h>
#include "types.h"

#define STATE_PATH_DEFAULT "/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"
#define N_BINS 10
#define N_PARAMS 10

static const char *param_names[N_PARAMS] = {
    "position_size", "conviction_threshold", "risk_tolerance",
    "lie_sensitivity", "herd_antipathy", "stop_loss_pct",
    "take_profit_pct", "min_edge_pct", "time_horizon",
    "mean_reversion_bias"
};

typedef struct {
    float param_val;
    float win_rate;
    float pnl_pct;
    float capital;
    int   trades;
    int   wins;
} AgentHeat;

typedef struct {
    float min_v, max_v;
    int   count;
    double wr_sum;
    double pnl_sum;
    double cap_sum;
    int   trade_sum;
    int   win_sum;
} BinStats;

static float get_param(const Genome *g, int idx) {
    switch (idx) {
        case 0:  return g->position_size;
        case 1:  return g->conviction_threshold;
        case 2:  return g->risk_tolerance;
        case 3:  return g->lie_sensitivity;
        case 4:  return g->herd_antipathy;
        case 5:  return g->stop_loss_pct;
        case 6:  return g->take_profit_pct;
        case 7:  return g->min_edge_pct;
        case 8:  return g->time_horizon;
        case 9:  return g->mean_reversion_bias;
        default: return 0;
    }
}

/* Find param range across active agents */
static void get_range(const AgentState *agents, int n, int param_idx,
                       float *min_v, float *max_v) {
    *min_v = 1e10f;
    *max_v = -1e10f;
    for (int i = 0; i < n; i++) {
        if (!agents[i].alive) continue;
        float v = get_param(&agents[i].genome, param_idx);
        if (v < *min_v) *min_v = v;
        if (v > *max_v) *max_v = v;
    }
    /* Avoid zero range */
    if (*max_v - *min_v < 1e-6f) {
        *max_v = *min_v + 1.0f;
    }
}

static int bin_index(float val, float min_v, float max_v) {
    if (val >= max_v) return N_BINS - 1;
    if (val <= min_v) return 0;
    float norm = (val - min_v) / (max_v - min_v);
    int idx = (int)(norm * N_BINS);
    if (idx >= N_BINS) idx = N_BINS - 1;
    if (idx < 0) idx = 0;
    return idx;
}

static double calc_wr(int wins, int trades) {
    return trades > 0 ? (double)wins / trades * 100.0 : 0.0;
}

static double calc_return(float capital, float starting) {
    return starting > 0 ? (double)(capital - starting) / starting * 100.0 : 0.0;
}

static void build_heatmap(const AgentState *agents, int n, int param_idx,
                          float min_v, float max_v, BinStats *bins) {
    memset(bins, 0, sizeof(BinStats) * N_BINS);

    /* Compute bin widths */
    float bin_width = (max_v - min_v) / N_BINS;
    for (int i = 0; i < N_BINS; i++) {
        bins[i].min_v = min_v + i * bin_width;
        bins[i].max_v = min_v + (i + 1) * bin_width;
    }
    bins[N_BINS-1].max_v = max_v;

    /* Fill bins */
    for (int i = 0; i < n; i++) {
        if (!agents[i].alive) continue;
        float v = get_param(&agents[i].genome, param_idx);
        int bi = bin_index(v, min_v, max_v);
        bins[bi].count++;
        bins[bi].wr_sum += calc_wr(agents[i].wins, agents[i].trades);
        bins[bi].pnl_sum += calc_return(agents[i].capital, agents[i].starting_capital);
        bins[bi].cap_sum += agents[i].capital;
        bins[bi].trade_sum += agents[i].trades;
        bins[bi].win_sum += agents[i].wins;
    }
}

static void print_heatmap(const BinStats *bins, const char *pname,
                          float min_v, float max_v) {
    printf("\n=== %s [%.4f, %.4f] ===\n", pname, min_v, max_v);
    printf("BIN  RANGE         COUNT  AVG_WR%%  AVG_RET%%  AVG_CAP  TOT_TRADES  WINS\n");
    printf("---- ------------- -----  --------  --------  -------  ----------  ----\n");
    for (int i = 0; i < N_BINS; i++) {
        double avg_wr = bins[i].count > 0 ? bins[i].wr_sum / bins[i].count : 0;
        double avg_ret = bins[i].count > 0 ? bins[i].pnl_sum / bins[i].count : 0;
        double avg_cap = bins[i].count > 0 ? bins[i].cap_sum / bins[i].count : 0;
        printf("  %d  [%.4f,%.4f]  %5d  %8.2f  %9.2f  %7.0f  %10d  %4d\n",
               i, bins[i].min_v, bins[i].max_v, bins[i].count,
               avg_wr, avg_ret, avg_cap, bins[i].trade_sum, bins[i].win_sum);
    }

    /* Find trending: high vs low bin difference */
    int mid = N_BINS / 2;
    double high_wr = 0, low_wr = 0;
    int high_n = 0, low_n = 0;
    for (int i = mid; i < N_BINS; i++) {
        high_wr += bins[i].wr_sum;
        high_n += bins[i].count;
    }
    for (int i = 0; i < mid; i++) {
        low_wr += bins[i].wr_sum;
        low_n += bins[i].count;
    }
    double high_avg = high_n > 0 ? high_wr / high_n : 0;
    double low_avg = low_n > 0 ? low_wr / low_n : 0;
    double delta = high_avg - low_avg;
    char direction = delta > 1.0 ? '^' : (delta < -1.0 ? 'v' : '~');
    printf("  TREND: high_dec=%.1f%%  low_dec=%.1f%%  delta=%.1f%%  %c\n",
           high_avg, low_avg, delta, direction);

    /* Best bin */
    int best_bin = 0;
    double best_val = -1e9;
    for (int i = 0; i < N_BINS; i++) {
        if (bins[i].count > 0 && bins[i].wr_sum / bins[i].count > best_val) {
            best_val = bins[i].wr_sum / bins[i].count;
            best_bin = i;
        }
    }
    printf("  BEST BIN=%d [%.4f-%.4f] avg_wr=%.1f%% avg_ret=%.1f%% agents=%d\n",
           best_bin, bins[best_bin].min_v, bins[best_bin].max_v,
           bins[best_bin].count > 0 ? bins[best_bin].wr_sum / bins[best_bin].count : 0,
           bins[best_bin].count > 0 ? bins[best_bin].pnl_sum / bins[best_bin].count : 0,
           bins[best_bin].count);
}

static void print_json(const BinStats bins[N_PARAMS][N_BINS],
                       const AgentState *agents, int n) {
    printf("{\n");
    printf("  \"agent_count\": %d,\n", n);
    printf("  \"params\": [\n");
    for (int p = 0; p < N_PARAMS; p++) {
        printf("    {\n");
        printf("      \"name\": \"%s\",\n", param_names[p]);
        printf("      \"bins\": [\n");
        for (int b = 0; b < N_BINS; b++) {
            double avg_wr = bins[p][b].count > 0 ? bins[p][b].wr_sum / bins[p][b].count : 0;
            double avg_ret = bins[p][b].count > 0 ? bins[p][b].pnl_sum / bins[p][b].count : 0;
            double avg_cap = bins[p][b].count > 0 ? bins[p][b].cap_sum / bins[p][b].count : 0;
            printf("        {\"bin\":%d,\"min\":%.4f,\"max\":%.4f,\"count\":%d,"
                   "\"avg_wr\":%.2f,\"avg_ret\":%.2f,\"avg_cap\":%.2f}%s\n",
                   b, bins[p][b].min_v, bins[p][b].max_v, bins[p][b].count,
                   avg_wr, avg_ret, avg_cap,
                   b < N_BINS - 1 ? "," : "");
        }
        printf("      ]\n");
        printf("    }%s\n", p < N_PARAMS - 1 ? "," : "");
    }
    printf("  ]\n");
    printf("}\n");
}

static void print_stats(const AgentState *agents, int n) {
    int alive = 0;
    double wr_sum = 0, pnl_sum = 0, cap_sum = 0;
    int trade_total = 0, win_total = 0;
    float min_cap = 1e10f, max_cap = -1e10f;

    for (int i = 0; i < n; i++) {
        if (!agents[i].alive) continue;
        alive++;
        wr_sum += calc_wr(agents[i].wins, agents[i].trades);
        pnl_sum += calc_return(agents[i].capital, agents[i].starting_capital);
        cap_sum += agents[i].capital;
        trade_total += agents[i].trades;
        win_total += agents[i].wins;
        if (agents[i].capital < min_cap) min_cap = agents[i].capital;
        if (agents[i].capital > max_cap) max_cap = agents[i].capital;
    }

    printf("=== ECOSYSTEM AGGREGATE ===\n");
    printf("Agents:     %d alive / %d total\n", alive, n);
    printf("Avg WR:     %.2f%%\n", alive > 0 ? wr_sum / alive : 0);
    printf("Avg Ret:    %.2f%%\n", alive > 0 ? pnl_sum / alive : 0);
    printf("Total Cap:  $%.2f\n", cap_sum);
    printf("Avg Cap:    $%.2f\n", alive > 0 ? cap_sum / alive : 0);
    printf("Cap Range:  $%.2f - $%.2f\n", min_cap, max_cap);
    printf("Trades:     %d total, %d wins (%.2f%%)\n",
           trade_total, win_total,
           trade_total > 0 ? (double)win_total / trade_total * 100.0 : 0);
    printf("Zero/neg capital agents: ");
    int zero_cnt = 0;
    for (int i = 0; i < n; i++)
        if (agents[i].alive && agents[i].capital <= 0) zero_cnt++;
    printf("%d\n", zero_cnt);
}

int main(int argc, char **argv) {
    const char *path = STATE_PATH_DEFAULT;
    int mode = 0; /* 0=table, 1=json, 2=stats */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "json") == 0) mode = 1;
        else if (strcmp(argv[i], "stats") == 0) mode = 2;
        else path = argv[i];
    }

    /* Open and mmap room_state.bin */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s\n", path);
        return 1;
    }

    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }

    /* RoomState struct layout varies by compile flags.
     * We read the header portion (up to agents array), then compute
     * how many agents fit in the remaining file space. */
    size_t hdr_size = offsetof(RoomState, agents);
    if ((size_t)sb.st_size <= hdr_size) {
        fprintf(stderr, "Error: file too small (%ld bytes, header alone needs %zu)\n",
                (long)sb.st_size, hdr_size);
        close(fd);
        return 1;
    }

    RoomState *rs = (RoomState *)mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (rs == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    if (rs->magic != STATE_MAGIC) {
        fprintf(stderr, "Error: bad magic 0x%08X (expected 0x%08X)\n",
                rs->magic, STATE_MAGIC);
        munmap(rs, sb.st_size);
        return 1;
    }

    /* Derive agent count from file size (handle struct layout variations) */
    size_t agents_offset = offsetof(RoomState, agents);
    size_t remaining = (size_t)sb.st_size - agents_offset;
    size_t agent_size = sizeof(AgentState);
    int n = (int)(remaining / agent_size);
    if (n < 1) {
        fprintf(stderr, "Error: no agents fit in file (%zu bytes remaining, agent=%zu)\n",
                remaining, agent_size);
        munmap(rs, sb.st_size);
        return 1;
    }
    if (n > MAX_AGENTS) n = MAX_AGENTS;

    if (mode == 2) {
        print_stats(rs->agents, n);
        munmap(rs, sb.st_size);
        return 0;
    }

    /* Build heatmap for each param */
    BinStats all_bins[N_PARAMS][N_BINS];

    for (int p = 0; p < N_PARAMS; p++) {
        float min_v, max_v;
        get_range(rs->agents, n, p, &min_v, &max_v);
        build_heatmap(rs->agents, n, p, min_v, max_v, all_bins[p]);
    }

    if (mode == 1) {
        print_json(all_bins, rs->agents, n);
    } else {
        printf("ECOSYSTEM HEATMAP — RoomState at cycle %d, %d agents\n",
               rs->cycle, n);
        printf("Each param is split into %d equal-width bins.\n", N_BINS);
        printf("Bins show: agent count | avg win rate | avg return %% | avg capital\n");
        for (int p = 0; p < N_PARAMS; p++) {
            float min_v, max_v;
            get_range(rs->agents, n, p, &min_v, &max_v);
            print_heatmap(all_bins[p], param_names[p], min_v, max_v);
        }

        /* Cross-param summary: which params have strongest predictive power */
        printf("\n=== PARAMETER RANKING (by WR spread between top/bottom halves) ===\n");
        typedef struct {
            const char *name;
            double spread;
        } RankEntry;
        RankEntry ranks[N_PARAMS];
        for (int p = 0; p < N_PARAMS; p++) {
            int mid = N_BINS / 2;
            double high_wr = 0, low_wr = 0;
            int high_n = 0, low_n = 0;
            for (int i = mid; i < N_BINS; i++) {
                high_wr += all_bins[p][i].wr_sum;
                high_n += all_bins[p][i].count;
            }
            for (int i = 0; i < mid; i++) {
                low_wr += all_bins[p][i].wr_sum;
                low_n += all_bins[p][i].count;
            }
            double ha = high_n > 0 ? high_wr / high_n : 0;
            double la = low_n > 0 ? low_wr / low_n : 0;
            ranks[p].name = param_names[p];
            ranks[p].spread = ha - la;
        }
        /* Sort by abs spread descending */
        for (int i = 0; i < N_PARAMS; i++) {
            for (int j = i + 1; j < N_PARAMS; j++) {
                if (fabs(ranks[j].spread) > fabs(ranks[i].spread)) {
                    RankEntry tmp = ranks[i];
                    ranks[i] = ranks[j];
                    ranks[j] = tmp;
                }
            }
        }
        printf("RANK  PARAMETER             SPREAD   DIRECTION\n");
        printf("----  --------------------  -------  ---------\n");
        for (int i = 0; i < N_PARAMS; i++) {
            char dir = ranks[i].spread > 1.0 ? '^' :
                       (ranks[i].spread < -1.0 ? 'v' : '~');
            printf("  %2d  %-20s  %7.2f  %c\n",
                   i + 1, ranks[i].name, ranks[i].spread, dir);
        }
    }

    munmap(rs, sb.st_size);
    return 0;
}
