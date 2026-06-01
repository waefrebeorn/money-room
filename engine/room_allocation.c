/**
 * room_allocation.c — E47/E48/E49/E50: Portfolio Allocation & Risk
 *
 * Unified tool: reads room logs + snapshots, computes risk budget,
 * optimal capital allocation, performance-based allocation, and
 * drawdown protection.
 *
 * Build: gcc -O3 -march=native room_allocation.c -o room_allocation -lm -ljansson
 * Usage:
 *   ./room_allocation risk          — E47: portfolio risk metrics
 *   ./room_allocation allocate      — E48: optimal capital allocation
 *   ./room_allocation perf          — E49: performance-based allocation
 *   ./room_allocation drawdown      — E50: drawdown triggers & deallocation
 *   ./room_allocation all           — all of the above
 */
#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <jansson.h>

#define ROOMS_DIR "/home/wubu2/.hermes/pm_logs/rooms"
#define MAX_ROOMS 32
#define MAX_PATH 512
#define MAX_LINE 4096

typedef struct {
    char name[64];
    int  cycle;
    double wr;           /* win rate from snapshot */
    double sharpe;
    double signal;       /* -1..1 */
    double capital;      /* current capital from snapshot */
    double peak_capital;
    double drawdown_pct;
    /* Computed */
    double recent_pnl;   /* from log */
    double volatility;   /* from log */
    double sharpe_from_pnl;
    double allocation;   /* recommended allocation % */
} RoomAlloc;

/* Read room_snapshot.json */
static int read_snapshot(const char *path, RoomAlloc *ra) {
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) return 0;
    
    const char *p = strstr(path, "/rooms/");
    if (p) { p += 7; int len = 0;
        while (p[len] && p[len] != '/') len++;
        if (len > 63) len = 63;
        strncpy(ra->name, p, len); ra->name[len] = '\0'; }
    
    json_t *j = json_object_get(root, "cycle");
    if (json_is_integer(j)) ra->cycle = (int)json_integer_value(j);
    
    j = json_object_get(root, "vote_summary");
    if (json_is_object(j)) {
        int up = 0, down = 0, total = 0;
        json_t *v = json_object_get(j, "total");
        if (json_is_integer(v)) total = (int)json_integer_value(v);
        v = json_object_get(j, "up");
        if (json_is_integer(v)) up = (int)json_integer_value(v);
        v = json_object_get(j, "down");
        if (json_is_integer(v)) down = (int)json_integer_value(v);
        ra->signal = total > 0 ? (double)(up - down) / total : 0;
    }
    
    j = json_object_get(root, "stats");
    if (json_is_object(j)) {
        json_t *s;
        s = json_object_get(j, "win_rate");
        if (json_is_real(s)) ra->wr = json_real_value(s);
        s = json_object_get(j, "sharpe_ratio");
        if (json_is_real(s)) ra->sharpe = json_real_value(s);
        s = json_object_get(j, "capital_current");
        if (json_is_real(s)) ra->capital = json_real_value(s);
        s = json_object_get(j, "capital_peak");
        if (json_is_real(s)) ra->peak_capital = json_real_value(s);
    }
    json_decref(root);
    
    if (ra->peak_capital > 0)
        ra->drawdown_pct = (ra->peak_capital - ra->capital) / ra->peak_capital;
    
    return 1;
}

/* Read room_log.csv for recent stats (last 200 cycles) */
static void read_log(const char *room_dir, RoomAlloc *ra) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/room_log.csv", room_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    
    /* Count lines */
    int n = 0, ch;
    while ((ch = fgetc(f)) != EOF) if (ch == '\n') n++;
    rewind(f);
    
    int start = n - 200;
    if (start < 1) start = 1;
    
    char line[MAX_LINE];
    int cur = 0, count = 0;
    double pnl_sum = 0, pnl_sq = 0;
    
    while (fgets(line, sizeof(line), f)) {
        cur++;
        if (cur < start || cur == 1) continue;
        
        /* Extract pnl_pct (field 9) */
        int fld = 0;
        char *tok = strtok(line, ",");
        while (tok && fld < 10) {
            if (fld == 9) {
                double v = atof(tok);
                if (!isnan(v)) { pnl_sum += v; pnl_sq += v*v; count++; }
                break;
            }
            tok = strtok(NULL, ",");
            fld++;
        }
    }
    fclose(f);
    
    if (count > 0) {
        double mean = pnl_sum / count;
        double var = pnl_sq / count - mean * mean;
        ra->recent_pnl = mean;
        ra->volatility = var > 0 ? sqrt(var) : 0;
        if (ra->volatility > 1e-10)
            ra->sharpe_from_pnl = mean / ra->volatility;
    }
}

