/**
 * eco_seasonality.c — E42: Ecosystem Seasonality Detection
 *
 * Reads room_log.csv, analyzes day-of-week and month-of-year patterns
 * in win_rate, votes, sharpe, and consensus to find recurring cycles.
 *
 * Build: gcc -O3 -march=native eco_seasonality.c -o eco_seasonality -lm
 * Usage: ./eco_seasonality [room_log.csv]
 *        ./eco_seasonality dow [path]   — day-of-week only
 *        ./eco_seasonality moy [path]   — month-of-year only
 *        ./eco_seasonality trend [path] — weekly trend detection
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

static const char *dow_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

static const char *moy_names[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

typedef struct {
    long long count;
    double wr_sum;
    double votes_sum;
    double sharpe_sum;
    double consensus_sum;
    double active_sum;
    double cap_sum;
    int trades_total;
} BinData;

static int is_header(const char *line) {
    return strstr(line, "cycle") != NULL;
}

static int parse_csv_line(const char *line, long long *window_ts,
                          int *votes, int *active, double *win_rate,
                          double *sharpe, double *dd_pct,
                          double *consensus, double *room_pnl,
                          int *room_trades, double *room_wr,
                          double *room_cap) {
    char buf[MAX_LINE];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    
    /* Remove trailing newline */
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    
    int col = 0;
    char *token = strtok(buf, ",");
    while (token && col < 13) {
        switch (col) {
            case 0: break; /* cycle — skip */
            case 1: *window_ts = atoll(token); break;
            case 2: break; /* asset — skip */
            case 3: *votes = atoi(token); break;
            case 4: *active = atoi(token); break;
            case 5: *win_rate = atof(token); break;
            case 6: *sharpe = atof(token); break;
            case 7: *dd_pct = atof(token); break;
            case 8: *consensus = atof(token); break;
            case 9: *room_pnl = atof(token); break;
            case 10: *room_trades = atoi(token); break;
            case 11: *room_wr = atof(token); break;
            case 12: *room_cap = atof(token); break;
        }
        col++;
        token = strtok(NULL, ",");
    }
    return col >= 13;
}

static void accumulate(BinData *bin, double wr, double votes,
                       double sharpe, double consensus,
                       double active, double cap) {
    bin->count++;
    bin->wr_sum += wr;
    bin->votes_sum += votes;
    bin->sharpe_sum += sharpe;
    bin->consensus_sum += consensus;
    bin->active_sum += active;
    bin->cap_sum += cap;
}

static void print_bin_table(const char *label, BinData *bins, int n,
                            const char **names) {
    printf("\n=== %s ===\n", label);
    printf("BIN  NAME           CYCLES  AVG_WR%%  AVG_VOTES  AVG_SHARPE  AVG_ACTIVE  AVG_CAP\n");
    printf("---- ------------- ------  --------  ---------  ----------  ----------  -------\n");
    for (int i = 0; i < n; i++) {
        if (bins[i].count == 0) continue;
        double avg_wr = bins[i].wr_sum / bins[i].count * 100.0;
        double avg_votes = bins[i].votes_sum / bins[i].count;
        double avg_sharpe = bins[i].sharpe_sum / bins[i].count;
        double avg_active = bins[i].active_sum / bins[i].count;
        double avg_cap = bins[i].cap_sum / bins[i].count;
        printf("  %-2d  %-14s %6lld  %8.2f  %9.0f  %10.3f  %10.0f  %7.2f\n",
               i, names ? names[i] : "", bins[i].count,
               avg_wr, avg_votes, avg_sharpe, avg_active, avg_cap);
    }
    
    /* Best and worst bins */
    int best = 0, worst = 0;
    double best_val = -1e9, worst_val = 1e9;
    for (int i = 0; i < n; i++) {
        if (bins[i].count < 10) continue;
        double avg = bins[i].wr_sum / bins[i].count;
        if (avg > best_val) { best_val = avg; best = i; }
        if (avg < worst_val) { worst_val = avg; worst = i; }
    }
    if (bins[best].count >= 10) {
        printf("\n  BEST: %s (avg WR %.2f%%, %lld cycles)\n",
               names ? names[best] : "", best_val * 100, bins[best].count);
    }
    if (bins[worst].count >= 10 && worst != best) {
        printf("  WORST: %s (avg WR %.2f%%, %lld cycles)\n",
               names ? names[worst] : "", worst_val * 100, bins[worst].count);
    }
    
    /* Spread significance */
    double spread = (best_val - worst_val) * 100;
    printf("  SPREAD: %.2f%% (best-worst WR)\n", spread);
    if (spread > 2.0) printf("  ⚠ Notable seasonality detected (spread >2%%)\n");
    else printf("  ~ No significant seasonality (spread <2%%)\n");
}

