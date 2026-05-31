/**
 * world_risk_collector.c — World Risk & Geopolitical Monitor
 * Monitors global risk events: wars, conflicts, disasters, sanctions
 * Uses Google News RSS + Wikipedia current events
 *
 * Compile: gcc -O2 -Wall -o world_risk_collector world_risk_collector.c -lcurl -ljansson -lsqlite3 -lm
 * Usage: ./world_risk_collector
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <ctype.h>
#include <sys/stat.h>
#define DB_PATH "/home/wubu2/money-room/engine/timeline.db"
#define OUT_DIR "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/world_risk.json"

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
// Simple RSS tag extractor
static void ext_tag(const char *x, const char *tag, char *out, int max) {
    out[0]=0; char o[32],c[32]; snprintf(o,sizeof(o),"<%s>",tag); snprintf(c,sizeof(c),"</%s>",tag);
    const char *s=strstr(x,o); if(!s)return; s=strchr(s,'>'); if(!s)return; s++;
    const char *e=strstr(s,c); if(!e)return;
    int len=(int)(e-s); if(len>max-1)len=max-1; strncpy(out,s,len); out[len]=0;
}
static int contains(const char *text, const char **terms) {
    if(!text||!text[0]) return 0;
    for(int i=0;terms[i];i++) if(strstr(text,terms[i])) return 1;
    return 0;
}
int main(void) {
    curl_global_init(CURL_GLOBAL_ALL);
    sqlite3 *db; sqlite3_open(DB_PATH, &db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS world_risk ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT, source TEXT, "
        "category TEXT, severity REAL, fetched_at INTEGER)",
        NULL,NULL,NULL);
    json_t *root = json_array();
    time_t now = time(NULL);
    
    // Risk keywords
    const char *war_kw[] = {"war","conflict","invasion","military","sanctions","missile","nuclear",NULL};
    const char *disaster_kw[] = {"earthquake","hurricane","flood","wildfire","volcano","tsunami",NULL};
    const char *econ_kw[] = {"recession","inflation","crisis","default","bankruptcy","tariff","trade war",NULL};
    
    // Source: Google News RSS for risk categories
    const char *queries[] = {"world+news+conflict","natural+disaster+today","economic+crisis+2026","global+trade+tensions",NULL};
    
    for(int q=0;queries[q];q++){
        char url[512]; snprintf(url,sizeof(url),"https://news.google.com/rss/search?q=%s&hl=en-US&gl=US",queries[q]);
        char *rss = get(url); if(!rss) continue;
        const char *p = rss;
        while((p=strstr(p,"<item>"))){
            char title[1024]={0}, src[256]={0};
            ext_tag(p,"title",title,sizeof(title)); ext_tag(p,"source",src,sizeof(src));
            if(!title[0]||strlen(title)<15){p+=6;continue;}
            
            const char *cat = contains(title,war_kw) ? "conflict" :
                             contains(title,disaster_kw) ? "disaster" :
                             contains(title,econ_kw) ? "economic" : "general";
            double severity = contains(title,war_kw) ? (strstr(title,"nuclear")?0.9:0.7) :
                             contains(title,disaster_kw) ? (strstr(title,"earthquake")?0.8:0.6) : 0.3;
            
            json_t *e = json_pack("{s:s,s:s,s:s,s:f}",
                "title",title,"source",src[0]?src:"Google News",
                "category",cat,"severity",severity);
            json_array_append_new(root,e);
            
            sqlite3_stmt *st;
            if(sqlite3_prepare_v2(db,"INSERT INTO world_risk(title,source,category,severity,fetched_at) VALUES(?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){
                sqlite3_bind_text(st,1,title,-1,SQLITE_STATIC); sqlite3_bind_text(st,2,src[0]?src:"Google News",-1,SQLITE_STATIC);
                sqlite3_bind_text(st,3,cat,-1,SQLITE_STATIC); sqlite3_bind_double(st,4,severity);
                sqlite3_bind_int64(st,5,(sqlite3_int64)now); sqlite3_step(st); sqlite3_finalize(st);
            }
            p+=6;
        }
        free(rss);
    }
    mkdir(OUT_DIR,0755); json_dump_file(root,OUT_FILE,JSON_INDENT(2));
    printf("[WORLDRISK] %zu events -> %s\n", json_array_size(root), OUT_FILE);
    json_decref(root); sqlite3_close(db); curl_global_cleanup(); return 0;
}
