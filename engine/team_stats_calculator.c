/**
 * team_stats_calculator.c — Team Stats Calculator
 * Computes team-level stats FROM our own outcomes.db
 * No API calls — reads the 465+ sports outcomes we collected
 *
 * Stats per team: total W/L, home W/L, away W/L, streak, last 10 form,
 * points scored/allowed, avg margin, win pct
 *
 * Compile: gcc -O2 -Wall -o team_stats_calculator team_stats_calculator.c -lsqlite3 -ljansson -lm
 * Usage:   ./team_stats_calculator [--league nba] [--days 365]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include <jansson.h>
#include <strings.h>
#include <sys/stat.h>

#define DB_PATH "/home/wubu2/.hermes/pm_logs/outcomes.db"
#define OUT_DIR  "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/team_stats.json"

typedef struct {
    char name[128];
    char abbr[16];
    char league[32];
    int total_w, total_l;
    int home_w, home_l;
    int away_w, away_l;
    int streak_dir; // 1=winning, -1=losing, 0=even
    int streak_len;
    int last10[10];
    int last10_count;
    double pts_for, pts_against;
    double home_pts_for, home_pts_against;
    double away_pts_for, away_pts_against;
} TeamStats;

static int team_cmp(const void *a, const void *b) {
    const TeamStats *ta = (const TeamStats*)a;
    const TeamStats *tb = (const TeamStats*)b;
    double wa = ta->total_w / (double)(ta->total_w + ta->total_l + 1);
    double wb = tb->total_w / (double)(tb->total_w + tb->total_l + 1);
    if (wa > wb) return -1;
    if (wa < wb) return 1;
    return 0;
}

int main(int argc, char **argv) {
    int days_back = 365;
    const char *filter_league = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--days") == 0 && i+1 < argc) days_back = atoi(argv[++i]);
        else if (strcmp(argv[i], "--league") == 0 && i+1 < argc) filter_league = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strncmp(argv[i], "-h", 2) == 0) {
            printf("Usage: %s [--days N] [--league nba|nfl|...]\n", argv[0]);
            return 0;
        }
    }

    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "  DB open failed\n");
        return 1;
    }

    // Collect all unique team names from outcomes
    const char *sql = "SELECT DISTINCT league, home_team, away_team FROM sports_outcomes";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "  Query failed\n");
        sqlite3_close(db);
        return 1;
    }

    // First pass: count distinct teams
    typedef struct { char name[128]; char league[32]; } TeamKey;
    TeamKey *teams = NULL;
    int n_teams = 0, cap = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *league = (const char *)sqlite3_column_text(st, 0);
        const char *home = (const char *)sqlite3_column_text(st, 1);
        const char *away = (const char *)sqlite3_column_text(st, 2);

        if (filter_league && strcasecmp(filter_league, league) != 0) continue;

        // Add home team if new
        int found = 0;
        for (int i = 0; i < n_teams; i++) {
            if (strcmp(teams[i].name, home) == 0 && strcmp(teams[i].league, league) == 0) {
                found = 1; break;
            }
        }
        if (!found && home && home[0]) {
            if (n_teams >= cap) {
                cap = cap ? cap * 2 : 64;
                teams = realloc(teams, cap * sizeof(TeamKey));
            }
            strncpy(teams[n_teams].name, home, 127);
            strncpy(teams[n_teams].league, league, 31);
            n_teams++;
        }

        // Add away team if new
        found = 0;
        for (int i = 0; i < n_teams; i++) {
            if (strcmp(teams[i].name, away) == 0 && strcmp(teams[i].league, league) == 0) {
                found = 1; break;
            }
        }
        if (!found && away && away[0]) {
            if (n_teams >= cap) {
                cap = cap ? cap * 2 : 64;
                teams = realloc(teams, cap * sizeof(TeamKey));
            }
            strncpy(teams[n_teams].name, away, 127);
            strncpy(teams[n_teams].league, league, 31);
            n_teams++;
        }
    }
    sqlite3_finalize(st);
    printf("[TEAMSTATS] Found %d unique teams\n", n_teams);

    // Second pass: compute stats for each team
    TeamStats *stats = calloc(n_teams, sizeof(TeamStats));
    time_t cutoff = time(NULL) - (time_t)days_back * 86400;

    for (int t = 0; t < n_teams; t++) {
        strncpy(stats[t].name, teams[t].name, 127);
        strncpy(stats[t].league, teams[t].league, 31);
    }

    // Query all games within range
    const char *sql2 = "SELECT league, home_team, away_team, home_score, away_score, game_time "
                       "FROM sports_outcomes WHERE game_time >= ? ORDER BY game_time ASC";
    if (sqlite3_prepare_v2(db, sql2, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "  Query2 failed\n");
        sqlite3_close(db);
        return 1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *league = (const char *)sqlite3_column_text(st, 0);
        const char *home = (const char *)sqlite3_column_text(st, 1);
        const char *away = (const char *)sqlite3_column_text(st, 2);
        int h_score = sqlite3_column_int(st, 3);
        int a_score = sqlite3_column_int(st, 4);

        if (filter_league && strcasecmp(filter_league, league) != 0) continue;

        // Find team indices
        int hi = -1, ai = -1;
        for (int t = 0; t < n_teams; t++) {
            if (!stats[t].name[0]) continue;
            if (strcmp(stats[t].name, home) == 0 && strcmp(stats[t].league, league) == 0) hi = t;
            if (strcmp(stats[t].name, away) == 0 && strcmp(stats[t].league, league) == 0) ai = t;
        }
        if (hi < 0 || ai < 0) continue;

        // Update home team stats
        stats[hi].total_w += (h_score > a_score) ? 1 : 0;
        stats[hi].total_l += (h_score < a_score) ? 1 : 0;
        stats[hi].home_w += (h_score > a_score) ? 1 : 0;
        stats[hi].home_l += (h_score < a_score) ? 1 : 0;
        stats[hi].pts_for += h_score;
        stats[hi].pts_against += a_score;
        stats[hi].home_pts_for += h_score;
        stats[hi].home_pts_against += a_score;

        // Update away team stats
        stats[ai].total_w += (a_score > h_score) ? 1 : 0;
        stats[ai].total_l += (a_score < h_score) ? 1 : 0;
        stats[ai].away_w += (a_score > h_score) ? 1 : 0;
        stats[ai].away_l += (a_score < h_score) ? 1 : 0;
        stats[ai].pts_for += a_score;
        stats[ai].pts_against += h_score;
        stats[ai].away_pts_for += a_score;
        stats[ai].away_pts_against += h_score;

        // Last 10 form (store outcome: 1=home win, 0=away win)
        int home_won = h_score > a_score;
        if (stats[hi].last10_count < 10)
            stats[hi].last10[stats[hi].last10_count++] = home_won ? 1 : 0;
        if (stats[ai].last10_count < 10)
            stats[ai].last10[stats[ai].last10_count++] = home_won ? 0 : 1;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    // Compute streaks from last 10
    for (int t = 0; t < n_teams; t++) {
        if (stats[t].last10_count == 0) continue;
        stats[t].streak_dir = stats[t].last10[stats[t].last10_count - 1] ? 1 : -1;
        stats[t].streak_len = 1;
        for (int i = stats[t].last10_count - 2; i >= 0; i--) {
            int this_dir = stats[t].last10[i] ? 1 : -1;
            if (this_dir == stats[t].streak_dir)
                stats[t].streak_len++;
            else
                break;
        }
    }

    // Sort by win pct
    qsort(stats, n_teams, sizeof(TeamStats), team_cmp);

    // Build JSON output
    json_t *root = json_array();
    int printed = 0;
    for (int t = 0; t < n_teams; t++) {
        if (stats[t].total_w + stats[t].total_l == 0) continue;
        printed++;

        double wp = stats[t].total_w / (double)(stats[t].total_w + stats[t].total_l);
        double hwp = stats[t].home_w / (double)(stats[t].home_w + stats[t].home_l + 1);
        double awp = stats[t].away_w / (double)(stats[t].away_w + stats[t].away_l + 1);
        double avg_f = stats[t].total_w + stats[t].total_l > 0 ? stats[t].pts_for / (double)(stats[t].total_w + stats[t].total_l) : 0;
        double avg_a = stats[t].total_w + stats[t].total_l > 0 ? stats[t].pts_against / (double)(stats[t].total_w + stats[t].total_l) : 0;

        char streak_str[32];
        if (stats[t].streak_len > 0)
            snprintf(streak_str, sizeof(streak_str), "%s%d", stats[t].streak_dir > 0 ? "W" : "L", stats[t].streak_len);
        else
            snprintf(streak_str, sizeof(streak_str), "N/A");

        // Last 10 string
        char last10_str[16] = {0};
        for (int i = 0; i < stats[t].last10_count; i++)
            last10_str[i] = stats[t].last10[i] ? 'W' : 'L';
        last10_str[stats[t].last10_count] = '\0';

        json_t *entry = json_pack(
            "{s:s, s:s, s:s, s:i, s:i, s:f,"
             "s:i, s:i, s:f,"
             "s:i, s:i, s:f,"
             "s:s, s:s, s:f, s:f, s:f}",
            "team", stats[t].name,
            "league", stats[t].league,
            "record", last10_str,
            "wins", stats[t].total_w,
            "losses", stats[t].total_l,
            "win_pct", wp,
            "home_w", stats[t].home_w,
            "home_l", stats[t].home_l,
            "home_win_pct", hwp,
            "away_w", stats[t].away_w,
            "away_l", stats[t].away_l,
            "away_win_pct", awp,
            "streak", streak_str,
            "last10", last10_str,
            "avg_pts_for", avg_f,
            "avg_pts_against", avg_a,
            "avg_margin", avg_f - avg_a
        );
        json_array_append_new(root, entry);
    }

    // Write output
    mkdir(OUT_DIR, 0755);
    json_dump_file(root, OUT_FILE, JSON_INDENT(2));
    printf("[TEAMSTATS] %d teams -> %s\n", printed, OUT_FILE);

    json_decref(root);
    free(teams);
    free(stats);
    return 0;
}
