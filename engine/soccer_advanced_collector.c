/**
 * soccer_advanced_collector.c — Soccer Advanced Stats
 * Extracts team records and statistics from ESPN soccer endpoints
 * Soccer API structure differs from US sports — stats in team.recordSummary
 *
 * Compile: gcc -O2 -Wall -o soccer_advanced_collector soccer_advanced_collector.c -lcurl -ljansson -lsqlite3 -lm
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
#define OUT_DIR  "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/soccer_advanced.json"
#define DB_PATH  "/home/wubu2/money-room/engine/timeline.db"
typedef struct { char *d; size_t l; } buf_t;
static size_t wcb(void *p, size_t s, size_t n, void *u) {
    size_t t = s*n; buf_t *b = (buf_t*)u;
    char *np = realloc(b->d, b->l + t + 1);
    if (!np) return 0; b->d = np;
    memcpy(b->d + b->l, p, t); b->l += t; b->d[b->l] = 0; return t;
}
static char *get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    buf_t b = {NULL,0};
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    curl_easy_setopt(c, CURLOPT_URL, url); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb); curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L); curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c); curl_slist_free_all(h); curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(b.d); return NULL; } return b.d;
}
static const char *sstr(const json_t *o, const char *k) {
    json_t *v = o ? json_object_get(o, k) : NULL;
    return (v && json_is_string(v)) ? json_string_value(v) : "";
}

typedef struct { const char *name, *sport, *league; } Lg;
static Lg LEAGUES[] = {{"EPL","soccer","eng.1"},{"LaLiga","soccer","esp.1"},{"SerieA","soccer","ita.1"},{"MLS","soccer","usa.1"}};
#define NL 4

int main(void) {
    curl_global_init(CURL_GLOBAL_ALL);
    sqlite3 *db; sqlite3_open(DB_PATH, &db);
    sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS soccer_advanced (league TEXT,team TEXT,stat_name TEXT,stat_value TEXT,fetched_at INTEGER,PRIMARY KEY(league,team,stat_name))",NULL,NULL,NULL);
    json_t *root = json_array();

    for(int l=0;l<NL;l++) {
        char url[256]; snprintf(url,sizeof(url),"https://site.api.espn.com/apis/site/v2/sports/%s/%s/teams",LEAGUES[l].sport,LEAGUES[l].league);
        char *resp = get(url); if(!resp) continue;
        json_error_t err; json_t *j = json_loads(resp,0,&err); free(resp); if(!j) continue;
        json_t *ta = json_object_get(json_array_get(json_object_get(json_array_get(json_object_get(j,"sports"),0),"leagues"),0),"teams");
        
        size_t ti; json_t *te;
        json_array_foreach(ta,ti,te) {
            json_t *team = json_object_get(te,"team");
            const char *tn = sstr(team,"displayName");
            const char *rec = sstr(team,"recordSummary");
            const char *stand = sstr(team,"standingSummary");
            if(!tn[0]) continue;

            json_t *entry = json_object(); 
            json_object_set_new(entry,"team",json_string(tn));
            json_object_set_new(entry,"league",json_string(LEAGUES[l].name));
            
            // Parse recordSummary like "20-11-7" (wins-draws-losses)
            if(rec[0]) {
                json_object_set_new(entry,"record",json_string(rec));
                int w=0,d=0,ls=0;
                sscanf(rec,"%d-%d-%d",&w,&d,&ls);
                char gs[32]; snprintf(gs,sizeof(gs),"%d",w+d+ls);
                json_object_set_new(entry,"played",json_string(gs));
                json_object_set_new(entry,"wins",json_integer(w));
                json_object_set_new(entry,"draws",json_integer(d));
                json_object_set_new(entry,"losses",json_integer(ls));
                
                sqlite3_stmt *st;
                char vals[5][32];
                snprintf(vals[0],sizeof(vals[0]),"%d",w);
                snprintf(vals[1],sizeof(vals[1]),"%d",d);
                snprintf(vals[2],sizeof(vals[2]),"%d",ls);
                snprintf(vals[3],sizeof(vals[3]),"%d",w+d+ls);
                const char *sn[] = {"wins","draws","losses","played","record"};
                const char *sv[] = {vals[0],vals[1],vals[2],vals[3],rec};
                for(int i=0;i<5;i++){
                    if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO soccer_advanced VALUES(?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){
                        sqlite3_bind_text(st,1,LEAGUES[l].name,-1,SQLITE_STATIC);
                        sqlite3_bind_text(st,2,tn,-1,SQLITE_STATIC);
                        sqlite3_bind_text(st,3,sn[i],-1,SQLITE_STATIC);
                        sqlite3_bind_text(st,4,sv[i],-1,SQLITE_STATIC);
                        sqlite3_bind_int64(st,5,(sqlite3_int64)time(NULL));
                        sqlite3_step(st); sqlite3_finalize(st);
                    }
                }
            }
            if(stand[0]) {
                json_object_set_new(entry,"standing",json_string(stand));
                sqlite3_stmt *st;
                if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO soccer_advanced VALUES(?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){
                    sqlite3_bind_text(st,1,LEAGUES[l].name,-1,SQLITE_STATIC);
                    sqlite3_bind_text(st,2,tn,-1,SQLITE_STATIC);
                    sqlite3_bind_text(st,3,"standing",-1,SQLITE_STATIC);
                    sqlite3_bind_text(st,4,stand,-1,SQLITE_STATIC);
                    sqlite3_bind_int64(st,5,(sqlite3_int64)time(NULL));
                    sqlite3_step(st); sqlite3_finalize(st);
                }
            }
            json_array_append_new(root,entry);
        }
        json_decref(j);
        printf("[SOCCER] %s: %zu teams\n", LEAGUES[l].name, json_array_size(root));
    }

    mkdir(OUT_DIR,0755); json_dump_file(root,OUT_FILE,JSON_INDENT(2));
    printf("[SOCCER] Total: %zu teams -> %s\n", json_array_size(root), OUT_FILE);
    json_decref(root); sqlite3_close(db); curl_global_cleanup(); return 0;
}