/* Risk metrics — E47 */
static void print_risk(RoomAlloc *rooms, int n) {
    printf("=== PORTFOLIO RISK (E47) ===\n");
    
    double worst_dd = 0, total_cap = 0;
    int dd_rooms = 0;
    for (int i = 0; i < n; i++) {
        total_cap += rooms[i].capital;
        if (rooms[i].drawdown_pct > worst_dd) worst_dd = rooms[i].drawdown_pct;
        if (rooms[i].drawdown_pct > 0.05) dd_rooms++;
    }
    
    printf("ROOM             CAPITAL    PEAK_CAP   DD%%     VOL       CONC%%\n");
    printf("---------------  ---------  ---------  ------  --------  -----\n");
    for (int i = 0; i < n; i++) {
        double conc = total_cap > 0 ? rooms[i].capital / total_cap * 100 : 0;
        printf("%-15s  %9.0f  %9.0f  %6.1f  %8.4f  %5.1f\n",
               rooms[i].name, rooms[i].capital, rooms[i].peak_capital,
               rooms[i].drawdown_pct * 100, rooms[i].volatility, conc);
    }
    
    printf("\nTotal capital: $%.0f\n", total_cap);
    printf("Worst drawdown: %.1f%%\n", worst_dd * 100);
    printf("Rooms in drawdown (>5%%): %d/%d\n", dd_rooms, n);
    
    /* Herfindahl concentration */
    double hhi = 0;
    for (int i = 0; i < n; i++)
        if (total_cap > 0) hhi += pow(rooms[i].capital / total_cap, 2);
    printf("Concentration (HHI): %.4f %s\n", hhi,
           hhi > 0.5 ? "⚠ HIGH" : (hhi > 0.25 ? "⚠ MODERATE" : "✅ LOW"));
}

/* Optimal allocation — E48 */
static void print_alloc(RoomAlloc *rooms, int n) {
    printf("\n=== CAPITAL ALLOCATION (E48) ===\n");
    
    /* Score: (1+Sharpe) * (1+WR) * (1-|0.5-up_ratio|) */
    for (int i = 0; i < n; i++) {
        double sh = !isnan(rooms[i].sharpe) && rooms[i].sharpe > 0 ? rooms[i].sharpe : 0;
        double wr_bonus = rooms[i].wr * 2; /* 0-2 */
        double dd_penalty = 1.0 - rooms[i].drawdown_pct;
        if (dd_penalty < 0.1) dd_penalty = 0.1;
        rooms[i].allocation = (1.0 + sh) * (0.5 + wr_bonus) * dd_penalty;
    }
    
    double total_score = 0;
    for (int i = 0; i < n; i++) total_score += rooms[i].allocation;
    
    printf("ROOM             SCORE    ALLOC%%  CONVICTION\n");
    printf("---------------  ------  -------  ----------\n");
    for (int i = 0; i < n; i++) {
        double pct = total_score > 0 ? rooms[i].allocation / total_score * 100 : 0;
        rooms[i].allocation = pct;
        char *conv = pct > 50 ? "HIGH" : (pct > 20 ? "MED" : "LOW");
        printf("%-15s  %6.2f  %7.1f  %s\n",
               rooms[i].name, rooms[i].allocation, pct, conv);
    }
}

