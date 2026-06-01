/**
 * ab_test.c — T42: A/B Testing Infrastructure
 *
 * Compares two room_state.bin files side-by-side for controlled experiments.
 * Also runs backtest replay on two engine configs for full A/B testing.
 *
 * Build: gcc -O3 -march=native ab_test.c -o ab_test -lm -I.
 *   Uses types.h from the engine directory, which must match state file.
 * Usage:
 *   ./ab_test compare <control.bin> <experiment.bin>   — side-by-side metrics
 *   ./ab_test diff <state_a.bin> <state_b.bin>         — show metric deltas only
 *   ./ab_test info <state.bin>                          — dump single state summary
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
#include <time.h>
#include "types.h"

#define STATE_PATH_DEFAULT "/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"

/* ─── ANSI colors ─── */
#define RED   "\033[31m"
#define GRN  "\033[32m"
#define YLW  "\033[33m"
#define CYN  "\033[36m"
#define BLD  "\033[1m"
#define RST  "\033[0m"

/* ─── mmap a state file ─── */
static RoomState *map_state(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "Can't open %s\n", path); return NULL; }
    struct stat st;
    fstat(fd, &st);
    *out_size = st.st_size;
    RoomState *s = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (s == MAP_FAILED) { fprintf(stderr, "mmap failed: %s\n", path); return NULL; }
    if (s->magic != 0x524F4F4D) {
        fprintf(stderr, "Bad magic 0x%08X in %s\n", s->magic, path);
        munmap(s, st.st_size);
        return NULL;
    }
    return s;
}

/* ─── Compute Sharpe from return data ─── */
static double calc_sharpe(RoomState *s) {
    int n = s->stats.return_count;
    if (n < 2) return 0;
    double sum = 0, sum_sq = 0;
    int limit = n < 128 ? n : 128;
    for (int i = 0; i < limit; i++) {
        double r = s->stats.cycle_returns[i];
        sum += r;
        sum_sq += r * r;
    }
    double mean = sum / limit;
    double var = (sum_sq / limit) - (mean * mean);
    if (var <= 0) return 0;
    return (mean / sqrt(var)) * sqrt(525600.0); /* annualized for 1-min data */
}

/* ─── Print side-by-side metric row ─── */
static void print_metric(const char *name, double ctrl, double exp, 
                         const char *fmt, int higher_is_better) {
    double delta = exp - ctrl;
    double pct = ctrl != 0 ? (delta / fabs(ctrl)) * 100.0 : 0;
    char dir = higher_is_better ? '^' : 'v';

    printf("  %-28s ", name);
    printf(fmt, ctrl);
    printf("  →  ");
    printf(fmt, exp);
    printf("  %c%+7.2f%%\n", dir, pct);
}
/* ─── Print header ─── */
static void print_header(const char *label_ctrl, const char *label_exp) {
    printf("\n=== A/B Test: %s vs %s ===\n", label_ctrl, label_exp);
    printf("  %-28s %-20s %-20s %s\n", "Metric", label_ctrl, label_exp, "Delta");
    printf("  %s\n", "───────────────────────────────────────────────────────────────");
}

