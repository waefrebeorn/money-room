/**
 * data_quality_monitor.c — Data Quality Monitor for Sports Pipeline
 * Verifies all data sources and collectors are producing fresh data
 *
 * Compile: gcc -O2 -Wall -o data_quality_monitor data_quality_monitor.c -lsqlite3 -ljansson -lm
 * Usage: ./data_quality_monitor
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include <jansson.h>
#include <sys/stat.h>

#define OC_PATH "/home/wubu2/.hermes/pm_logs/outcomes.db"
#define TL_PATH "/home/wubu2/money-room/engine/timeline.db"
#define OUT_DIR "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/data_quality.json"

int main(void) {
    json_t *checks = json_array();
    time_t now = time(NULL);
    int errors = 0, warnings = 0;
    
    // Check 1: Sports outcomes DB
    sqlite3 *db;
    if(sqlite3_open(OC_PATH, &db) == SQLITE_OK) {
        sqlite3_stmt *st;
        int total = 0, recent = 0;
        if(sqlite3_prepare_v2(db,"SELECT COUNT(*), MAX(collected_at) FROM sports_outcomes",-1,&st,NULL)==SQLITE_OK){
            if(sqlite3_step(st)==SQLITE_ROW){
                total = sqlite3_column_int(st,0);
                recent = sqlite3_column_int(st,1);
            }
            sqlite3_finalize(st);
        }
        json_t *c = json_pack("{s:s, s:s, s:i, s:i, s:s}",
            "check", "sports_outcomes_db",
            "status", total > 1000 ? "pass" : "warn",
            "total_games", total,
            "last_update_hours", recent ? (int)((now - recent)/3600) : -1,
            "detail", total > 1000 ? "Good data coverage" : "Low data volume");
        json_array_append_new(checks, c);
        printf("[DQ] sports_outcomes: %d games, last updated %dh ago\n", total, recent ? (int)((now - recent)/3600) : -1);
        if(total < 100) { errors++; printf("  ERROR: Low data volume\n"); }
        else if(total < 1000) { warnings++; printf("  WARN: Low data volume\n"); }
        sqlite3_close(db);
    }
    
    // Check 2: Timeline DB sports tables
    if(sqlite3_open(TL_PATH, &db) == SQLITE_OK) {
        const char *tables[] = {"sports_data", "sports_news", "injuries", "nba_advanced", "mlb_advanced", "nfl_advanced", "soccer_advanced"};
        int nt = 7;
        for(int i=0; i<nt; i++) {
            sqlite3_stmt *st;
            int exists = 0;
            char q[256];
            snprintf(q,sizeof(q),"SELECT COUNT(*) FROM %s", tables[i]);
            if(sqlite3_prepare_v2(db,q,-1,&st,NULL)==SQLITE_OK){
                if(sqlite3_step(st)==SQLITE_ROW) exists = sqlite3_column_int(st,0) > 0;
                sqlite3_finalize(st);
            }
            json_t *c = json_pack("{s:s, s:s, s:i, s:s}",
                "check", tables[i],
                "status", exists ? "pass" : "warn",
                "has_data", exists ? 1 : 0,
                "detail", exists ? "Table populated" : "Table empty or missing");
            json_array_append_new(checks, c);
            printf("[DQ] %s: %s\n", tables[i], exists ? "OK" : "EMPTY");
            if(!exists) warnings++;
        }
        sqlite3_close(db);
    }
    
    // Check 3: File freshness
    const char *files[] = {"sports_data.json","team_stats.json","injuries.json","head2head.json",
                           "sports_news.json","sports_training_features.json","schedule.json",
                           "game_timing.json","nba_advanced.json","mlb_advanced.json","nfl_advanced.json"};
    int nf = 11;
    for(int i=0; i<nf; i++) {
        char path[512];
        snprintf(path,sizeof(path),"%s/%s", OUT_DIR, files[i]);
        struct stat st;
        int exists = stat(path, &st) == 0;
        int hours = exists ? (int)((now - st.st_mtime)/3600) : -1;
        json_t *c = json_pack("{s:s, s:s, s:i, s:i, s:s}",
            "check", files[i],
            "status", exists ? (hours < 48 ? "pass" : "stale") : "missing",
            "exists", exists ? 1 : 0,
            "age_hours", hours,
            "detail", exists ? (hours < 48 ? "Fresh" : "Stale") : "Missing");
        json_array_append_new(checks, c);
        printf("[DQ] %s: %s (%dh old)\n", files[i], exists ? "OK" : "MISSING", hours);
        if(!exists) { errors++; }
        else if(hours > 48) { warnings++; }
    }
    
    json_t *report = json_pack("{s:i, s:i, s:i, s:o}",
        "total_checks", (int)json_array_size(checks),
        "errors", errors,
        "warnings", warnings,
        "checks", checks);
    
    mkdir(OUT_DIR,0755);
    json_dump_file(report, OUT_FILE, JSON_INDENT(2));
    printf("[DQ] Report: %zu checks, %d errors, %d warnings -> %s\n",
           json_array_size(checks), errors, warnings, OUT_FILE);
    
    json_decref(report);
    return errors > 0 ? 1 : 0;
}