/* Performance-based reallocation — E49 */
static void print_perf_alloc(RoomAlloc *rooms, int n) {
    printf("\n=== PERFORMANCE ALLOCATION (E49) ===\n");
    printf("Ranked by recent PnL — top rooms get more capital\n\n");
    
    /* Sort by recent PnL descending */
    for (int i = 0; i < n; i++)
        for (int j = i+1; j < n; j++)
            if (rooms[j].recent_pnl > rooms[i].recent_pnl) {
                RoomAlloc t = rooms[i]; rooms[i] = rooms[j]; rooms[j] = t;
            }
    
    printf("RANK ROOM             RECENT_PNL  SHARPE  ALLOC%%\n");
    printf("---- ---------------  ----------  ------  ------\n");
    double total_pnl = 0;
    for (int i = 0; i < n; i++) total_pnl += fmax(rooms[i].recent_pnl, 0);
    if (total_pnl <= 0) total_pnl = n;
    
    double allocs[MAX_ROOMS];
    for (int i = 0; i < n; i++) {
        double pnl = fmax(rooms[i].recent_pnl, 0);
        allocs[i] = (pnl / total_pnl) * 100;
        double sh = !isnan(rooms[i].sharpe) ? rooms[i].sharpe : 0;
        printf("  %2d  %-15s  %+10.2f  %6.2f  %6.1f\n",
               i+1, rooms[i].name, rooms[i].recent_pnl, sh, allocs[i]);
    }
}

/* Drawdown triggers — E50 */
static void print_drawdown(RoomAlloc *rooms, int n) {
    printf("\n=== DRAWDOWN PROTECTION (E50) ===\n");
    printf("Rooms exceeding drawdown thresholds\n\n");
    
    int trig = 0;
    printf("ROOM             DD%%     THRESHOLD  ACTION\n");
    printf("---------------  ------  ---------  ------\n");
    for (int i = 0; i < n; i++) {
        double dd = rooms[i].drawdown_pct * 100;
        const char *action = "OK";
        if (dd > 20) { action = "🚫 HALT"; trig++; }
        else if (dd > 10) { action = "⚠ REDUCE 50%"; trig++; }
        else if (dd > 5) { action = "⚠ REDUCE 25%"; trig++; }
        printf("%-15s  %6.1f  >5:25 >10:50  %s\n",
               rooms[i].name, dd, action);
    }
    if (trig == 0) printf("\nNo rooms triggering drawdown protection.\n");
    else printf("\n⚠ %d rooms triggering drawdown protection.\n", trig);
}

static int find_rooms(RoomAlloc *rooms, int *n) {
    DIR *dir = opendir(ROOMS_DIR);
    if (!dir) return 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *n < MAX_ROOMS) {
        if (entry->d_name[0] == '.') continue;
        
        char snap_path[MAX_PATH];
        snprintf(snap_path, sizeof(snap_path), "%s/%s/room_snapshot.json",
                 ROOMS_DIR, entry->d_name);
        
        RoomAlloc ra = {0};
        if (!read_snapshot(snap_path, &ra)) continue;
        
        char room_path[MAX_PATH];
        snprintf(room_path, sizeof(room_path), "%s/%s", ROOMS_DIR, entry->d_name);
        read_log(room_path, &ra);
        
        rooms[(*n)++] = ra;
    }
    closedir(dir);
    return *n > 0;
}

int main(int argc, char **argv) {
    int mode = 0; /* 0=all, 1=risk, 2=allocate, 3=perf, 4=drawdown */
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "risk") == 0) mode = 1;
        else if (strcmp(argv[i], "allocate") == 0) mode = 2;
        else if (strcmp(argv[i], "perf") == 0) mode = 3;
        else if (strcmp(argv[i], "drawdown") == 0) mode = 4;
        else if (strcmp(argv[i], "all") == 0) mode = 0;
    }
    
    RoomAlloc rooms[MAX_ROOMS];
    int n_rooms = 0;
    if (!find_rooms(rooms, &n_rooms)) {
        fprintf(stderr, "No rooms found\n");
        return 1;
    }
    
    printf("ROOM ALLOCATOR — %d rooms\n", n_rooms);
    
    if (mode == 0 || mode == 1) print_risk(rooms, n_rooms);
    if (mode == 0 || mode == 2) print_alloc(rooms, n_rooms);
    if (mode == 0 || mode == 3) print_perf_alloc(rooms, n_rooms);
    if (mode == 0 || mode == 4) print_drawdown(rooms, n_rooms);
    
    return 0;
}