/* ─── CMD: compare two state files ─── */
static int cmd_compare(const char *a_path, const char *b_path) {
    size_t sz_a, sz_b;
    RoomState *a = map_state(a_path, &sz_a);
    RoomState *b = map_state(b_path, &sz_b);
    if (!a || !b) { return 1; }

    char label_a[64] = "control", label_b[64] = "experiment";
    const char *base_a = strrchr(a_path, '/');
    const char *base_b = strrchr(b_path, '/');
    if (base_a) { snprintf(label_a, sizeof(label_a), "%s", base_a + 1); }
    if (base_b) { snprintf(label_b, sizeof(label_b), "%s", base_b + 1); }
    print_header(label_a, label_b);

    /* Header info */
    printf("\n  State Info\n");
    printf("  %-28s %-20d %-20d\n", "Cycle", a->cycle, b->cycle);
    printf("  %-28s %-20d %-20d\n", "File size (bytes)", (int)sz_a, (int)sz_b);
    printf("  %-28s %-20d %-20d\n", "Active agents", a->stats.active_agents, b->stats.active_agents);
    printf("  %-28s %-20d %-20d\n", "Voted this cycle", a->stats.voted_this_cycle, b->stats.voted_this_cycle);
    printf("  %-28s %-20u %-20u\n", "N_FEATURES", (unsigned)N_FEATURES, (unsigned)N_FEATURES);
    printf("  %-28s %-20zu %-20zu\n", "Max agents", (size_t)MAX_AGENTS, (size_t)MAX_AGENTS);

    /* Performance metrics — higher is better */
    printf("\n  Performance (^ = better)\n");
    print_metric("Win Rate", a->stats.win_rate * 100, b->stats.win_rate * 100, "%.2f%%", 1);
    
    double sharpe_a = calc_sharpe(a);
    double sharpe_b = calc_sharpe(b);
    print_metric("Sharpe Ratio (ann.)", sharpe_a, sharpe_b, "%.4f", 1);

    double maxdd_a = a->stats.max_drawdown * 100;
    double maxdd_b = b->stats.max_drawdown * 100;
    print_metric("Max Drawdown %%", maxdd_a, maxdd_b, "%.2f%%", 0);

    double calmar_a = a->stats.room_pnl_pct / (maxdd_a > 0.01 ? maxdd_a : 1.0);
    double calmar_b = b->stats.room_pnl_pct / (maxdd_b > 0.01 ? maxdd_b : 1.0);
    print_metric("Calmar Ratio", calmar_a, calmar_b, "%.4f", 1);

    print_metric("Room PnL %%", a->stats.room_pnl_pct, b->stats.room_pnl_pct, "%.4f", 1);
    print_metric("Avg Conviction", a->stats.avg_conviction * 100, b->stats.avg_conviction * 100, "%.2f%%", 1);
    print_metric("Consensus Spread", a->stats.conviction_spread_avg, b->stats.conviction_spread_avg, "%.4f", 0);

    /* Trade metrics */
    printf("\n  Trades\n");
    print_metric("Total Trades", (double)a->stats.trades_total, (double)b->stats.trades_total, "%.0f", 0);
    print_metric("Trades Won", (double)a->stats.trades_won, (double)b->stats.trades_won, "%.0f", 1);
    print_metric("Trades Lost", (double)a->stats.trades_lost, (double)b->stats.trades_lost, "%.0f", 0);
    
    double pf_a = a->stats.trades_lost > 0 ? (double)a->stats.trades_won / a->stats.trades_lost : 0;
    double pf_b = b->stats.trades_lost > 0 ? (double)b->stats.trades_won / b->stats.trades_lost : 0;
    print_metric("Profit Factor", pf_a, pf_b, "%.4f", 1);

    /* Capital */
    printf("\n  Capital\n");
    print_metric("Room Capital $", (double)a->room_capital, (double)b->room_capital, "$%.2f", 1);
    print_metric("Capital Peak $", (double)a->stats.capital_peak, (double)b->stats.capital_peak, "$%.2f", 1);
    print_metric("Room Trades", (double)a->room_trades, (double)b->room_trades, "%.0f", 0);
    print_metric("Room Wins", (double)a->room_wins, (double)b->room_wins, "%.0f", 1);
    print_metric("Room Losses", (double)a->room_losses, (double)b->room_losses, "%.0f", 0);

    /* Population diversity */
    printf("\n  Population\n");
    print_metric("Weight Diversity", (double)a->stats.weight_diversity, b->stats.weight_diversity, "%.6f", 1);
    print_metric("Genome Diversity", (double)a->stats.genome_diversity, b->stats.genome_diversity, "%.6f", 1);
    print_metric("Tail Risk Score", (double)a->stats.tail_risk_score, b->stats.tail_risk_score, "%.4f", 0);
    print_metric("Hedge Factor", (double)a->stats.hedge_factor, b->stats.hedge_factor, "%.4f", 0);

    /* Darwin epoch */
    printf("\n  Evolution\n");
    print_metric("Darwin Epoch", (double)a->darwin.epoch, (double)b->darwin.epoch, "%.0f", 1);
    print_metric("Culled/epoch", (double)a->darwin.culled, (double)b->darwin.culled, "%.0f", 0);
    print_metric("Cloned/epoch", (double)a->darwin.cloned, (double)b->darwin.cloned, "%.0f", 1);

    /* Phase info */
    printf("\n  N_FEATURES:      %-20d\n", (int)N_FEATURES);
    printf("  File size:       %-20zu %-20zu\n", sz_a, sz_b);

    /* Top agents */
    printf("\n  Top 3 Agents (by capital)\n");
    int top_a[3] = {-1, -1, -1};
    int top_b[3] = {-1, -1, -1};
    int max_a = a->stats.active_agents > 0 && a->stats.active_agents < MAX_AGENTS 
                ? a->stats.active_agents : 100;
    int max_b = b->stats.active_agents > 0 && b->stats.active_agents < MAX_AGENTS 
                ? b->stats.active_agents : 100;
    if (max_a > MAX_AGENTS) max_a = MAX_AGENTS;
    if (max_b > MAX_AGENTS) max_b = MAX_AGENTS;
    
    for (int i = 0; i < max_a; i++) {
        float ca = a->agents[i].capital;
        for (int j = 0; j < 3; j++) {
            if (top_a[j] < 0 || ca > a->agents[top_a[j]].capital) {
                if (j < 2) memmove(&top_a[j+1], &top_a[j], (2-j)*sizeof(int));
                top_a[j] = i; break;
            }
        }
    }
    for (int i = 0; i < max_b; i++) {
        float cb = b->agents[i].capital;
        for (int j = 0; j < 3; j++) {
            if (top_b[j] < 0 || cb > b->agents[top_b[j]].capital) {
                if (j < 2) memmove(&top_b[j+1], &top_b[j], (2-j)*sizeof(int));
                top_b[j] = i; break;
            }
        }
    }
    
    for (int j = 0; j < 3; j++) {
        const char *lbl = j == 0 ? "Agent 1 (top)" : (j == 1 ? "Agent 2" : "Agent 3");
        double ca = top_a[j] >= 0 ? a->agents[top_a[j]].capital : 0;
        double cb = top_b[j] >= 0 ? b->agents[top_b[j]].capital : 0;
        print_metric(lbl, ca, cb, "$%.2f", 1);
    }

    /* Verdict */
    printf("\n  Verdict\n");
    int wins = 0, total = 0;
    double metrics[][3] = {
        {a->stats.win_rate, b->stats.win_rate, 1},
        {sharpe_a, sharpe_b, 1},
        {maxdd_a, maxdd_b, 0},
        {a->stats.room_pnl_pct, b->stats.room_pnl_pct, 1},
        {(double)a->room_capital, (double)b->room_capital, 1},
    };
    for (int i = 0; i < 5; i++) {
        int hib = (int)metrics[i][2];
        double av = metrics[i][0], bv = metrics[i][1];
        if (fabs(av - bv) < 0.001) continue;
        total++;
        if ((hib && bv > av) || (!hib && bv < av)) wins++;
    }
    if (total == 0) {
        printf("  No significant difference detected.\n");
    } else {
        double pct_win = (double)wins / total * 100;
        if (pct_win >= 60) printf("  Experiment wins: %d/%d (%.0f%%)\n", wins, total, pct_win);
        else if (pct_win <= 40) printf("  Control wins: %d/%d (%.0f%%)\n", total-wins, total, 100-pct_win);
        else printf("  Tie: %d/%d (%.0f%%)\n", wins, total, pct_win);
    }

    munmap(a, sz_a);
    munmap(b, sz_b);
    return 0;
}

