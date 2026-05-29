/**
 * prediction_logger.c — T161: Per-Room Prediction Logger & Accuracy Scorer
 *
 * Reads market_feed.json for room consensus predictions,
 * logs them to outcomes.db predictions table,
 * correlates with resolved outcomes for accuracy scoring.
 *
 * Build: gcc -O2 -o prediction_logger prediction_logger.c -lsqlite3 -ljansson -lm
 * Usage: ./prediction_logger [--score-only]
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
#define FEED_DIR "/home/wubu2/.hermes/pm_logs/c_room"
#define MAX_ROOMS 16

/* Room configurations */
static const char *rooms[] = {
    "momentum", "reversal", "breakout", "scalper",
    "swing", "position", "grid", "arb",
    "mean_reversion", "volatility", "trend", "range",
    "dca", "twap", "vwap", "sentiment"
};

/* Extract float from JSON object */
static double json_get(const json_t *o, const char *key) {
    json_t *v = json_object_get(o, key);
    return v ? json_number_value(v) : 0;
}

int main(int argc, char **argv) {
    int score_only = (argc > 1 && strcmp(argv[1], "--score-only") == 0);
    printf("[PRED_LOGGER] Per-Room Prediction Logger v1\n");

    sqlite3 *db;
    if (sqlite3_open(OC_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "  DB open fail: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    /* Ensure predictions table exists with per-room tracking */
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS predictions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "market_id TEXT, room_id INTEGER, "
        "predicted_price REAL, predicted_at INTEGER, "
        "source TEXT"
        ");", NULL, NULL, NULL);

    int logged = 0, scored = 0;
    time_t now = time(NULL);

    if (!score_only) {
        /* Phase 1: Log predictions from each room's market_feed */
        for (int r = 0; r < MAX_ROOMS; r++) {
            char path[256];
            snprintf(path, sizeof(path), "%s/market_feed.json", FEED_DIR);

            /* Read market_feed.json */
            FILE *f = fopen(path, "r");
            if (!f) {
                /* Try per-room files */
                snprintf(path, sizeof(path), "%s/%s/market_feed.json", FEED_DIR, rooms[r]);
                f = fopen(path, "r");
                if (!f) continue;
            }

            fseek(f, 0, SEEK_END);
            long flen = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (flen < 10 || flen > 1048576) { fclose(f); continue; }

            char *buf = malloc((size_t)flen + 1);
            fread(buf, 1, (size_t)flen, f);
            buf[flen] = 0;
            fclose(f);

            json_error_t err;
            json_t *root = json_loads(buf, 0, &err);
            free(buf);
            if (!root) continue;

            /* Extract room data from feed */
            const char *feed_room = json_string_value(json_object_get(root, "room"));
            double price = json_get(root, "price");
            double volume = json_get(root, "volume");
            double signal = json_get(root, "price_momentum");

            /* Check if this is our target room */
            // If it's the central feed, log for all rooms
            if (!feed_room || strcmp(feed_room, rooms[r]) != 0) {
                /* Central feed — create synthetic predictions for each room */
                json_decref(root);
                root = NULL;

                /* Use the room's own state file if it exists */
                char state_path[256];
                snprintf(state_path, sizeof(state_path), "%s/%s_state.bin", FEED_DIR, rooms[r]);
                FILE *sf = fopen(state_path, "r");
                if (sf) { fclose(sf); } else continue;

                /* Create a prediction entry */
                sqlite3_stmt *ins;
                const char *isql = "INSERT INTO predictions "
                    "(market_id, room_id, predicted_price, predicted_at, source) "
                    "VALUES (?, ?, ?, ?, ?)";
                if (sqlite3_prepare_v2(db, isql, -1, &ins, NULL) == SQLITE_OK) {
                    char mkt_id[64];
                    snprintf(mkt_id, 64, "room_%s_%ld", rooms[r], (long)now);
                    sqlite3_bind_text(ins, 1, mkt_id, -1, SQLITE_STATIC);
                    sqlite3_bind_int(ins, 2, r);
                    sqlite3_bind_double(ins, 3, signal > 0 ? 0.6 : 0.4);
                    sqlite3_bind_int64(ins, 4, (long long)now);
                    sqlite3_bind_text(ins, 5, "market_feed", -1, SQLITE_STATIC);
                    if (sqlite3_step(ins) == SQLITE_DONE) logged++;
                    sqlite3_finalize(ins);
                }
                continue;
            }

            json_decref(root);
        }
    }

    /* Phase 2: Score predictions against resolved outcomes */
    {
        sqlite3_stmt *st;
        const char *sql = "SELECT p.market_id, p.room_id, p.predicted_price, "
                          "o.resolved_price, o.outcome "
                          "FROM predictions p "
                          "LEFT JOIN outcomes o ON p.market_id = o.market_id "
                          "WHERE o.resolved_price IS NOT NULL "
                          "AND p.id NOT IN (SELECT prediction_id FROM prediction_accuracy WHERE prediction_id IS NOT NULL)";

        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *mkt_id = (const char *)sqlite3_column_text(st, 0);
                int room_id = sqlite3_column_int(st, 1);
                double pred_price = sqlite3_column_double(st, 2);
                double res_price = sqlite3_column_double(st, 3);
                int outcome = sqlite3_column_int(st, 4);
                int prediction_id = 0; /* would need prediction id column */

                if (!mkt_id) continue;

                /* Compute accuracy */
                double accuracy = 1.0 - fabs(pred_price - res_price);
                if (accuracy < 0) accuracy = 0;

                /* Log to prediction_accuracy */
                sqlite3_stmt *ins;
                const char *isql = "INSERT OR IGNORE INTO prediction_accuracy "
                    "(market_id, source, predicted_price, resolved_price, "
                    "accuracy, outcome, resolved_at, logged_at) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
                if (sqlite3_prepare_v2(db, isql, -1, &ins, NULL) == SQLITE_OK) {
                    char src[32];
                    snprintf(src, 32, "room_%d", room_id);
                    sqlite3_bind_text(ins, 1, mkt_id, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 2, src, -1, SQLITE_STATIC);
                    sqlite3_bind_double(ins, 3, pred_price);
                    sqlite3_bind_double(ins, 4, res_price);
                    sqlite3_bind_double(ins, 5, accuracy);
                    sqlite3_bind_int(ins, 6, outcome);
                    sqlite3_bind_int64(ins, 7, (long long)now);
                    sqlite3_bind_int64(ins, 8, (long long)now);
                    if (sqlite3_step(ins) == SQLITE_DONE) scored++;
                    sqlite3_finalize(ins);
                }
            }
            sqlite3_finalize(st);
        }
    }

    /* Report per-room accuracy */
    printf("[PRED_LOGGER] Logged %d predictions, scored %d\n", logged, scored);

    sqlite3_stmt *acc;
    const char *acc_sql = "SELECT source, COUNT(*), AVG(accuracy) "
                          "FROM prediction_accuracy WHERE source LIKE 'room_%' "
                          "GROUP BY source ORDER BY AVG(accuracy) DESC";
    if (sqlite3_prepare_v2(db, acc_sql, -1, &acc, NULL) == SQLITE_OK) {
        printf("\n  Room Accuracy Rankings:\n");
        while (sqlite3_step(acc) == SQLITE_ROW) {
            const char *src = (const char *)sqlite3_column_text(acc, 0);
            int cnt = sqlite3_column_int(acc, 1);
            double avg = sqlite3_column_double(acc, 2);
            printf("  %20s: %4d predictions, μ=%.4f\n", src ? src : "?", cnt, avg);
        }
        sqlite3_finalize(acc);
    }

    sqlite3_close(db);
    printf("[PRED_LOGGER] Done\n");
    return 0;
}
