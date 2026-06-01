/**
 * room_aggregator.c — E44: Cross-Room Signal Aggregation
 *
 * Reads room_snapshot.json from active rooms, computes weighted consensus
 * signal across all rooms.
 *
 * Build: gcc -O3 -march=native room_aggregator.c -o room_aggregator -lm -ljansson
 * Usage: ./room_aggregator [room_dir]
 *        ./room_aggregator consensus   — show aggregate signal only
 *        ./room_aggregator json        — JSON output for dashboard
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

#define ROOMS_DIR_DEFAULT "/home/wubu2/.hermes/pm_logs/rooms"
#define MAX_ROOMS 32
#define MAX_PATH 512

typedef struct {
    char name[64];
    int  cycle;
    int  total_votes;
    int  up_votes;
    int  down_votes;
    int  active_agents;
    double win_rate;
    double sharpe;
    double max_dd;
    double consensus_spread;
    double avg_conviction;
    double room_pnl;
    double capital_current;
    double capital_peak;
    double weight;       /* computed: votes * abs(win_rate - 0.5) */
    double signal;       /* -1..1: (up-down)/total * weight */
} RoomSignal;

static int read_room_snapshot(const char *path, RoomSignal *rs) {
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) return 0;
    
    /* Extract name from dir */
    const char *slash = strrchr(path, '/');
    if (!slash) slash = path - 1;
    const char *prev = slash - 1;
    while (prev >= path && *prev != '/') prev--;
    if (prev < path) prev = path;
    else prev++;
    int slen = (int)(slash - prev);
    if (slen > 63) slen = 63;
    strncpy(rs->name, prev, slen);
    rs->name[slen] = '\0';
    
    json_t *j = json_object_get(root, "cycle");
    if (json_is_integer(j)) rs->cycle = (int)json_integer_value(j);
    
    j = json_object_get(root, "vote_summary");
    if (json_is_object(j)) {
        json_t *v;
        v = json_object_get(j, "total");
        if (json_is_integer(v)) rs->total_votes = (int)json_integer_value(v);
        v = json_object_get(j, "up");
        if (json_is_integer(v)) rs->up_votes = (int)json_integer_value(v);
        v = json_object_get(j, "down");
        if (json_is_integer(v)) rs->down_votes = (int)json_integer_value(v);
        v = json_object_get(j, "avg_conviction");
        if (json_is_real(v)) rs->avg_conviction = json_real_value(v);
        v = json_object_get(j, "consensus_spread");
        if (json_is_real(v)) rs->consensus_spread = json_real_value(v);
    }
    
    j = json_object_get(root, "stats");
    if (json_is_object(j)) {
        json_t *s;
        s = json_object_get(j, "active_agents");
        if (json_is_integer(s)) rs->active_agents = (int)json_integer_value(s);
        s = json_object_get(j, "win_rate");
        if (json_is_real(s)) rs->win_rate = json_real_value(s);
        s = json_object_get(j, "sharpe_ratio");
        if (json_is_real(s)) rs->sharpe = json_real_value(s);
        s = json_object_get(j, "max_drawdown");
        if (json_is_real(s)) rs->max_dd = json_real_value(s);
        s = json_object_get(j, "room_pnl_pct");
        if (json_is_real(s)) rs->room_pnl = json_real_value(s);
        s = json_object_get(j, "capital_current");
        if (json_is_real(s)) rs->capital_current = json_real_value(s);
        s = json_object_get(j, "capital_peak");
        if (json_is_real(s)) rs->capital_peak = json_real_value(s);
    }
    
    json_decref(root);
    
    /* Compute weight: votes × conviction (conviction = abs(win_rate-0.5)) */
    double conviction = fabs(rs->win_rate - 0.5);
    rs->weight = rs->total_votes > 0 ? rs->total_votes * conviction : 0;
    
    /* Signal: -1..1 based on vote imbalance */
    if (rs->total_votes > 0) {
        rs->signal = (double)(rs->up_votes - rs->down_votes) / rs->total_votes;
    } else {
        rs->signal = 0;
    }
    
    return 1;
}

