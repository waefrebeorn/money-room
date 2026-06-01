/**
 * room_correlation.c — E45: Cross-Room Signal Correlation
 *
 * Reads room_snapshot.json files from active rooms, records vote ratios
 * to a history DB, computes pairwise signal correlation matrix.
 *
 * Build: gcc -O3 -march=native room_correlation.c -o room_correlation -lm -ljansson
 * Usage: ./room_correlation                        — live snapshot + correlation
 *        ./room_correlation history [db_path]      — show history table
 *        ./room_correlation live [room_dir]        — add snapshot, compute
 */
#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <jansson.h>

#define ROOMS_DIR_DEFAULT "/home/wubu2/.hermes/pm_logs/rooms"
#define MAX_ROOMS 32
#define MAX_PATH 512
#define HIST_FILE "/home/wubu2/.hermes/pm_logs/room_signals.csv"
#define MAX_HIST 100000

typedef struct {
    char name[64];
    double up_ratio;  /* up_votes / total_votes */
    int total_votes;
} RoomPoint;

typedef struct {
    time_t ts;
    int n_rooms;
    RoomPoint points[MAX_ROOMS];
    double sig[MAX_ROOMS]; /* up_ratio mapped to -1..1 */
} Snapshot;

static Snapshot history[MAX_HIST];
static int hist_count = 0;

/* ── Read all room snapshots → single snapshot ── */
static int read_all_rooms(const char *rooms_dir, Snapshot *snap) {
    DIR *dir = opendir(rooms_dir);
    if (!dir) return 0;
    
    memset(snap, 0, sizeof(Snapshot));
    snap->ts = time(NULL);
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && snap->n_rooms < MAX_ROOMS) {
        if (entry->d_name[0] == '.') continue;
        
        char snap_path[MAX_PATH];
        snprintf(snap_path, sizeof(snap_path), "%s/%s/room_snapshot.json",
                 rooms_dir, entry->d_name);
        
        json_error_t err;
        json_t *root = json_load_file(snap_path, 0, &err);
        if (!root) continue;
        
        RoomPoint *p = &snap->points[snap->n_rooms];
        strncpy(p->name, entry->d_name, sizeof(p->name) - 1);
        
        json_t *vs = json_object_get(root, "vote_summary");
        if (json_is_object(vs)) {
            json_t *jup = json_object_get(vs, "up");
            json_t *jdown = json_object_get(vs, "down");
            json_t *jtotal = json_object_get(vs, "total");
            if (json_is_integer(jtotal) && json_integer_value(jtotal) > 0) {
                int total = (int)json_integer_value(jtotal);
                int up = json_is_integer(jup) ? (int)json_integer_value(jup) : 0;
                p->total_votes = total;
                p->up_ratio = (double)up / total;
                snap->n_rooms++;
            }
        }
        json_decref(root);
    }
    closedir(dir);
    return snap->n_rooms > 0;
}

/* ── Append snapshot to history file (CSV) ── */
static void append_history(Snapshot *snap) {
    if (hist_count >= MAX_HIST) return;
    history[hist_count++] = *snap;
    
    FILE *f = fopen(HIST_FILE, "a");
    if (!f) return;
    fprintf(f, "%ld", (long)snap->ts);
    for (int i = 0; i < snap->n_rooms; i++) {
        fprintf(f, ",%s,%.6f,%d",
                snap->points[i].name,
                snap->points[i].up_ratio,
                snap->points[i].total_votes);
    }
    fprintf(f, "\n");
    fclose(f);
}

/* ── Load history from file ── */
static int load_history(void) {
    FILE *f = fopen(HIST_FILE, "r");
    if (!f) return 0;
    char line[4096];
    hist_count = 0;
    while (fgets(line, sizeof(line), f) && hist_count < MAX_HIST) {
        Snapshot *s = &history[hist_count];
        memset(s, 0, sizeof(Snapshot));
        char *tok = strtok(line, ",\n");
        if (!tok) continue;
        s->ts = atol(tok);
        int col = 0;
        while ((tok = strtok(NULL, ",\n")) && s->n_rooms < MAX_ROOMS) {
            switch (col % 3) {
                case 0: strncpy(s->points[s->n_rooms].name, tok,
                               sizeof(s->points[s->n_rooms].name)-1); break;
                case 1: s->points[s->n_rooms].up_ratio = atof(tok); break;
                case 2: s->points[s->n_rooms].total_votes = atoi(tok);
                        s->n_rooms++; break;
            }
            col++;
        }
        if (s->n_rooms > 0) hist_count++;
    }
    fclose(f);
    return hist_count;
}

/* ── Compute signal from up_ratio: -1..1 ── */
static double signal_from_ratio(double up_ratio) {
    return (up_ratio - 0.5) * 2.0; /* 0%→-1, 50%→0, 100%→1 */
}

/* ── Find room index in snapshot ── */
static int room_idx(Snapshot *s, const char *name) {
    for (int i = 0; i < s->n_rooms; i++)
        if (strcmp(s->points[i].name, name) == 0) return i;
    return -1;
}

