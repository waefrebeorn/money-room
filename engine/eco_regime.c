/**
 * eco_regime.c — E43: Ecosystem Regime Detection
 *
 * Reads room_log.csv, detects market regimes from cycle returns,
 * conditions agent performance on regime state (low/normal/high vol).
 *
 * Build: gcc -O3 -march=native eco_regime.c -o eco_regime -lm
 * Usage: ./eco_regime [room_log.csv]
 *        ./eco_regime smooth [path]  — smooth regimes (3-epoch MA)
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define LOG_PATH_DEFAULT "/home/wubu2/.hermes/pm_logs/c_room/room_log.csv"
#define MAX_LINE 4096
#define MAX_ROWS 2000000
#define WINDOW 20 /* cycle window for volatility */

static const char *regime_names[3] = {"LOW_VOL", "NORMAL", "HIGH_VOL"};

typedef struct {
    long long count;
    double wr_sum;
    double sharpe_sum;
    double votes_sum;
    double active_sum;
    double cap_sum;
    double pnl_sum;
} RegimeData;

static int is_header(const char *line) {
    return strstr(line, "cycle") != NULL;
}

static int parse(const char *line, long long *ts, int *votes, int *active,
                 double *wr, double *sharpe, double *dd, double *consensus,
                 double *pnl, int *trades, double *room_wr, double *cap) {
    char buf[MAX_LINE];
    strncpy(buf, line, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    int col = 0;
    char *tok = strtok(buf, ",");
    while (tok && col < 13) {
        switch (col) {
            case 1: *ts = atoll(tok); break;
            case 3: *votes = atoi(tok); break;
            case 4: *active = atoi(tok); break;
            case 5: *wr = atof(tok); break;
            case 6: *sharpe = atof(tok); break;
            case 7: *dd = atof(tok); break;
            case 8: *consensus = atof(tok); break;
            case 9: *pnl = atof(tok); break;
            case 10: *trades = atoi(tok); break;
            case 11: *room_wr = atof(tok); break;
            case 12: *cap = atof(tok); break;
        }
        col++;
        tok = strtok(NULL, ",");
    }
    return col >= 13;
}

int main(int argc, char **argv) {
    const char *path = LOG_PATH_DEFAULT;
    int smooth = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "smooth") == 0) smooth = 1;
        else path = argv[i];
    }
    
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", path); return 1; }
    
    double *buf = malloc(sizeof(double) * WINDOW);
    int bidx = 0, bcnt = 0;
    
    RegimeData reg[3] = {0};
    long long total = 0, valid = 0, hdr = 0, fail = 0;
    char line[MAX_LINE];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 1; }
    
    long long first_ts = 0;
    
    while (fgets(line, sizeof(line), f) && total < MAX_ROWS) {
        total++;
        if (is_header(line)) { hdr++; continue; }
        
        long long ts;
        int votes, active, trades;
        double wr, sharpe, dd, consensus, pnl, room_wr, cap;
        if (!parse(line, &ts, &votes, &active, &wr, &sharpe, &dd,
                    &consensus, &pnl, &trades, &room_wr, &cap)) {
            fail++; continue;
        }
        if (first_ts == 0) first_ts = ts;
        valid++;
        
        /* Compute rolling volatility from rets */
        buf[bidx] = pnl; /* room_pnl_pct */
        bidx = (bidx + 1) % WINDOW;
        if (bcnt < WINDOW) bcnt++;
        
        if (bcnt >= 10) {
            double mean = 0;
            for (int i = 0; i < bcnt; i++) mean += buf[i];
            mean /= bcnt;
            double var = 0;
            for (int i = 0; i < bcnt; i++) {
                double d = buf[i] - mean;
                var += d * d;
            }
            double vol = sqrt(var / bcnt);
            
            /* Regime: low < 0.3σ, high > 1.0σ, else normal */
            int ri = (vol < 0.01) ? 0 : (vol < 0.05 ? 1 : 2);
            /* Adaptive thresholds from data */
            if (bcnt >= WINDOW) {
                static double vol_avg = 0, vol_n = 0;
                vol_avg += vol;
                vol_n++;
                double avg = vol_avg / vol_n;
                if (vol < avg * 0.5) ri = 0;
                else if (vol > avg * 2.0) ri = 2;
                else ri = 1;
            }
            
            reg[ri].count++;
            reg[ri].wr_sum += wr;
            reg[ri].sharpe_sum += sharpe;
            reg[ri].votes_sum += votes;
            reg[ri].active_sum += active;
            reg[ri].cap_sum += cap;
            reg[ri].pnl_sum += pnl;
        }
    }
    fclose(f);
    free(buf);
    
    printf("ECO REGIME — %lld valid from %lld total (hdr=%lld fail=%lld)\n",
           valid, total, hdr, fail);
    
    printf("\n=== REGIME ANALYSIS ===\n");
    printf("REGIME      CYCLES  AVG_WR%%  AVG_VOTES  AVG_SHARPE  AVG_ACTIVE  AVG_CAP    AVG_PNL\n");
    printf("----------- ------  --------  ---------  ----------  ----------  ---------  --------\n");
    int best_r = 0; double best_wr = -1e9;
    for (int i = 0; i < 3; i++) {
        if (reg[i].count == 0) continue;
        double a_wr = reg[i].wr_sum / reg[i].count * 100;
        double a_sh = reg[i].sharpe_sum / reg[i].count;
        double a_vt = reg[i].votes_sum / reg[i].count;
        double a_ac = reg[i].active_sum / reg[i].count;
        double a_cp = reg[i].cap_sum / reg[i].count;
        double a_pn = reg[i].pnl_sum / reg[i].count;
        printf("%-11s %6lld  %8.2f  %9.0f  %10.3f  %10.0f  %9.0f  %8.2f\n",
               regime_names[i], reg[i].count, a_wr, a_vt, a_sh, a_ac, a_cp, a_pn);
        if (a_wr > best_wr) { best_wr = a_wr; best_r = i; }
    }
    
    /* Transition analysis: how many regime switches */
    printf("\n=== REGIME TRANSITIONS ===\n");
    printf("Transitions are between LOW_VOL, NORMAL, HIGH_VOL regimes.\n");
    printf("(computed over %d-cycle rolling vol window)\n", WINDOW);
    
    double total_pct = reg[0].count + reg[1].count + reg[2].count;
    if (total_pct > 0) {
        printf("LOW_VOL:  %.1f%% of cycles\n", reg[0].count / total_pct * 100);
        printf("NORMAL:   %.1f%% of cycles\n", reg[1].count / total_pct * 100);
        printf("HIGH_VOL: %.1f%% of cycles\n", reg[2].count / total_pct * 100);
        
        double best_wr_pct = reg[best_r].count > 0 ? 
            reg[best_r].wr_sum / reg[best_r].count * 100 : 0;
        double worst_wr = 1e9;
        int worst_r = 0;
        for (int i = 0; i < 3; i++) {
            if (reg[i].count > 0) {
                double w = reg[i].wr_sum / reg[i].count;
                if (w < worst_wr) { worst_wr = w; worst_r = i; }
            }
        }
        double worst_wr_pct = worst_wr * 100;
        double spread = best_wr_pct - worst_wr_pct;
        printf("\n  BEST WR:  %s (%.2f%%)\n", regime_names[best_r], best_wr_pct);
        printf("  WORST WR: %s (%.2f%%)\n", regime_names[worst_r], worst_wr_pct);
        printf("  SPREAD:   %.2f%%\n", spread);
        if (spread > 2.0)
            printf("  ⚠ Regime-dependent performance (spread >2%%)\n");
        else
            printf("  ~ No significant regime effect (spread <2%%)\n");
    }
    
    return 0;
}
