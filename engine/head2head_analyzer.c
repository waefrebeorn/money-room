/**
 * head2head_analyzer.c — Historical Head-to-Head Analysis
 * Computes H2H records for all matchups from outcomes.db
 *
 * Compile: gcc -O2 -Wall -o head2head_analyzer head2head_analyzer.c -lsqlite3 -ljansson -lm
 * Usage:   ./head2head_analyzer [--league nba] [--min-games 3]
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
#define OUT_FILE OUT_DIR "/head2head.json"

typedef struct {
    char home[128], away[128], league[32];
    int h_wins, a_wins;
    double home_pts, away_pts;
    int games;
} H2H;

static int h2h_cmp(const void *a, const void *b) {
    const H2H *ha = (const H2H*)a;
    const H2H *hb = (const H2H*)b;
    int ga = ha->games > hb->games ? -1 : (ha->games < hb->games ? 1 : 0);
    return ga;
}

int main(int argc, char **argv) {
    int min_games = 3;
    const char *filter_league = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--min-games") == 0 && i+1 < argc) min_games = atoi(argv[++i]);
        else if (strcmp(argv[i], "--league") == 0 && i+1 < argc) filter_league = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strncmp(argv[i], "-h", 2) == 0) {
            printf("Usage: %s [--min-games N] [--league nba|nfl|...]\n", argv[0]);
            return 0;
        }
    }

    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { fprintf(stderr, "DB open failed\n"); return 1; }

    const char *sql = "SELECT league, home_team, away_team, home_score, away_score "
                       "FROM sports_outcomes ORDER BY game_time";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { sqlite3_close(db); return 1; }

    H2H *h2h = NULL;
    int n = 0, cap = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *league = (const char *)sqlite3_column_text(st, 0);
        const char *home = (const char *)sqlite3_column_text(st, 1);
        const char *away = (const char *)sqlite3_column_text(st, 2);
        int hs = sqlite3_column_int(st, 3);
        int as = sqlite3_column_int(st, 4);

        if (filter_league && strcasecmp(filter_league, league) != 0) continue;

        // Find matchup - try both permutations
        int idx = -1;
        for (int i = 0; i < n; i++) {
            if (strcmp(h2h[i].league, league) != 0) continue;
            int hh = (strcmp(h2h[i].home, home) == 0 && strcmp(h2h[i].away, away) == 0);
            int ha = (strcmp(h2h[i].home, away) == 0 && strcmp(h2h[i].away, home) == 0);
            if (hh) { idx = i; break; }
            if (ha) { idx = i; break; }
        }

        if (idx < 0) {
            if (n >= cap) {
                cap = cap ? cap * 2 : 256;
                h2h = realloc(h2h, cap * sizeof(H2H));
            }
            idx = n++;
            strncpy(h2h[idx].home, home, 127);
            strncpy(h2h[idx].away, away, 127);
            strncpy(h2h[idx].league, league, 31);
            h2h[idx].h_wins = h2h[idx].a_wins = 0;
            h2h[idx].home_pts = h2h[idx].away_pts = 0;
            h2h[idx].games = 0;
        }

        // Check if the game matches our stored orientation
        if (strcmp(h2h[idx].home, home) == 0 && strcmp(h2h[idx].away, away) == 0) {
            h2h[idx].h_wins += (hs > as) ? 1 : 0;
            h2h[idx].a_wins += (as > hs) ? 1 : 0;
            h2h[idx].home_pts += hs;
            h2h[idx].away_pts += as;
        } else {
            // Reversed orientation
            h2h[idx].h_wins += (as > hs) ? 1 : 0;
            h2h[idx].a_wins += (hs > as) ? 1 : 0;
            h2h[idx].home_pts += as;
            h2h[idx].away_pts += hs;
        }
        h2h[idx].games++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    qsort(h2h, n, sizeof(H2H), h2h_cmp);

    json_t *root = json_array();
    int printed = 0;
    for (int i = 0; i < n; i++) {
        if (h2h[i].games < min_games) continue;
        printed++;
        json_t *entry = json_pack(
            "{s:s, s:s, s:s, s:i, s:i, s:i, s:f, s:f, s:f}",
            "home", h2h[i].home,
            "away", h2h[i].away,
            "league", h2h[i].league,
            "games", h2h[i].games,
            "home_wins", h2h[i].h_wins,
            "away_wins", h2h[i].a_wins,
            "home_avg_pts", h2h[i].games > 0 ? h2h[i].home_pts / h2h[i].games : 0,
            "away_avg_pts", h2h[i].games > 0 ? h2h[i].away_pts / h2h[i].games : 0,
            "home_win_pct", h2h[i].games > 0 ? (double)h2h[i].h_wins / h2h[i].games : 0
        );
        json_array_append_new(root, entry);
    }

    mkdir(OUT_DIR, 0755);
    json_dump_file(root, OUT_FILE, JSON_INDENT(2));
    printf("[H2H] %d matchups (>=%d games) -> %s\n", printed, min_games, OUT_FILE);
    if (printed > 0) {
        printf("  Most common: %s (%s) vs %s (%d games)\n",
            h2h[0].home, h2h[0].league, h2h[0].away, h2h[0].games);
    }

    free(h2h);
    json_decref(root);
    return 0;
}