/* ── Compute pairwise correlation matrix ── */
static void compute_correlation(const char **room_names, int n_rooms) {
    /* Build time series for each room */
    int n = hist_count;
    double *means = calloc(n_rooms, sizeof(double));
    int *counts = calloc(n_rooms, sizeof(int));
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n_rooms; j++) {
            int ri = room_idx(&history[i], room_names[j]);
            if (ri >= 0) {
                means[j] += signal_from_ratio(history[i].points[ri].up_ratio);
                counts[j]++;
            }
        }
    }
    
    for (int j = 0; j < n_rooms; j++)
        if (counts[j] > 0) means[j] /= counts[j];
    
    printf("\n=== SIGNAL CORRELATION MATRIX ===\n");
    printf("Rooms: %d  Snapshots: %d\n\n", n_rooms, n);
    
    /* Header */
    printf("%-14s", "");
    for (int j = 0; j < n_rooms; j++)
        printf("  %-12s", room_names[j]);
    printf("  MEAN_SIG\n");
    
    double **mat = calloc(n_rooms, sizeof(double*));
    for (int i = 0; i < n_rooms; i++)
        mat[i] = calloc(n_rooms, sizeof(double));
    
    for (int i = 0; i < n_rooms; i++) {
        printf("%-14s", room_names[i]);
        for (int j = 0; j < n_rooms; j++) {
            double cov = 0, var_i = 0, var_j = 0;
            int pairs = 0;
            for (int k = 0; k < n; k++) {
                int ri = room_idx(&history[k], room_names[i]);
                int rj = room_idx(&history[k], room_names[j]);
                if (ri >= 0 && rj >= 0) {
                    double si = signal_from_ratio(history[k].points[ri].up_ratio) - means[i];
                    double sj = signal_from_ratio(history[k].points[rj].up_ratio) - means[j];
                    cov += si * sj;
                    var_i += si * si;
                    var_j += sj * sj;
                    pairs++;
                }
            }
            double r = 0;
            if (pairs > 2 && var_i > 1e-10 && var_j > 1e-10)
                r = cov / sqrt(var_i * var_j);
            mat[i][j] = r;
            
            char buf[16];
            if (fabs(r) < 0.01 && r != 0) snprintf(buf, sizeof(buf), "%.3f", r);
            else snprintf(buf, sizeof(buf), "%+.3f", r);
            printf("  %-12s", buf);
        }
        printf("  %+8.4f\n", means[i]);
    }
    
    /* Find highest/lowest correlations */
    double max_r = -2, min_r = 2;
    const char *max_pair = "", *min_pair = "";
    for (int i = 0; i < n_rooms; i++) {
        for (int j = i + 1; j < n_rooms; j++) {
            if (mat[i][j] > max_r) { max_r = mat[i][j]; max_pair = room_names[i]; }
            if (mat[i][j] < min_r) { min_r = mat[i][j]; min_pair = room_names[i]; }
        }
    }
    
    printf("\nHighest correlation: r=%+.3f (%s)\n", max_r, max_pair);
    printf("Lowest correlation:  r=%+.3f (%s)\n", min_r, min_pair);
    
    for (int i = 0; i < n_rooms; i++) free(mat[i]);
    free(mat);
    free(means);
    free(counts);
}

int main(int argc, char **argv) {
    const char *rooms_dir = ROOMS_DIR_DEFAULT;
    int mode = 0; /* 0=live+correlate, 1=history, 2=live-only */
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "history") == 0) mode = 1;
        else if (strcmp(argv[i], "live") == 0) mode = 2;
        else rooms_dir = argv[i];
    }
    
    if (mode == 1) {
        int n = load_history();
        if (n == 0) { fprintf(stderr, "No history found\n"); return 1; }
        printf("HISTORY: %d snapshots from %s\n", n, HIST_FILE);
        
        /* Collect all room names seen in history */
        char *seen[MAX_ROOMS];
        int n_seen = 0;
        for (int i = 0; i < n && n_seen < MAX_ROOMS; i++) {
            for (int j = 0; j < history[i].n_rooms; j++) {
                int found = 0;
                for (int k = 0; k < n_seen; k++)
                    if (strcmp(seen[k], history[i].points[j].name) == 0) { found = 1; break; }
                if (!found) seen[n_seen++] = history[i].points[j].name;
            }
        }
        
        compute_correlation((const char**)seen, n_seen);
        return 0;
    }
    
    /* Live mode: read current rooms */
    Snapshot snap;
    if (!read_all_rooms(rooms_dir, &snap)) {
        fprintf(stderr, "No room snapshots found\n");
        return 1;
    }
    
    printf("LIVE SNAPSHOT — %d rooms at %s",
           snap.n_rooms, ctime(&snap.ts));
    printf("%-15s  VOTES  UP_RATIO  SIGNAL\n", "ROOM");
    printf("---------------  -----  --------  ------\n");
    
    const char *names[MAX_ROOMS];
    for (int i = 0; i < snap.n_rooms; i++) {
        double sig = signal_from_ratio(snap.points[i].up_ratio);
        names[i] = snap.points[i].name;
        printf("%-15s  %5d  %8.4f  %+.4f\n",
               snap.points[i].name,
               snap.points[i].total_votes,
               snap.points[i].up_ratio, sig);
    }
    
    /* Append to history */
    append_history(&snap);
    printf("\nSnapshot appended to %s (%d total)\n", HIST_FILE, hist_count);
    
    /* Load full history for correlation */
    int h = load_history();
    if (h >= 3) {
        compute_correlation(names, snap.n_rooms);
    } else {
        printf("\nNeed at least 3 snapshots for correlation (have %d)\n", h);
    }
    
    return 0;
}
