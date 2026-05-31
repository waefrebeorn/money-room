/**
 * nfl_advanced_collector.c — NFL Advanced Stats (passing/rushing/defense splits)
 * Compile: gcc -O2 -Wall -o nfl_advanced_collector nfl_advanced_collector.c -lcurl -ljansson -lsqlite3 -lm
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
#define OUT_FILE OUT_DIR "/nfl_advanced.json"
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
int main(void) {
    curl_global_init(CURL_GLOBAL_ALL);
    sqlite3 *db; sqlite3_open(DB_PATH, &db);
    sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS nfl_advanced (team TEXT,stat_name TEXT,stat_value TEXT,fetched_at INTEGER,PRIMARY KEY(team,stat_name))",NULL,NULL,NULL);
    json_t *root = json_array();
    char *resp = get("https://site.api.espn.com/apis/site/v2/sports/football/nfl/teams");
    if (!resp) return 1;
    json_error_t err; json_t *j = json_loads(resp, 0, &err); free(resp);
    json_t *ta = json_object_get(json_array_get(json_object_get(json_array_get(json_object_get(j,"sports"),0),"leagues"),0),"teams");
    int ids[100], n=0; size_t ti; json_t *te;
    json_array_foreach(ta,ti,te) { int id = atoi(sstr(json_object_get(te,"team"),"id")); if(id>0&&n<100) ids[n++]=id; }
    json_decref(j); printf("[NFL] %d teams\n", n);
    for(int t=0;t<n;t++) {
        char url[256]; snprintf(url,sizeof(url),"https://site.api.espn.com/apis/site/v2/sports/football/nfl/teams/%d/statistics",ids[t]);
        char *tr = get(url); if(!tr) continue;
        json_t *tj = json_loads(tr,0,&err); free(tr); if(!tj) continue;
        const char *tn = sstr(json_object_get(tj,"team"),"displayName");
        if(!tn[0]){json_decref(tj);continue;}
        json_t *cats = json_object_get(json_object_get(json_object_get(tj,"results"),"stats"),"categories");
        if(!cats||!json_is_array(cats)){json_decref(tj);continue;}
        json_t *entry = json_object(); json_object_set_new(entry,"team",json_string(tn));
        size_t ci; json_t *cat;
        json_array_foreach(cats,ci,cat) {
            json_t *cs = json_object_get(cat,"stats"); if(!cs||!json_is_array(cs)) continue;
            size_t si; json_t *st;
            json_array_foreach(cs,si,st) {
                const char *nm = json_string_value(json_object_get(st,"displayName"));
                const char *vl = json_string_value(json_object_get(st,"displayValue"));
                if(!nm) nm = json_string_value(json_object_get(st,"name"));
                if(!nm||!vl) continue;
                json_object_set_new(entry,nm,json_string(vl));
                sqlite3_stmt *st2;
                if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO nfl_advanced VALUES(?,?,?,?)",-1,&st2,NULL)==SQLITE_OK){
                    sqlite3_bind_text(st2,1,tn,-1,SQLITE_STATIC); sqlite3_bind_text(st2,2,nm,-1,SQLITE_STATIC);
                    sqlite3_bind_text(st2,3,vl,-1,SQLITE_STATIC); sqlite3_bind_int64(st2,4,(sqlite3_int64)time(NULL));
                    sqlite3_step(st2); sqlite3_finalize(st2);
                }
            }
        }
        json_array_append_new(root,entry); printf("[NFL] %s OK\n", tn); json_decref(tj);
    }
    mkdir(OUT_DIR,0755); json_dump_file(root,OUT_FILE,JSON_INDENT(2));
    printf("[NFL] %zu teams -> %s\n",json_array_size(root),OUT_FILE);
    json_decref(root); sqlite3_close(db); curl_global_cleanup(); return 0;
}
