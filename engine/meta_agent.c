/**
 * meta_agent.c — E46: Meta-Agent (Room Signal Selector)
 *
 * Reads room_snapshot.json and room_log.csv for each active room,
 * evaluates recent performance (windowed WR), selects the best
 * room's current signal for trading.
 *
 * Build: gcc -O3 -march=native meta_agent.c -o meta_agent -lm -ljansson
 * Usage: ./meta_agent                              — table mode
 *        ./meta_agent trade                         — output trade signal only
 *        ./meta_agent json                          — JSON for dashboard
 *        ./meta_agent <room_dir> [window=N]         — custom path/window
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
#define DEFAULT_WINDOW 100 /* cycles to evaluate */

typedef struct {
    char name[64];
    int  cycle;
    double signal;       /* -1..1 from vote summary */
    double up_ratio;     /* up/total */
    double wr;           /* win_rate from stats */
    double sharpe;
    double avg_pnl;      /* from room_log last N cycles */
    int    votes_total;
    int    active_agents;
    double score;        /* composite: WR * (1+Sharpe) * log(trades) */
    char   decision[16]; /* BUY/SELL/PASS */
} RoomEval;

/* Read room_snapshot.json */
static int read_snapshot(const char *path, RoomEval *re) {
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) return 0;
    
    /* Extract name from path */
    const char *p = strstr(path, "/rooms/");
    if (p) {
        p += 7;
        int len = 0;
        while (p[len] && p[len] != '/') len++;
        if (len > 63) len = 63;
        strncpy(re->name, p, len);
        re->name[len] = '\0';
    }
    
    json_t *j = json_object_get(root, "cycle");
    if (json_is_integer(j)) re->cycle = (int)json_integer_value(j);
    
    j = json_object_get(root, "vote_summary");
    if (json_is_object(j)) {
        json_t *v;
        int up = 0, down = 0, total = 0;
        v = json_object_get(j, "total");
        if (json_is_integer(v)) total = (int)json_integer_value(v);
        v = json_object_get(j, "up");
        if (json_is_integer(v)) up = (int)json_integer_value(v);
        v = json_object_get(j, "down");
        if (json_is_integer(v)) down = (int)json_integer_value(v);
        re->votes_total = total;
        re->signal = total > 0 ? (double)(up - down) / total : 0;
        re->up_ratio = total > 0 ? (double)up / total : 0.5;
    }
    
    j = json_object_get(root, "stats");
    if (json_is_object(j)) {
        json_t *s;
        s = json_object_get(j, "win_rate");
        if (json_is_real(s)) re->wr = json_real_value(s);
        s = json_object_get(j, "sharpe_ratio");
        if (json_is_real(s)) re->sharpe = json_real_value(s);
        s = json_object_get(j, "active_agents");
        if (json_is_integer(s)) re->active_agents = (int)json_integer_value(s);
    }
    
    json_decref(root);
    return 1;
}

/* Read room_log.csv last N cycles for avg PnL */
static double read_recent_pnl(const char *room_dir, int window) {
    char log_path[MAX_PATH];
    snprintf(log_path, sizeof(log_path), "%s/room_log.csv", room_dir);
    
    FILE *f = fopen(log_path, "r");
    if (!f) return 0;
    
    /* Count lines */
    int n = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) if (ch == '\n') n++;
    rewind(f);
    
    /* Read from end */
    int start_line = n - window;
    if (start_line < 1) start_line = 1;
    
    char line[MAX_LINE];
    int current = 0;
    double pnl_sum = 0;
    int count = 0;
    
    while (fgets(line, sizeof(line), f)) {
        current++;
        if (current < start_line || current == 1) continue; /* skip header */
        
        double pnl = 0;
        /* CSV: cycle,window_ts,asset,votes,active,wr,sharpe,dd,consensus,pnl,... */
        int fields = 0;
        char *tok = strtok(line, ",");
        while (tok && fields < 11) {
            if (fields == 9) { pnl = atof(tok); break; } /* room_pnl_pct */
            tok = strtok(NULL, ",");
            fields++;
        }
        pnl_sum += pnl;
        count++;
    }
    fclose(f);
    return count > 0 ? pnl_sum / count : 0;
}