static void detect_trend(BinData *weekly, int n_weeks) {
    printf("\n=== WEEKLY TREND ===\n");
    for (int i = 2; i < n_weeks - 2; i++) {
        double ma = 0;
        int cnt = 0;
        for (int j = i - 2; j <= i + 2; j++) {
            if (j >= 0 && j < n_weeks && weekly[j].count > 0) {
                ma += weekly[j].wr_sum / weekly[j].count;
                cnt++;
            }
        }
        if (cnt == 0) continue;
        ma /= cnt;
        double cur = weekly[i].count > 0 ? weekly[i].wr_sum / weekly[i].count : 0;
        printf("  Week %-4d cycles=%-6lld  WR=%.2f%%  MA5=%.2f%%  %s\n",
               i, weekly[i].count, cur * 100, ma * 100,
               cur > ma ? "▲" : (cur < ma ? "▼" : "—"));
    }
}

int main(int argc, char **argv) {
    const char *path = LOG_PATH_DEFAULT;
    int mode = 0; /* 0=all, 1=dow, 2=moy, 3=trend */
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "dow") == 0) mode = 1;
        else if (strcmp(argv[i], "moy") == 0) mode = 2;
        else if (strcmp(argv[i], "trend") == 0) mode = 3;
        else path = argv[i];
    }
    
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s\n", path);
        return 1;
    }
    
    BinData dow[7] = {0}, moy[12] = {0}, weekly[520] = {0};
    long long total_rows = 0, valid_rows = 0, header_count = 0, parse_fails = 0;
    char line[MAX_LINE];
    
    /* Skip header */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 1; }
    
    long long first_ts = 0, last_ts = 0;
    
    while (fgets(line, sizeof(line), f) && total_rows < MAX_ROWS) {
        total_rows++;
        if (is_header(line)) { header_count++; continue; }
        
        long long window_ts;
        int votes, active, room_trades;
        double win_rate, sharpe, dd_pct, consensus, room_pnl, room_wr, room_cap;
        
        int ok = parse_csv_line(line, &window_ts, &votes, &active,
                                 &win_rate, &sharpe, &dd_pct,
                                 &consensus, &room_pnl, &room_trades,
                                 &room_wr, &room_cap);
        if (!ok) { parse_fails++; continue; }
        
        if (first_ts == 0) first_ts = window_ts;
        last_ts = window_ts;
        
        time_t ts = (time_t)window_ts;
        struct tm *tm = localtime(&ts);
        if (!tm) { parse_fails++; continue; }
        
        int dow_idx = tm->tm_wday;
        int moy_idx = tm->tm_mon;
        int week_idx = (int)((window_ts - first_ts) / (3600 * 24 * 7));
        if (week_idx >= 520) week_idx = 519;
        
        valid_rows++;
        
        if (mode == 0 || mode == 1)
            accumulate(&dow[dow_idx], win_rate, votes, sharpe,
                      consensus, active, room_cap);
        if (mode == 0 || mode == 2)
            accumulate(&moy[moy_idx], win_rate, votes, sharpe,
                      consensus, active, room_cap);
        if (mode == 0 || mode == 3)
            accumulate(&weekly[week_idx], win_rate, votes, sharpe,
                      consensus, active, room_cap);
    }
    
    fclose(f);
    
    printf("ECO SEASONALITY — %lld valid from %lld total (hdr=%lld fail=%lld)\n",
           valid_rows, total_rows, header_count, parse_fails);
    printf("Date range: %s", ctime((time_t *)&first_ts));
    printf("           %s", ctime((time_t *)&last_ts));
    
    if (mode == 0 || mode == 1)
        print_bin_table("DAY OF WEEK", dow, 7, dow_names);
    if (mode == 0 || mode == 2)
        print_bin_table("MONTH OF YEAR", moy, 12, moy_names);
    if (mode == 0 || mode == 3)
        detect_trend(weekly, 520);
    
    return 0;
}
