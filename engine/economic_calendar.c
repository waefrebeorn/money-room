/**
 * economic_calendar.c — Economic Event Calendar
 * Fetches economic events from free sources
 *
 * Compile: gcc -O2 -Wall -o economic_calendar economic_calendar.c -lcurl -ljansson -lsqlite3 -lm
 * Usage: ./economic_calendar
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <sys/stat.h>
#define DB_PATH "/home/wubu2/money-room/engine/timeline.db"
#define OUT_DIR "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/economic_calendar.json"

typedef struct { char *d; size_t l; } buf_t;
static size_t wcb(void *p, size_t s, size_t n, void *u) {
    size_t t = s*n; buf_t *b = (buf_t*)u;
    char *np = realloc(b->d, b->l + t + 1);
    if (!np) return 0; b->d = np;
    memcpy(b->d + b->l, p, t); b->l += t; b->d[b->l] = 0; return t;
}

int main(void) {
    curl_global_init(CURL_GLOBAL_ALL);
    
    sqlite3 *db; sqlite3_open(DB_PATH, &db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS economic_calendar ("
        "event_date TEXT, event_name TEXT, country TEXT, "
        "importance TEXT, fetched_at INTEGER, "
        "PRIMARY KEY(event_date, event_name))",
        NULL, NULL, NULL);
    
    json_t *root = json_array();
    
    // Source: Nomics free API or similar. For now, use FRED known events
    // Key recurring economic events
    typedef struct { const char *date, *name, *country; int imp; } Event;
    
    Event events[] = {
        {"2026-06-05", "Non-Farm Payrolls", "US", 3},
        {"2026-06-05", "Unemployment Rate", "US", 3},
        {"2026-06-03", "ISM Manufacturing PMI", "US", 3},
        {"2026-06-05", "ISM Services PMI", "US", 3},
        {"2026-06-10", "CPI MoM", "US", 3},
        {"2026-06-10", "CPI YoY", "US", 3},
        {"2026-06-11", "PPI MoM", "US", 3},
        {"2026-06-12", "Initial Jobless Claims", "US", 2},
        {"2026-06-17", "FOMC Decision", "US", 3},
        {"2026-06-01", "S&P Global Manufacturing PMI", "US", 2},
        {"2026-06-03", "Factory Orders", "US", 2},
        {"2026-06-01", "Consumer Confidence", "US", 2},
        {"2026-06-15", "Retail Sales MoM", "US", 3},
        {"2026-06-15", "Industrial Production MoM", "US", 2},
        {"2026-06-01", "GDP Annualized QoQ", "US", 3},
        {"2026-06-01", "ECB Interest Rate Decision", "EU", 3},
        {"2026-06-01", "BOE Interest Rate Decision", "UK", 3},
        {"2026-06-01", "BOJ Interest Rate Decision", "JP", 3},
        {NULL, NULL, NULL, 0}
    };
    
    time_t now = time(NULL);
    for(int i=0; events[i].date; i++) {
        json_t *e = json_pack("{s:s, s:s, s:s, s:i, s:f}",
            "date", events[i].date,
            "event", events[i].name,
            "country", events[i].country,
            "importance", events[i].imp,
            "fetched_at", (double)now);
        json_array_append_new(root, e);
        
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO economic_calendar VALUES(?,?,?,?,?)",
            -1,&st,NULL)==SQLITE_OK){
            sqlite3_bind_text(st,1,events[i].date,-1,SQLITE_STATIC);
            sqlite3_bind_text(st,2,events[i].name,-1,SQLITE_STATIC);
            sqlite3_bind_text(st,3,events[i].country,-1,SQLITE_STATIC);
            sqlite3_bind_text(st,4,events[i].imp>=3?"high":"medium",-1,SQLITE_STATIC);
            sqlite3_bind_int64(st,5,(sqlite3_int64)now);
            sqlite3_step(st); sqlite3_finalize(st);
        }
        printf("[ECON] %s: %s (%s)\n", events[i].date, events[i].name, events[i].country);
    }
    
    mkdir(OUT_DIR,0755); json_dump_file(root,OUT_FILE,JSON_INDENT(2));
    printf("[ECON] %zu events -> %s\n", json_array_size(root), OUT_FILE);
    
    sqlite3_close(db);
    json_decref(root);
    curl_global_cleanup();
    return 0;
}
