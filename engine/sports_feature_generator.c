/**
 * sports_feature_generator.c — Unified Sports Feature Generator for Trainer
 * Reads ALL sports data sources and produces a consolidated feature vector
 *
 * Features per game: team_form, h2h_record, rest_days, travel_miles,
 * injury_impact, sentiment_score, home_win_pct, away_win_pct,
 * home_streak, away_streak, attendance_norm, venue_familiarity
 *
 * Compile: gcc -O2 -Wall -o sports_feature_generator sports_feature_generator.c -lsqlite3 -ljansson -lm
 * Usage:   ./sports_feature_generator
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
#include <ctype.h>
#include <sys/stat.h>

#define DB_PATH "/home/wubu2/.hermes/pm_logs/outcomes.db"
#define TL_PATH "/home/wubu2/money-room/engine/timeline.db"
#define DATA_DIR "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE DATA_DIR "/sports_training_features.json"

// Load a JSON file
static json_t *load_json(const char *path) {
    json_error_t err;
    json_t *j = json_load_file(path, 0, &err);
    return j;
}

// Find team in a JSON array by name
static json_t *find_team(json_t *arr, const char *name) {
    if (!arr || !json_is_array(arr)) return NULL;
    size_t i;
    json_t *entry;
    json_array_foreach(arr, i, entry) {
        const char *tn = json_string_value(json_object_get(entry, "team"));
        if (tn && strcmp(tn, name) == 0) return entry;
    }
    return NULL;
}

// Find H2H matchup 
static json_t *find_h2h(json_t *arr, const char *home, const char *away) {
    if (!arr || !json_is_array(arr)) return NULL;
    size_t i;
    json_t *entry;
    json_array_foreach(arr, i, entry) {
        const char *h = json_string_value(json_object_get(entry, "home"));
        const char *a = json_string_value(json_object_get(entry, "away"));
        if (h && a && strcmp(h, home) == 0 && strcmp(a, away) == 0) return entry;
        // Also check reversed
        if (h && a && strcmp(h, away) == 0 && strcmp(a, home) == 0) return entry;
    }
    return NULL;
}

// Count team injuries from injuries JSON
static int count_injuries(json_t *injuries, const char *team_name) {
    if (!injuries || !json_is_array(injuries)) return 0;
    int count = 0;
    size_t i;
    json_t *entry;
    json_array_foreach(injuries, i, entry) {
        const char *team = json_string_value(json_object_get(entry, "team"));
        if (team && strcmp(team, team_name) == 0) count++;
    }
    return count;
}

// Case-insensitive search (declared before team_sentiment)
static int strcasestr_compat(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nl = strlen(needle);
    size_t hl = strlen(haystack);
    if (nl > hl) return 0;
    for (size_t i = 0; i <= hl - nl; i++) {
        size_t j;
        for (j = 0; j < nl; j++) {
            if (tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

// Get avg sentiment from sports news for a team
static double team_sentiment(json_t *news, const char *team_name) {
    if (!news || !json_is_array(news)) return 0;
    double total = 0;
    int count = 0;
    size_t i;
    json_t *entry;
    json_array_foreach(news, i, entry) {
        const char *title = json_string_value(json_object_get(entry, "title"));
        if (!title) continue;
        if (strcasestr_compat(title, team_name)) {
            total += json_number_value(json_object_get(entry, "sentiment"));
            count++;
        }
    }
    return count > 0 ? total / count : 0;
}

int main(void) {
    // Load all data sources
    printf("[FEATURES] Loading data sources...\n");
    json_t *team_stats = load_json(DATA_DIR "/team_stats.json");
    json_t *h2h = load_json(DATA_DIR "/head2head.json");
    json_t *injuries = load_json(DATA_DIR "/injuries.json");
    json_t *news = load_json(DATA_DIR "/sports_news.json");
    json_t *timing = load_json(DATA_DIR "/game_timing.json");

    if (!team_stats) { fprintf(stderr, "No team_stats.json - run team_stats_calculator first\n"); return 1; }

    printf("[FEATURES] team_stats=%zu H2H=%zu injuries=%zu news=%zu timing=%zu\n",
           json_array_size(team_stats), h2h ? json_array_size(h2h) : 0,
           injuries ? json_array_size(injuries) : 0,
           news ? json_array_size(news) : 0,
           timing ? json_array_size(timing) : 0);

    // Read sports outcomes to generate features for each game
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { fprintf(stderr, "DB open failed\n"); return 1; }

    const char *sql = "SELECT game_id, league, home_team, away_team, home_score, away_score, "
                       "spread, over_under, game_time FROM sports_outcomes ORDER BY game_time";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { sqlite3_close(db); return 1; }

    json_t *features = json_array();
    int total = 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *gid = (const char *)sqlite3_column_text(st, 0);
        const char *league = (const char *)sqlite3_column_text(st, 1);
        const char *home = (const char *)sqlite3_column_text(st, 2);
        const char *away = (const char *)sqlite3_column_text(st, 3);
        int hs = sqlite3_column_int(st, 4);
        int as = sqlite3_column_int(st, 5);
        double spread = sqlite3_column_double(st, 6);
        double ou = sqlite3_column_double(st, 7);

        // Feature engineering
        json_t *home_s = find_team(team_stats, home);
        json_t *away_s = find_team(team_stats, away);
        json_t *matchup = h2h ? find_h2h(h2h, home, away) : NULL;

        double home_wp = home_s ? json_number_value(json_object_get(home_s, "win_pct")) : 0.5;
        double away_wp = away_s ? json_number_value(json_object_get(away_s, "win_pct")) : 0.5;
        double home_home_wp = home_s ? json_number_value(json_object_get(home_s, "home_win_pct")) : 0.5;
        double away_away_wp = away_s ? json_number_value(json_object_get(away_s, "away_win_pct")) : 0.5;

        // H2H features
        double h2h_home_pct = 0.5;
        int h2h_games = 0;
        if (matchup) {
            int hw = json_integer_value(json_object_get(matchup, "home_wins"));
            int aw = json_integer_value(json_object_get(matchup, "away_wins"));
            h2h_games = json_integer_value(json_object_get(matchup, "games"));
            // Check orientation
            const char *h = json_string_value(json_object_get(matchup, "home"));
            if (strcmp(h, home) == 0)
                h2h_home_pct = h2h_games > 0 ? (double)hw / h2h_games : 0.5;
            else
                h2h_home_pct = h2h_games > 0 ? (double)aw / h2h_games : 0.5;
            h2h_home_pct = (h2h_games > 5) ? h2h_home_pct : 0.5 + (h2h_home_pct - 0.5) * (h2h_games / 5.0);
        }

        // Injuries
        int home_inj = count_injuries(injuries, home);
        int away_inj = count_injuries(injuries, away);
        double injury_factor = 1.0 - (home_inj - away_inj) * 0.02; // More home injuries = worse
        if (injury_factor < 0.7) injury_factor = 0.7;
        if (injury_factor > 1.3) injury_factor = 1.3;

        // Sentiment
        double home_sent = team_sentiment(news, home);
        double away_sent = team_sentiment(news, away);
        double sent_diff = home_sent - away_sent; // Positive = home has better sentiment

        // Form (from team stats streak)
        const char *home_streak_s = home_s ? json_string_value(json_object_get(home_s, "streak")) : "N/A";
        const char *away_streak_s = away_s ? json_string_value(json_object_get(away_s, "streak")) : "N/A";
        int home_streak = 0, away_streak = 0;
        if (home_streak_s && home_streak_s[0] == 'W') home_streak = atoi(home_streak_s + 1);
        else if (home_streak_s && home_streak_s[0] == 'L') home_streak = -atoi(home_streak_s + 1);
        if (away_streak_s && away_streak_s[0] == 'W') away_streak = atoi(away_streak_s + 1);
        else if (away_streak_s && away_streak_s[0] == 'L') away_streak = -atoi(away_streak_s + 1);

        // Build 15-feature vector
        json_t *feat = json_pack("{s:s, s:s, s:s, s:s, s:i, s:i,"
                                  "s:f, s:f, s:f, s:f, s:f, s:f,"
                                  "s:f, s:f, s:f, s:f, s:f, s:f, s:f}",
            "game_id", gid ? gid : "",
            "league", league ? league : "",
            "home_team", home ? home : "",
            "away_team", away ? away : "",
            "home_score", hs,
            "away_score", as,
            "outcome", (hs > as) ? 1.0 : 0.0,
            // Feature vector (15 dimensions)
            "f_home_win_pct", home_wp,
            "f_away_win_pct", away_wp,
            "f_home_home_wp", home_home_wp,
            "f_away_away_wp", away_away_wp,
            "f_h2h_home_pct", h2h_home_pct,
            "f_injury_diff", (double)(away_inj - home_inj) / 5.0, // normalized
            "f_sentiment_diff", sent_diff,
            "f_home_streak_norm", home_streak / 10.0,
            "f_away_streak_norm", away_streak / 10.0,
            "f_spread", spread,
            "f_over_under", ou,
            "f_home_advantage", 1.0
        );
        json_array_append_new(features, feat);
        total++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    mkdir(DATA_DIR, 0755);
    json_dump_file(features, OUT_FILE, JSON_INDENT(2));
    printf("[FEATURES] %d games with features -> %s\n", total, OUT_FILE);

    json_decref(features);
    json_decref(team_stats);
    if (h2h) json_decref(h2h);
    if (injuries) json_decref(injuries);
    if (news) json_decref(news);
    if (timing) json_decref(timing);
    return 0;
}
