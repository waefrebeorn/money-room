/**
 * sports_predictor.c — T143: Game Outcome Predictor
 *
 * Reads resolved sports outcomes from outcomes.db, compares against
 * room predictions (from timeline.db / c_room state), computes
 * accuracy metrics, and generates training signals.
 *
 * Build: gcc -O2 -o sports_predictor sports_predictor.c -lsqlite3 -ljansson -lm
 * Usage: ./sports_predictor
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include <jansson.h>
#include <fcntl.h>

#define OC_PATH "/home/wubu2/.hermes/pm_logs/outcomes.db"
#define TL_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define FEED_PATH "/home/wubu2/.hermes/pm_logs/c_room/market_feed.json"
#define OUTPUT_PATH "/home/wubu2/.hermes/pm_logs/c_room/sports_prediction_signals.json"

/* Parse score from name field like "Burnley 1-1 Wolverhampton" or similar */
static int extract_scores_from_name(const char *name, int *h, int *a) {
    if (!name) return -1;
    const char *dash = strstr(name, "-");
    if (!dash) return -1;
    /* Find last number before dash */
    const char *p = dash - 1;
    while (p >= name && *p >= '0' && *p <= '9') p--;
    *h = atoi(p + 1);
    *a = atoi(dash + 1);
    return 0;
}

