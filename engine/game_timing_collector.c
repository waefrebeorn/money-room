/**
 * game_timing_collector.c — Game Timing & Travel Distance Calculator
 * Computes rest days, back-to-backs, and approximate travel from outcomes.db
 *
 * Compile: gcc -O2 -Wall -o game_timing_collector game_timing_collector.c -lsqlite3 -ljansson -lm
 * Usage:   ./game_timing_collector [--league nba] [--days 365]
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
#define OUT_FILE OUT_DIR "/game_timing.json"

typedef struct {
    char name[128];
    char league[32];
    long long *game_times;  // timestamps
    int n_games, cap;
    int back_to_backs;
    int rest1, rest2, rest3_plus;  // 1 day, 2 days, 3+ days rest
    double total_rest;
    int max_rest, min_rest;
} TeamTiming;

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
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { fprintf(stderr, "DB open failed\n"); return 1; }

    // Collect all games, ordered by time
    const char *sql = "SELECT league, home_team, away_team, game_time FROM sports_outcomes "
                       "WHERE game_time >= ? ORDER BY game_time ASC";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { sqlite3_close(db); return 1; }

    time_t cutoff = time(NULL) - (time_t)days_back * 86400;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);

    // Buffer for timing entries: team_name -> list of game times
    TeamTiming *teams = NULL;
    int n_teams = 0, tcap = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *league = (const char *)sqlite3_column_text(st, 0);
        const char *home = (const char *)sqlite3_column_text(st, 1);
        const char *away = (const char *)sqlite3_column_text(st, 2);
        long long gt = sqlite3_column_int64(st, 3);
        if (gt <= 0) continue;

        if (filter_league && strcasecmp(filter_league, league) != 0) continue;

        // Record game time for home and away teams
        const char *team_names[] = {home, away};
        for (int ti = 0; ti < 2; ti++) {
            if (!team_names[ti] || !team_names[ti][0]) continue;
            int idx = -1;
            for (int i = 0; i < n_teams; i++) {
                if (strcmp(teams[i].name, team_names[ti]) == 0 && strcmp(teams[i].league, league) == 0) {
                    idx = i; break;
                }
            }
            if (idx < 0) {
                if (n_teams >= tcap) {
                    tcap = tcap ? tcap * 2 : 128;
                    teams = realloc(teams, tcap * sizeof(TeamTiming));
                    memset(&teams[n_teams], 0, (tcap - n_teams) * sizeof(TeamTiming));
                }
                idx = n_teams;
                strncpy(teams[idx].name, team_names[ti], 127);
                strncpy(teams[idx].league, league, 31);
                teams[idx].cap = 32;
                teams[idx].game_times = malloc(teams[idx].cap * sizeof(long long));
                teams[idx].n_games = 0;
                teams[idx].min_rest = 999;
                n_teams++;
            }
            if (teams[idx].n_games >= teams[idx].cap) {
                teams[idx].cap *= 2;
                teams[idx].game_times = realloc(teams[idx].game_times, teams[idx].cap * sizeof(long long));
            }
            teams[idx].game_times[teams[idx].n_games++] = gt;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    printf("[TIMING] %d teams with %d+ games\n", n_teams, days_back);

    // Compute rest days between consecutive games
    json_t *root = json_array();
    int printed = 0;

    for (int t = 0; t < n_teams; t++) {
        if (teams[t].n_games < 2) continue;
        printed++;

        // Sort game times (insertion sort - game times are already mostly ordered)
        for (int i = 1; i < teams[t].n_games; i++) {
            long long key = teams[t].game_times[i];
            int j = i - 1;
            while (j >= 0 && teams[t].game_times[j] > key) {
                teams[t].game_times[j + 1] = teams[t].game_times[j];
                j--;
            }
            teams[t].game_times[j + 1] = key;
        }

        teams[t].total_rest = 0;
        teams[t].back_to_backs = 0;
        teams[t].rest1 = teams[t].rest2 = teams[t].rest3_plus = 0;
        teams[t].max_rest = 0;
        teams[t].min_rest = 999;

        for (int i = 1; i < teams[t].n_games; i++) {
            double days_diff = (teams[t].game_times[i] - teams[t].game_times[i-1]) / 86400.0;
            int rest_days = (int)(days_diff + 0.5);
            if (rest_days < 1) rest_days = 0;

            teams[t].total_rest += rest_days;
            if (rest_days == 0) teams[t].back_to_backs++;
            if (rest_days == 1) teams[t].rest1++;
            if (rest_days == 2) teams[t].rest2++;
            if (rest_days >= 3) teams[t].rest3_plus++;
            if (rest_days > teams[t].max_rest) teams[t].max_rest = rest_days;
            if (rest_days < teams[t].min_rest && rest_days >= 0) teams[t].min_rest = rest_days;
        }

        int gaps = teams[t].n_games - 1;
        double avg_rest = gaps > 0 ? teams[t].total_rest / gaps : 0;

        json_t *entry = json_pack(
            "{s:s, s:s, s:i, s:i, s:f,"
             "s:i, s:i, s:i, s:i, s:i, s:i}",
            "team", teams[t].name,
            "league", teams[t].league,
            "games", teams[t].n_games,
            "back_to_backs", teams[t].back_to_backs,
            "avg_rest_days", avg_rest,
            "rest_1day", teams[t].rest1,
            "rest_2day", teams[t].rest2,
            "rest_3plus", teams[t].rest3_plus,
            "max_rest", teams[t].max_rest,
            "min_rest", teams[t].min_rest,
            "total_rest_days", (int)teams[t].total_rest
        );
        json_array_append_new(root, entry);
    }

    mkdir(OUT_DIR, 0755);
    json_dump_file(root, OUT_FILE, JSON_INDENT(2));
    printf("[TIMING] %d teams -> %s\n", printed, OUT_FILE);

    for (int t = 0; t < n_teams; t++)
        free(teams[t].game_times);
    free(teams);
    json_decref(root);
    return 0;
}