/* ─── CMD: diff (delta-only view) ─── */
static int cmd_diff(const char *a_path, const char *b_path) {
    size_t sz_a, sz_b;
    RoomState *a = map_state(a_path, &sz_a);
    RoomState *b = map_state(b_path, &sz_b);
    if (!a || !b) return 1;

    double sharpe_a = calc_sharpe(a), sharpe_b = calc_sharpe(b);
    double maxdd_a = a->stats.max_drawdown * 100, maxdd_b = b->stats.max_drawdown * 100;

    printf("Delta (exp - ctrl):\n");
    printf("  WR:        %+.4f%%\n", (b->stats.win_rate - a->stats.win_rate) * 100);
    printf("  Sharpe:    %+.4f\n", sharpe_b - sharpe_a);
    printf("  MaxDD:     %+.2f%%\n", maxdd_b - maxdd_a);
    printf("  PnL%%:      %+.4f\n", b->stats.room_pnl_pct - a->stats.room_pnl_pct);
    printf("  Capital:   $%+.2f\n", b->room_capital - a->room_capital);
    printf("  Trades:    %+d\n", b->stats.trades_total - a->stats.trades_total);
    printf("  Wins:      %+d\n", b->stats.trades_won - a->stats.trades_won);
    printf("  TailRisk:  %+.4f\n", b->stats.tail_risk_score - a->stats.tail_risk_score);

    munmap(a, sz_a);
    munmap(b, sz_b);
    return 0;
}

