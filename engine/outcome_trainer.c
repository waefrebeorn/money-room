/*
 * outcome_trainer.c — Training signal generator from real outcomes
 * Reads resolved outcomes (Polymarket + sports) from outcomes.db.
 * Computes prediction accuracy per room and generates training signals.
 * Output: JSON training data for room_capital.c SGD update.
 *
 * gcc -O2 -o outcome_trainer outcome_trainer.c -lcurl -ljansson -lsqlite3 -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sqlite3.h>
#include <jansson.h>

#define OC_PATH "/home/wubu2/.hermes/pm_logs/outcomes.db"
#define TL_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"

int main(void) {
    sqlite3 *db_oc, *db_tl;

    if (sqlite3_open(OC_PATH, &db_oc) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s\n", OC_PATH);
        return 1;
    }
    if (sqlite3_open(TL_PATH, &db_tl) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s\n", TL_PATH);
        sqlite3_close(db_oc);
        return 1;
    }

    /* Create prediction_accuracy table */
    sqlite3_exec(db_oc,
        "CREATE TABLE IF NOT EXISTS prediction_accuracy ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "market_id TEXT, source TEXT, "
        "predicted_price REAL, resolved_price REAL, "
        "accuracy REAL, outcome INTEGER, "
        "resolved_at INTEGER, logged_at INTEGER"
        ");", NULL, NULL, NULL);

    printf("OUTCOME TRAINER v1 — Training signal generator\n\n");

    time_t now = time(NULL);
    int total = 0, new_entries = 0;

    /* Phase 1: Process Polymarket outcomes */
    {
        sqlite3_stmt *st;
        const char *sql = "SELECT market_id, source, predicted_price, resolved_price, "
                          "outcome, resolution_time, accuracy "
                          "FROM outcomes WHERE source='polymarket' "
                          "ORDER BY resolution_time DESC LIMIT 500";
        if (sqlite3_prepare_v2(db_oc, sql, -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *mid = (const char *)sqlite3_column_text(st, 0);
                const char *src = (const char *)sqlite3_column_text(st, 1);
                double pred = sqlite3_column_double(st, 2);
                double res_price = sqlite3_column_double(st, 3);
                int outcome = (int)sqlite3_column_int64(st, 4);
                long long res_time = sqlite3_column_int64(st, 5);
                double accuracy = sqlite3_column_double(st, 6);

                if (!mid) continue;
                total++;

                /* Check if already logged */
                sqlite3_stmt *ck;
                int exists = 0;
                if (sqlite3_prepare_v2(db_oc, "SELECT 1 FROM prediction_accuracy WHERE market_id=? AND source=?",
                                      -1, &ck, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ck, 1, mid, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ck, 2, src, -1, SQLITE_STATIC);
                    if (sqlite3_step(ck) == SQLITE_ROW) exists = 1;
                    sqlite3_finalize(ck);
                }
                if (exists) continue;

                /* Log training signal */
                sqlite3_stmt *ins;
                const char *isql = "INSERT INTO prediction_accuracy "
                    "(market_id, source, predicted_price, resolved_price, "
                    " accuracy, outcome, resolved_at, logged_at) "
                    "VALUES (?,?,?,?,?,?,?,?)";
                if (sqlite3_prepare_v2(db_oc, isql, -1, &ins, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ins, 1, mid, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 2, src, -1, SQLITE_STATIC);
                    sqlite3_bind_double(ins, 3, pred);
                    sqlite3_bind_double(ins, 4, res_price);
                    sqlite3_bind_double(ins, 5, accuracy);
                    sqlite3_bind_int(ins, 6, outcome);
                    sqlite3_bind_int64(ins, 7, res_time);
                    sqlite3_bind_int64(ins, 8, (long long)now);
                    sqlite3_step(ins);
                    sqlite3_finalize(ins);
                    new_entries++;
                }
            }
            sqlite3_finalize(st);
        }
    }

    /* Phase 2: Process sports outcomes (spread prediction accuracy) */
    {
        sqlite3_stmt *st;
        const char *sql = "SELECT game_id, league, home_team, away_team, "
                          "home_score, away_score, winner, spread "
                          "FROM sports_outcomes ORDER BY collected_at DESC LIMIT 500";
        if (sqlite3_prepare_v2(db_oc, sql, -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *gid = (const char *)sqlite3_column_text(st, 0);
                const char *league = (const char *)sqlite3_column_text(st, 1);
                const char *home = (const char *)sqlite3_column_text(st, 2);
                const char *away = (const char *)sqlite3_column_text(st, 3);
                int hs = (int)sqlite3_column_int64(st, 4);
                int as_ = (int)sqlite3_column_int64(st, 5);
                const char *winner = (const char *)sqlite3_column_text(st, 6);
                double spread = sqlite3_column_double(st, 7);
                double resolved_spread = (double)(hs - as_);

                if (!gid) continue;
                total++;

                /* Check if already logged under a polymarket-style market_id */
                char mid[128];
                snprintf(mid, sizeof(mid), "sports_%s_%s", league ? league : "?", gid);
                sqlite3_stmt *ck;
                int exists = 0;
                if (sqlite3_prepare_v2(db_oc, "SELECT 1 FROM prediction_accuracy WHERE market_id=? AND source='sports'",
                                      -1, &ck, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ck, 1, mid, -1, SQLITE_STATIC);
                    if (sqlite3_step(ck) == SQLITE_ROW) exists = 1;
                    sqlite3_finalize(ck);
                }
                if (exists) continue;

                /* Compute spread outcome: did the favorite cover? */
                int spread_outcome = (resolved_spread > spread) ? 1 : 0;
                double spread_accuracy = spread != 0 ? fmin(1.0, fabs(resolved_spread - spread) / 10.0) : 0.5;

                /* Log training signal */
                sqlite3_stmt *ins;
                const char *isql = "INSERT INTO prediction_accuracy "
                    "(market_id, source, predicted_price, resolved_price, "
                    " accuracy, outcome, resolved_at, logged_at) "
                    "VALUES (?,?,?,?,?,?,?,?)";
                if (sqlite3_prepare_v2(db_oc, isql, -1, &ins, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ins, 1, mid, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 2, "sports", -1, SQLITE_STATIC);
                    sqlite3_bind_double(ins, 3, spread);
                    sqlite3_bind_double(ins, 4, resolved_spread);
                    sqlite3_bind_double(ins, 5, spread_accuracy);
                    sqlite3_bind_int(ins, 6, spread_outcome);
                    sqlite3_bind_int64(ins, 7, (long long)now);
                    sqlite3_bind_int64(ins, 8, (long long)now);
                    sqlite3_step(ins);
                    sqlite3_finalize(ins);
                    new_entries++;
                }
            }
            sqlite3_finalize(st);
        }
    }

    /* Summary */
    sqlite3_stmt *sum;
    printf("  Processed %d outcomes, %d new training signals\n\n", total, new_entries);
    printf("  TRAINING SIGNAL SUMMARY:\n");

    if (sqlite3_prepare_v2(db_oc, "SELECT source, COUNT(*), AVG(accuracy), SUM(outcome) "
                                   "FROM prediction_accuracy GROUP BY source",
                          -1, &sum, NULL) == SQLITE_OK) {
        while (sqlite3_step(sum) == SQLITE_ROW) {
            const char *src = (const char *)sqlite3_column_text(sum, 0);
            int cnt = (int)sqlite3_column_int64(sum, 1);
            double avg = sqlite3_column_double(sum, 2);
            int yes = (int)sqlite3_column_int64(sum, 3);
            printf("    %s: %d signals, avg accuracy=%.3f, YES=%d\n", src, cnt, avg, yes);
        }
        sqlite3_finalize(sum);
    }

    /* Output JSON for room_capital.c to consume */
    printf("\n  --- JSON TRAINING SIGNAL ---\n");
    json_t *root = json_object();
    json_t *arr = json_array();

    sqlite3_stmt *js;
    if (sqlite3_prepare_v2(db_oc, "SELECT market_id, source, predicted_price, resolved_price, accuracy, outcome "
                                   "FROM prediction_accuracy ORDER BY id DESC LIMIT 50",
                          -1, &js, NULL) == SQLITE_OK) {
        while (sqlite3_step(js) == SQLITE_ROW) {
            json_t *item = json_object();
            json_object_set_new(item, "market_id", json_string((const char*)sqlite3_column_text(js, 0)));
            json_object_set_new(item, "source", json_string((const char*)sqlite3_column_text(js, 1)));
            json_object_set_new(item, "predicted", json_real(sqlite3_column_double(js, 2)));
            json_object_set_new(item, "resolved", json_real(sqlite3_column_double(js, 3)));
            json_object_set_new(item, "accuracy", json_real(sqlite3_column_double(js, 4)));
            json_object_set_new(item, "outcome", json_integer((int)sqlite3_column_int64(js, 5)));
            json_array_append_new(arr, item);
        }
        sqlite3_finalize(js);
    }

    json_object_set_new(root, "training_signals", arr);
    json_object_set_new(root, "total", json_integer(total));
    json_object_set_new(root, "new", json_integer(new_entries));
    json_object_set_new(root, "generated_at", json_integer((long long)now));

    char *out = json_dumps(root, JSON_INDENT(2));
    if (out) {
        printf("%s\n", out);
        /* Also write to file for room_capital.c to consume */
        FILE *f = fopen("/home/wubu2/.hermes/pm_logs/training_signals.json", "w");
        if (f) { fprintf(f, "%s\n", out); fclose(f); }
        free(out);
    }
    json_decref(root);

    sqlite3_close(db_oc);
    sqlite3_close(db_tl);
    return 0;
}
