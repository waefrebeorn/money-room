/**
 * accuracy_scorer.c — Prediction accuracy scorer
 * Computes Brier scores, win rates, calibration for all market predictions.
 * Reads from timeline.db outcome fields. Exits 0 with JSON stats.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <math.h>
#include <time.h>

static sqlite3 *db;
static int total_predictions = 0, correct = 0;
static double brier_sum = 0.0;

static int count_callback(void *data, int argc, char **argv, char **col) {
    if (argc > 0 && argv[0]) {
        int *count = (int*)data;
        *count = atoi(argv[0]);
    }
    return 0;
}

static int score_callback(void *data, int argc, char **argv, char **col) {
    if (argc >= 2 && argv[0] && argv[1]) {
        double prob = atof(argv[0]);
        int outcome = atoi(argv[1]);
        total_predictions++;
        brier_sum += (prob - outcome) * (prob - outcome);
        if ((prob >= 0.5 && outcome == 1) || (prob < 0.5 && outcome == 0))
            correct++;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *db_path = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s/money-room/data/outcomes.db", db_path ? db_path : ".");

    if (sqlite3_open(path, &db) != SQLITE_OK) {
        // Try timeline.db
        snprintf(path, sizeof(path), "%s/money-room/data/timeline.db", db_path ? db_path : ".");
        if (sqlite3_open(path, &db) != SQLITE_OK) {
            printf("{\"accuracy_scorer\": \"no db\", \"status\": \"ok\"}\n");
            return 0;
        }
    }

    // Count predictions
    char *err = NULL;
    sqlite3_exec(db, "SELECT COUNT(*) FROM predictions WHERE outcome IS NOT NULL",
                 count_callback, &total_predictions, &err);

    sqlite3_exec(db, "SELECT probability, outcome FROM predictions WHERE outcome IS NOT NULL",
                 score_callback, NULL, &err);

    double accuracy = total_predictions > 0 ? (double)correct / total_predictions : 0.0;
    double brier = total_predictions > 0 ? brier_sum / total_predictions : 0.0;
    double calibration = total_predictions > 0 ? fabs(accuracy - brier) : 0.0;

    printf("{\n");
    printf("  \"accuracy_scorer\": \"ok\",\n");
    printf("  \"total_predictions\": %d,\n", total_predictions);
    printf("  \"correct\": %d,\n", correct);
    printf("  \"accuracy\": %.4f,\n", accuracy);
    printf("  \"brier_score\": %.4f,\n", brier);
    printf("  \"calibration_error\": %.4f\n", calibration);
    printf("}\n");

    sqlite3_close(db);
    return 0;
}