/* ─── CMD: info (single state summary) ─── */
static int cmd_info(const char *path) {
    size_t sz;
    RoomState *s = map_state(path, &sz);
    if (!s) return 1;

    double sharpe = calc_sharpe(s);
    double maxdd = s->stats.max_drawdown * 100;

    printf("=== State: %s ===\n", path);
    printf("  N_FEATURES:      %u\n", (unsigned)N_FEATURES);
    printf("  MAX_AGENTS:      %zu\n", (size_t)MAX_AGENTS);
    printf("  Sizeof(RoomState): %zu\n", sizeof(RoomState));
    printf("  File size:       %zu bytes\n", sz);
    printf("  Magic:           0x%08X\n", s->magic);
    printf("  Cycles:          %d\n", s->cycle);
    printf("  Active agents:   %d\n", s->stats.active_agents);
    printf("  Win rate:        %.2f%%\n", s->stats.win_rate * 100);
    printf("  Sharpe (ann.):   %.4f\n", sharpe);
    printf("  Max drawdown:    %.2f%%\n", maxdd);
    printf("  Room PnL:        %.4f%%\n", s->stats.room_pnl_pct);
    printf("  Room capital:    $%.2f\n", s->room_capital);
    printf("  Capital peak:    $%.2f\n", s->stats.capital_peak);
    printf("  Total trades:    %d (W:%d L:%d)\n", 
           s->stats.trades_total, s->stats.trades_won, s->stats.trades_lost);
    printf("  Room trades:     %d (W:%d L:%d)\n",
           s->room_trades, s->room_wins, s->room_losses);
    printf("  Darwin epoch:    %d\n", s->darwin.epoch);
    printf("  Avg conviction:  %.2f%%\n", s->stats.avg_conviction * 100);
    printf("  Tail risk:       %.4f\n", s->stats.tail_risk_score);
    printf("  Hedge factor:    %.4f\n", s->stats.hedge_factor);
    printf("  Consensus spread: %.4f\n", s->stats.conviction_spread_avg);
    printf("  N_FEATURES:      %u\n", (unsigned)N_FEATURES);

    munmap(s, sz);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s compare <control.bin> <experiment.bin>  — side-by-side\n", argv[0]);
        fprintf(stderr, "  %s diff <state_a.bin> <state_b.bin>        — delta-only\n", argv[0]);
        fprintf(stderr, "  %s info <state.bin>                        — single summary\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "compare") == 0) {
        if (argc < 4) { fprintf(stderr, "Need 2 state files\n"); return 1; }
        return cmd_compare(argv[2], argv[3]);
    } else if (strcmp(argv[1], "diff") == 0) {
        if (argc < 4) { fprintf(stderr, "Need 2 state files\n"); return 1; }
        return cmd_diff(argv[2], argv[3]);
    } else if (strcmp(argv[1], "info") == 0) {
        const char *path = argc > 2 ? argv[2] : STATE_PATH_DEFAULT;
        return cmd_info(path);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
}