int main(int argc, char **argv) {
    const char *rooms_dir = ROOMS_DIR;
    int window = DEFAULT_WINDOW;
    int mode = 0; /* 0=table, 1=signal-only, 2=json */
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "trade") == 0) mode = 1;
        else if (strcmp(argv[i], "json") == 0) mode = 2;
        else if (strncmp(argv[i], "window=", 7) == 0)
            window = atoi(argv[i] + 7);
        else rooms_dir = argv[i];
    }
    
    /* Scan rooms */
    DIR *dir = opendir(rooms_dir);
    if (!dir) { fprintf(stderr, "Error: cannot open %s\n", rooms_dir); return 1; }
    
    RoomEval rooms[MAX_ROOMS];
    int n_rooms = 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && n_rooms < MAX_ROOMS) {
        if (entry->d_name[0] == '.') continue;
        char snap_path[MAX_PATH];
        snprintf(snap_path, sizeof(snap_path), "%s/%s/room_snapshot.json",
                 rooms_dir, entry->d_name);
        
        RoomEval re = {0};
        if (!read_snapshot(snap_path, &re)) continue;
        
        /* Get recent PnL */
        char room_path[MAX_PATH];
        snprintf(room_path, sizeof(room_path), "%s/%s", rooms_dir, entry->d_name);
        re.avg_pnl = read_recent_pnl(room_path, window);
        
        rooms[n_rooms++] = re;
    }
    closedir(dir);
    
    if (n_rooms == 0) { fprintf(stderr, "No rooms found\n"); return 1; }
    
    /* Score each room */
    for (int i = 0; i < n_rooms; i++) {
        RoomEval *r = &rooms[i];
        /* Score = WR * (1+Sharpe) * confidence, where confidence = |signal| */
        double conf = fabs(r->signal);
        double sharpe_factor = 1.0;
        if (!isnan(r->sharpe) && r->sharpe > 0) sharpe_factor += r->sharpe;
        r->score = r->wr * sharpe_factor * (0.5 + conf * 0.5);
        
        /* Decision */
        if (r->score > 0.35 && r->signal > 0.05) strcpy(r->decision, "BUY");
        else if (r->score > 0.35 && r->signal < -0.05) strcpy(r->decision, "SELL");
        else strcpy(r->decision, "PASS");
    }
    
    /* Sort by score descending */
    for (int i = 0; i < n_rooms; i++)
        for (int j = i+1; j < n_rooms; j++)
            if (rooms[j].score > rooms[i].score) {
                RoomEval t = rooms[i]; rooms[i] = rooms[j]; rooms[j] = t;
            }
    
    /* Best room */
    RoomEval *best = &rooms[0];
    
    if (mode == 1) {
        printf("%s %s sig=%+.4f score=%.4f margin=%+.4f\n",
               best->name, best->decision, best->signal, best->score,
               n_rooms > 1 ? rooms[0].score - rooms[1].score : 0);
        return 0;
    }
    
    if (mode == 2) {
        printf("{\n");
        printf("  \"selection\": \"%s\",\n", best->name);
        printf("  \"decision\": \"%s\",\n", best->decision);
        printf("  \"signal\": %.4f,\n", best->signal);
        printf("  \"score\": %.4f,\n", best->score);
        printf("  \"n_rooms\": %d,\n", n_rooms);
        printf("  \"rooms\": [\n");
        for (int i = 0; i < n_rooms; i++) {
            printf("    {\"name\":\"%s\",\"cycle\":%d,\"signal\":%.4f,\"wr\":%.4f,"
                   "\"sharpe\":%.2f,\"avg_pnl\":%.2f,\"score\":%.4f,\"decision\":\"%s\"}%s\n",
                   rooms[i].name, rooms[i].cycle, rooms[i].signal, rooms[i].wr,
                   rooms[i].sharpe, rooms[i].avg_pnl, rooms[i].score, rooms[i].decision,
                   i < n_rooms - 1 ? "," : "");
        }
        printf("  ]\n}\n");
        return 0;
    }
    
    /* Table mode */
    printf("META-AGENT — %d rooms, window=%d cycles\n", n_rooms, window);
    printf("SELECTED: %s → %s  (signal=%.4f, score=%.4f)\n\n",
           best->name, best->decision, best->signal, best->score);
    
    printf("ROOM             CYCLE  SIGNAL    WR%%    SHARPE  AVG_PNL   SCORE   DECISION\n");
    printf("---------------  -----  -------  ------  ------  --------  ------  --------\n");
    for (int i = 0; i < n_rooms; i++) {
        printf("%-15s  %5d  %+.4f  %6.2f  %6.2f  %+8.2f  %6.4f  %s\n",
               rooms[i].name, rooms[i].cycle, rooms[i].signal,
               rooms[i].wr * 100, rooms[i].sharpe,
               rooms[i].avg_pnl, rooms[i].score, rooms[i].decision);
    }
    
    printf("\nComposite score = WR × (1+Sharpe) × conviction\n");
    
    return 0;
}
