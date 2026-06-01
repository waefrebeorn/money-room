/*
 * sports_outcomes.c — Sports game outcome tracker
 * Reads sports game data from timeline.db.
 * Detects completed games (status="Final", scores > 0).
 * Stores results in outcomes.db for training signal.
 *
 * gcc -O2 -o sports_outcomes sports_outcomes.c -lcurl -ljansson -lsqlite3 -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <jansson.h>

#define TL_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define OC_PATH "/home/wubu2/.hermes/pm_logs/outcomes.db"

int main(void) {
    sqlite3 *db_tl, *db_oc;

    /* Open timeline.db (source) */
    if (sqlite3_open(TL_PATH, &db_tl) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s\n", TL_PATH);
        return 1;
    }
    /* Open outcomes.db (target) */
    if (sqlite3_open(OC_PATH, &db_oc) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s\n", OC_PATH);
        sqlite3_close(db_tl);
        return 1;
    }

    /* Ensure outcomes table exists */
    sqlite3_exec(db_oc,
        "CREATE TABLE IF NOT EXISTS outcomes ("
        "market_id TEXT PRIMARY KEY, source TEXT, question TEXT, "
        "predicted_price REAL, resolved_price REAL, "
        "outcome INTEGER, resolution_time INTEGER, collected_at INTEGER, accuracy REAL"
        ");"
        "CREATE TABLE IF NOT EXISTS sports_outcomes ("
        "game_id TEXT PRIMARY KEY, league TEXT, home_team TEXT, away_team TEXT, "
        "home_score INTEGER, away_score INTEGER, winner TEXT, "
        "spread REAL, over_under REAL, status TEXT, "
        "game_time INTEGER, collected_at INTEGER"
        ");", NULL, NULL, NULL);
    sqlite3_exec(db_oc, "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;", NULL, NULL, NULL);

    /* Get list of sports sources from timeline.db */
    sqlite3_stmt *stmt;
    const char *sql = "SELECT DISTINCT source FROM timeline "
                      "WHERE source LIKE 'sports_%' AND source != 'sports_data' "
                      "ORDER BY source";
    if (sqlite3_prepare_v2(db_tl, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Query error: %s\n", sqlite3_errmsg(db_tl));
        sqlite3_close(db_tl);
        sqlite3_close(db_oc);
        return 1;
    }

    printf("SPORTS OUTCOME TRACKER\n\n");
    int total_games = 0, new_outcomes = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *source = (const char *)sqlite3_column_text(stmt, 0);
        if (!source) continue;
        const char *league = source + 7; /* skip "sports_" prefix */

        /* Get latest data per game_id from this source */
        char query[1024];
        snprintf(query, sizeof(query),
            "SELECT t1.data FROM timeline t1 "
            "INNER JOIN (SELECT json_extract(data, '$.game_id') as gid, "
            "  MAX(ts) as max_ts FROM timeline WHERE source='%s' "
            "  GROUP BY gid) t2 "
            "ON json_extract(t1.data, '$.game_id') = t2.gid "
            "AND t1.ts = t2.max_ts "
            "WHERE t1.source='%s' AND json_extract(t1.data, '$.status') != 'Scheduled' "
            "AND CAST(json_extract(t1.data, '$.home_score') AS INTEGER) > 0 "
            "AND CAST(json_extract(t1.data, '$.away_score') AS INTEGER) > 0",
            source, source);

        sqlite3_stmt *q2;
        if (sqlite3_prepare_v2(db_tl, query, -1, &q2, NULL) != SQLITE_OK) continue;

        while (sqlite3_step(q2) == SQLITE_ROW) {
            const char *json_str = (const char *)sqlite3_column_text(q2, 0);
            if (!json_str) continue;

            json_error_t err;
            json_t *j = json_loads(json_str, 0, &err);
            if (!j) continue;

            const char *game_id = json_string_value(json_object_get(j, "game_id"));
            const char *home_team = json_string_value(json_object_get(j, "home_team"));
            const char *away_team = json_string_value(json_object_get(j, "away_team"));
            int home_score = (int)json_integer_value(json_object_get(j, "home_score"));
            int away_score = (int)json_integer_value(json_object_get(j, "away_score"));
            const char *status = json_string_value(json_object_get(j, "status"));
            double spread = json_number_value(json_object_get(j, "spread"));
            double over_under = json_number_value(json_object_get(j, "over_under"));

            if (!game_id || !home_team || !away_team) {
                json_decref(j);
                continue;
            }

            total_games++;

            /* Check if already in outcomes DB */
            sqlite3_stmt *ck;
            int exists = 0;
            if (sqlite3_prepare_v2(db_oc, "SELECT 1 FROM sports_outcomes WHERE game_id=?", -1, &ck, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ck, 1, game_id, -1, SQLITE_STATIC);
                if (sqlite3_step(ck) == SQLITE_ROW) exists = 1;
                sqlite3_finalize(ck);
            }

            if (exists) { json_decref(j); continue; }

            /* Determine winner */
            const char *winner = (home_score > away_score) ? home_team : away_team;
            if (home_score == away_score) winner = "TIE";

            /* Store in outcomes DB */
            time_t now = time(NULL);
            sqlite3_stmt *ins;
            const char *isql = "INSERT OR REPLACE INTO sports_outcomes "
                "(game_id, league, home_team, away_team, home_score, away_score, "
                " winner, spread, over_under, status, game_time, collected_at) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)";
            if (sqlite3_prepare_v2(db_oc, isql, -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ins, 1, game_id, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 2, league, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 3, home_team, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 4, away_team, -1, SQLITE_STATIC);
                sqlite3_bind_int(ins, 5, home_score);
                sqlite3_bind_int(ins, 6, away_score);
                sqlite3_bind_text(ins, 7, winner, -1, SQLITE_STATIC);
                sqlite3_bind_double(ins, 8, spread);
                sqlite3_bind_double(ins, 9, over_under);
                sqlite3_bind_text(ins, 10, status, -1, SQLITE_STATIC);
                sqlite3_bind_int64(ins, 11, (long long)now);
                sqlite3_bind_int64(ins, 12, (long long)now);
                sqlite3_step(ins);
                sqlite3_finalize(ins);
                new_outcomes++;
                printf("  ✅ %s: %s %d - %d %s (%s)\n",
                       league, away_team, away_score, home_score, home_team, winner);
            }
            json_decref(j);
        }
        sqlite3_finalize(q2);
    }
    sqlite3_finalize(stmt);

    printf("\n  SUMMARY:\n");
    printf("    Total games scanned: %d\n", total_games);
    printf("    New outcomes:        %d\n", new_outcomes);

    /* Summary of all tracked outcomes */
    sqlite3_stmt *sum;
    if (sqlite3_prepare_v2(db_oc, "SELECT league, COUNT(*), MIN(home_score), MAX(home_score) "
                                   "FROM sports_outcomes GROUP BY league ORDER BY COUNT(*) DESC",
                           -1, &sum, NULL) == SQLITE_OK) {
        while (sqlite3_step(sum) == SQLITE_ROW) {
            const char *l = (const char *)sqlite3_column_text(sum, 0);
            int cnt = (int)sqlite3_column_int64(sum, 1);
            printf("    %s: %d outcomes\n", l ? l : "?", cnt);
        }
        sqlite3_finalize(sum);
    }

    sqlite3_close(db_tl);
    sqlite3_close(db_oc);
    return new_outcomes > 0 ? 0 : 0; /* Always success */
}