int main(int argc, char **argv) {
    const char *rooms_dir = ROOMS_DIR_DEFAULT;
    int mode = 0; /* 0=table, 1=consensus only, 2=json */
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "consensus") == 0) mode = 1;
        else if (strcmp(argv[i], "json") == 0) mode = 2;
        else rooms_dir = argv[i];
    }
    
    DIR *dir = opendir(rooms_dir);
    if (!dir) {
        fprintf(stderr, "Error: cannot open %s\n", rooms_dir);
        return 1;
    }
    
    RoomSignal rooms[MAX_ROOMS];
    int n_rooms = 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && n_rooms < MAX_ROOMS) {
        if (entry->d_type != DT_DIR && entry->d_type != DT_LNK) continue;
        if (entry->d_name[0] == '.') continue;
        
        char snap_path[MAX_PATH];
        snprintf(snap_path, sizeof(snap_path), "%s/%s/room_snapshot.json",
                 rooms_dir, entry->d_name);
        
        RoomSignal rs = {0};
        if (read_room_snapshot(snap_path, &rs)) {
            rooms[n_rooms++] = rs;
        }
    }
    closedir(dir);
    
    if (n_rooms == 0) {
        fprintf(stderr, "No room snapshots found in %s\n", rooms_dir);
        return 1;
    }
    
    /* Compute aggregate */
    double total_weight = 0;
    double weighted_signal = 0;
    double total_votes = 0;
    int total_active = 0;
    double avg_wr = 0;
    int wr_count = 0;
    double best_sharpe = -1e9;
    const char *best_room = "";
    
    for (int i = 0; i < n_rooms; i++) {
        total_weight += rooms[i].weight;
        weighted_signal += rooms[i].signal * rooms[i].weight;
        total_votes += rooms[i].total_votes;
        total_active += rooms[i].active_agents;
        if (!isnan(rooms[i].win_rate) && rooms[i].win_rate > 0) {
            avg_wr += rooms[i].win_rate;
            wr_count++;
        }
        if (!isnan(rooms[i].sharpe) && rooms[i].sharpe > best_sharpe) {
            best_sharpe = rooms[i].sharpe;
            best_room = rooms[i].name;
        }
    }
    
    double consensus = total_weight > 0 ? weighted_signal / total_weight : 0;
    double avg_wr_all = wr_count > 0 ? avg_wr / wr_count * 100 : 0;
    
    if (mode == 1) {
        printf("CONSENSUS: %.4f (%s)\n", consensus,
               consensus > 0.1 ? "BULLISH" :
               consensus < -0.1 ? "BEARISH" : "NEUTRAL");
        printf("Rooms: %d  Total votes: %.0f  Active agents: %d\n",
               n_rooms, total_votes, total_active);
        printf("Avg WR: %.2f%%  Best Sharpe: %s (%.2f)\n",
               avg_wr_all, best_room, best_sharpe);
        return 0;
    }
    
    if (mode == 2) {
        printf("{\n");
        printf("  \"consensus\": %.4f,\n", consensus);
        printf("  \"signal\": \"%s\",\n",
               consensus > 0.1 ? "bullish" :
               consensus < -0.1 ? "bearish" : "neutral");
        printf("  \"n_rooms\": %d,\n", n_rooms);
        printf("  \"total_votes\": %.0f,\n", total_votes);
        printf("  \"total_active\": %d,\n", total_active);
        printf("  \"avg_win_rate\": %.2f,\n", avg_wr_all);
        printf("  \"rooms\": [\n");
        for (int i = 0; i < n_rooms; i++) {
            printf("    {\"name\":\"%s\",\"cycle\":%d,\"votes\":%d,\"up\":%d,\"down\":%d,"
                   "\"active\":%d,\"wr\":%.4f,\"sharpe\":%.2f,\"signal\":%.4f,\"weight\":%.0f}%s\n",
                   rooms[i].name, rooms[i].cycle, rooms[i].total_votes,
                   rooms[i].up_votes, rooms[i].down_votes,
                   rooms[i].active_agents, rooms[i].win_rate,
                   rooms[i].sharpe, rooms[i].signal, rooms[i].weight,
                   i < n_rooms - 1 ? "," : "");
        }
        printf("  ]\n}\n");
        return 0;
    }
    
    /* Table mode */
    printf("ROOM AGGREGATOR — %d rooms reporting\n", n_rooms);
    printf("CONSENSUS: %.4f (%s)\n\n", consensus,
           consensus > 0.1 ? "BULLISH" :
           consensus < -0.1 ? "BEARISH" : "NEUTRAL");
    
    printf("ROOM             CYCLE  VOTES  UP    DOWN  ACTIVE  WR%%    SHARPE  SIGNAL   WEIGHT\n");
    printf("---------------  -----  -----  ----  ----  ------  -----  ------  -------  ------\n");
    /* Sort by weight descending */
    for (int i = 0; i < n_rooms; i++) {
        for (int j = i + 1; j < n_rooms; j++) {
            if (rooms[j].weight > rooms[i].weight) {
                RoomSignal t = rooms[i]; rooms[i] = rooms[j]; rooms[j] = t;
            }
        }
    }
    for (int i = 0; i < n_rooms; i++) {
        printf("%-15s %5d  %5d  %4d  %4d  %6d  %5.1f  %6.2f  %7.4f  %6.0f\n",
               rooms[i].name, rooms[i].cycle,
               rooms[i].total_votes, rooms[i].up_votes, rooms[i].down_votes,
               rooms[i].active_agents,
               rooms[i].win_rate * 100, rooms[i].sharpe,
               rooms[i].signal, rooms[i].weight);
    }
    
    printf("\n---\n");
    printf("Total votes: %.0f  Active agents: %d  Avg WR: %.2f%%\n",
           total_votes, total_active, avg_wr_all);
    
    return 0;
}