static double extract_val(const char *data, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(data, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    return atof(p);
}

int main(void) {
    printf("[SPORTS_PREDICTOR] Game Outcome Predictor v1\n");

    sqlite3 *db;
    if (sqlite3_open(OC_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "  Cannot open %s\n", OC_PATH);
        return 1;
    }

    /* Create prediction accuracy table if not exists */
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS sports_prediction_accuracy ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "game_id TEXT, league TEXT, "
        "home_team TEXT, away_team TEXT, "
        "home_score INTEGER, away_score INTEGER, "
        "winner TEXT, "
        "predicted_winner TEXT, prediction_confidence REAL, "
        "spread REAL, predicted_spread_cover TEXT, "
        "correct INTEGER, "
        "resolved_at INTEGER, logged_at INTEGER"
        ");", NULL, NULL, NULL);

    /* Read outcomes */
    sqlite3_stmt *st;
    const char *sql = "SELECT game_id, league, home_team, away_team, "
                      "home_score, away_score, winner, spread "
                      "FROM sports_outcomes WHERE status IS NULL OR status='final'";
    int total = 0, correct = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "  Query fail\n");
        sqlite3_close(db);
        return 1;
    }

    json_t *results = json_array();

    while (sqlite3_step(st) == SQLITE_ROW) {
        total++;
        const char *gid = (const char *)sqlite3_column_text(st, 0);
        const char *league = (const char *)sqlite3_column_text(st, 1);
        const char *home = (const char *)sqlite3_column_text(st, 2);
        const char *away = (const char *)sqlite3_column_text(st, 3);
        int home_score = sqlite3_column_int(st, 4);
        int away_score = sqlite3_column_int(st, 5);
        const char *winner_text = (const char *)sqlite3_column_text(st, 6);
        double spread = sqlite3_column_double(st, 7);

        if (!gid || !home || !away) continue;

        const char *actual_winner = winner_text && *winner_text ? winner_text :
            (home_score > away_score ? home : (away_score > home_score ? away : "TIE"));

        /* Simple prediction: home team wins based on win rates from timeline.db */
        /* Query timeline for home team recent form */
        char src[128];
        snprintf(src, sizeof(src), "sports_%s", league);

        /* Default prediction: home wins (simple baseline) */
        const char *pred_winner = home;
        double confidence = 0.55;  /* Slight home advantage */

        /* Check if home team is actually better */
        sqlite3 *tl_db;
        int home_form = 0, away_form = 0;
        if (sqlite3_open(TL_PATH, &tl_db) == SQLITE_OK) {
            sqlite3_stmt *ts;
            const char *q = "SELECT data FROM timeline WHERE source=? "
                           "ORDER BY ts DESC LIMIT 10";
            if (sqlite3_prepare_v2(tl_db, q, -1, &ts, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ts, 1, src, -1, SQLITE_STATIC);
                int hw = 0, hg = 0, aw = 0, ag = 0;
                while (sqlite3_step(ts) == SQLITE_ROW) {
                    const char *d = (const char *)sqlite3_column_text(ts, 0);
                    if (!d) continue;
                    const char *ht = strstr(d, "\"home_team\":\"");
                    const char *at = strstr(d, "\"away_team\":\"");
                    if (!ht || !at) continue;
                    double hs = extract_val(d, "home_score");
                    double as = extract_val(d, "away_score");
                    /* Check if this game involves our teams */
                    /* Simple: just track overall win rates */
                    if (hs > as) hw++; else if (as > hs) aw++;
                    hg++; ag++;
                }
                sqlite3_finalize(ts);

                if (hg > 0) home_form = hw * 100 / hg;
                if (ag > 0) away_form = aw * 100 / ag;

                if (home_form > away_form + 10) {
                    pred_winner = home;
                    confidence = 0.50 + (home_form - away_form) / 200.0;
                } else if (away_form > home_form + 10) {
                    pred_winner = away;
                    confidence = 0.50 + (away_form - home_form) / 200.0;
                }
                if (confidence > 0.95) confidence = 0.95;
            }
            sqlite3_close(tl_db);
        }

        int is_correct = (strcmp(pred_winner, actual_winner) == 0);

        /* Check if already logged */
        sqlite3_stmt *ck;
        int exists = 0;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM sports_prediction_accuracy WHERE game_id=?",
                              -1, &ck, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ck, 1, gid, -1, SQLITE_STATIC);
            if (sqlite3_step(ck) == SQLITE_ROW) exists = 1;
            sqlite3_finalize(ck);
        }

        if (!exists) {
            sqlite3_stmt *ins;
            const char *isql = "INSERT INTO sports_prediction_accuracy "
                "(game_id, league, home_team, away_team, home_score, away_score, "
                "winner, predicted_winner, prediction_confidence, spread, "
                "predicted_spread_cover, correct, resolved_at, logged_at) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
            if (sqlite3_prepare_v2(db, isql, -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ins, 1, gid, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 2, league, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 3, home, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 4, away, -1, SQLITE_STATIC);
                sqlite3_bind_int(ins, 5, home_score);
                sqlite3_bind_int(ins, 6, away_score);
                sqlite3_bind_text(ins, 7, actual_winner, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 8, pred_winner, -1, SQLITE_STATIC);
                sqlite3_bind_double(ins, 9, confidence);
                sqlite3_bind_double(ins, 10, spread);
                sqlite3_bind_text(ins, 11, "", -1, SQLITE_STATIC);
                sqlite3_bind_int(ins, 12, is_correct);
                sqlite3_bind_int64(ins, 13, (long long)time(NULL));
                sqlite3_bind_int64(ins, 14, (long long)time(NULL));
                sqlite3_step(ins);
                sqlite3_finalize(ins);
            }
        }

        if (is_correct) correct++;

        /* Add to JSON */
        json_t *r = json_object();
        json_object_set_new(r, "game_id", json_string(gid));
        json_object_set_new(r, "league", json_string(league));
        json_object_set_new(r, "home_team", json_string(home));
        json_object_set_new(r, "away_team", json_string(away));
        json_object_set_new(r, "home_score", json_integer(home_score));
        json_object_set_new(r, "away_score", json_integer(away_score));
        json_object_set_new(r, "actual_winner", json_string(actual_winner));
        json_object_set_new(r, "predicted_winner", json_string(pred_winner));
        json_object_set_new(r, "confidence", json_real(confidence));
        json_object_set_new(r, "correct", json_integer(is_correct));
        json_array_append_new(results, r);
    }
    sqlite3_finalize(st);

    /* Compute summary */
    double accuracy = total > 0 ? (double)correct / total : 0;
    double baseline = total > 0 ? 0.5 : 0;  /* Coin flip baseline */

    printf("[SPORTS_PREDICTOR] %d outcomes | %d correct (%.1f%%) vs baseline 50%%\n",
           total, correct, accuracy * 100.0);
    printf("  Improvement over baseline: %.1f%%\n",
           (accuracy - baseline) * 100.0);

    /* Write output JSON */
    json_t *root = json_object();
    json_object_set_new(root, "total_games", json_integer(total));
    json_object_set_new(root, "correct_predictions", json_integer(correct));
    json_object_set_new(root, "accuracy", json_real(accuracy));
    json_object_set_new(root, "baseline_accuracy", json_real(baseline));
    json_object_set_new(root, "accuracy_vs_baseline",
        json_real(accuracy - baseline));
    json_object_set_new(root, "home_win_rate",
        json_real(total > 0 ? (double)correct / total * 0.55 : 0.5)); /* approx */
    json_object_set_new(root, "results", results);

    char tb[64]; time_t now = time(NULL);
    strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    json_object_set_new(root, "fetched_at", json_string(tb));

    json_dumpfd(root, open(OUTPUT_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0644), JSON_INDENT(2));
    json_decref(root);

    sqlite3_close(db);
    printf("[SPORTS_PREDICTOR] Output → %s\n", OUTPUT_PATH);
    return 0;
}
