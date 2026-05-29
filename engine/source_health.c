/**
 * source_health.c — T125: Per-Source Freshness Dashboard
 *
 * Scans timeline.db for all unique data sources and reports:
 *   - Last collected timestamp
 *   - Age in minutes
 *   - Staleness status
 *   - Record count
 *   - Data quality flags
 *
 * Output: JSON to stdout (consumed by stale_alerter and website)
 *
 * Build: gcc -O2 -o source_health source_health.c -lsqlite3 -ljansson
 * Usage: ./source_health
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <jansson.h>
#include <fcntl.h>

#define TL_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define OUTPUT_PATH "/home/wubu2/.hermes/vp_cache/source_health.json"
#define MAX_SOURCES 200

/* Thresholds (minutes) */
#define STALE_MIN 360     /* 6 hours = stale */
#define WARN_MIN  120     /* 2 hours = warn */
#define FAIL_MIN  1440    /* 24 hours = fail */

typedef struct {
    char name[128];
    char category[64];
    long long last_ts;
    long long earliest_ts;
    int record_count;
    int age_min;
    int status;         /* 0=ok, 1=warn, 2=stale, 3=fail, 4=missing */
    double last_value;  /* from numeric data if available */
} SourceHealth;

int main(void) {
    sqlite3 *db;
    if (sqlite3_open(TL_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "{\"error\":\"cannot open %s\"}\n", TL_PATH);
        return 1;
    }

    SourceHealth sources[MAX_SOURCES];
    int n_sources = 0;

    /* Query all distinct sources with their metrics */
    sqlite3_stmt *st;
    const char *sql = "SELECT source, category, "
                      "MAX(ts) as last_ts, MIN(ts) as first_ts, "
                      "COUNT(*) as count "
                      "FROM timeline GROUP BY source ORDER BY source";

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "{\"error\":\"query failed\"}\n");
        sqlite3_close(db);
        return 1;
    }

    long long now = (long long)time(NULL);
    int total_stale = 0, total_fail = 0, total_warn = 0, total_ok = 0;
    int total_missing = 0;

    while (sqlite3_step(st) == SQLITE_ROW && n_sources < MAX_SOURCES) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *cat = (const char *)sqlite3_column_text(st, 1);
        long long last_ts = sqlite3_column_int64(st, 2);
        long long first_ts = sqlite3_column_int64(st, 3);
        int count = sqlite3_column_int(st, 4);

        if (!name) continue;

        SourceHealth *s = &sources[n_sources];
        strncpy(s->name, name, 127);
        s->name[127] = 0;
        strncpy(s->category, cat ? cat : "unknown", 63);
        s->category[63] = 0;
        s->last_ts = last_ts;
        s->earliest_ts = first_ts;
        s->record_count = count;

        int age_min = (int)((now - last_ts) / 60);
        s->age_min = age_min < 0 ? 0 : age_min;

        if (count == 0 || last_ts == 0) {
            s->status = 4;  /* missing */
            total_missing++;
        } else if (age_min >= FAIL_MIN) {
            s->status = 3;  /* fail */
            total_fail++;
        } else if (age_min >= STALE_MIN) {
            s->status = 2;  /* stale */
            total_stale++;
        } else if (age_min >= WARN_MIN) {
            s->status = 1;  /* warn */
            total_warn++;
        } else {
            s->status = 0;  /* ok */
            total_ok++;
        }

        s->last_value = 0;
        n_sources++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    /* Build JSON output */
    json_t *root = json_object();
    json_t *src_arr = json_array();

    for (int i = 0; i < n_sources; i++) {
        SourceHealth *s = &sources[i];
        json_t *j = json_object();
        json_object_set_new(j, "name", json_string(s->name));
        json_object_set_new(j, "category", json_string(s->category));
        json_object_set_new(j, "last_ts", json_integer(s->last_ts));
        json_object_set_new(j, "earliest_ts", json_integer(s->earliest_ts));
        json_object_set_new(j, "record_count", json_integer(s->record_count));
        json_object_set_new(j, "age_min", json_integer(s->age_min));

        const char *status_str = "ok";
        switch (s->status) {
            case 1: status_str = "warn"; break;
            case 2: status_str = "stale"; break;
            case 3: status_str = "fail"; break;
            case 4: status_str = "missing"; break;
            default: status_str = "ok";
        }
        json_object_set_new(j, "status", json_string(status_str));
        json_object_set_new(j, "stale", json_integer(s->status >= 2 ? 1 : 0));
        json_object_set_new(j, "last_value", json_real(s->last_value));

        /* Human-readable age */
        char age_str[64];
        if (s->age_min < 1) snprintf(age_str, 64, "now");
        else if (s->age_min < 60) snprintf(age_str, 64, "%dm", s->age_min);
        else if (s->age_min < 1440) snprintf(age_str, 64, "%dh%dm", s->age_min/60, s->age_min%60);
        else snprintf(age_str, 64, "%dd%dh", s->age_min/1440, (s->age_min%1440)/60);
        json_object_set_new(j, "age_str", json_string(age_str));

        json_array_append_new(src_arr, j);
    }

    json_object_set_new(root, "sources", src_arr);
    json_object_set_new(root, "total_sources", json_integer(n_sources));
    json_object_set_new(root, "total_ok", json_integer(total_ok));
    json_object_set_new(root, "total_warn", json_integer(total_warn));
    json_object_set_new(root, "total_stale", json_integer(total_stale));
    json_object_set_new(root, "total_fail", json_integer(total_fail));
    json_object_set_new(root, "total_missing", json_integer(total_missing));

    char tb[64]; time_t now_t = time(NULL);
    strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now_t));
    json_object_set_new(root, "fetched_at", json_string(tb));

    /* Summary line for human reading */
    char summary[256];
    int total_issues = total_stale + total_fail + total_missing;
    snprintf(summary, 256, "[HEALTH] %d sources: %d OK, %d warn, %d stale, %d fail, %d missing",
             n_sources, total_ok, total_warn, total_stale, total_fail, total_missing);

    /* Write output */
    json_dumpfd(root, open(OUTPUT_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0644), JSON_INDENT(2));
    json_decref(root);

    /* Print summary and JSON to stdout for stale_alerter */
    printf("%s\n", summary);
    printf("{\"summary\":\"%s\",\"issues\":%d}\n", summary, total_issues);

    return total_issues > 0 ? 1 : 0;
}
