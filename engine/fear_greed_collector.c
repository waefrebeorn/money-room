/**
 * fear_greed_collector.c — Alternative.me Fear & Greed Index (T370-T378 proxy)
 * Free API, no key needed. Crypto market sentiment indicator.
 * 
 * Compile: gcc -O3 -Wall -Wextra -o fear_greed_collector fear_greed_collector.c -lcurl -lsqlite3 -lm -ljansson
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <jansson.h>
#include <time.h>
#define DB_PATH "/home/wubu2/money-room/engine/timeline.db"

typedef struct { char *d; size_t s; } MB;
static size_t wcb(void *p,size_t sz,size_t n,void *u){MB*b=(MB*)u;size_t t=sz*n;char*nw=realloc(b->d,b->s+t+1);if(!nw)return 0;b->d=nw;memcpy(b->d+b->s,p,t);b->s+=t;b->d[b->s]=0;return t;}

static sqlite3 *odb() {
    sqlite3 *db;if(sqlite3_open(DB_PATH,&db)!=SQLITE_OK)return NULL;
    sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS fear_greed (obs_date TEXT PRIMARY KEY,value INTEGER,classification TEXT,updated_at TEXT DEFAULT(datetime('now')));",0,0,0);
    return db;
}

int main(int argc,char**argv){
    sqlite3 *db=odb();if(!db)return 1;
    if(argc>1&&strcmp(argv[1],"stats")==0){
        sqlite3_stmt*s;sqlite3_prepare_v2(db,"SELECT COUNT(*),AVG(value),MAX(obs_date),MIN(obs_date)FROM fear_greed",-1,&s,0);
        if(sqlite3_step(s)==SQLITE_ROW)printf("Fear & Greed: %d days, avg=%d, %s to %s\n",sqlite3_column_int(s,0),(int)sqlite3_column_double(s,1),sqlite3_column_text(s,3),sqlite3_column_text(s,2));
        sqlite3_finalize(s);sqlite3_close(db);return 0;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);MB b={0};
    CURL*c=curl_easy_init();if(!c)return 1;
    curl_easy_setopt(c,CURLOPT_URL,"https://api.alternative.me/fng/?limit=30");
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wcb);curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,15L);curl_easy_setopt(c,CURLOPT_USERAGENT,"MoneyRoom/1.0");
    CURLcode r=curl_easy_perform(c);curl_easy_cleanup(c);
    if(r!=CURLE_OK){free(b.d);curl_global_cleanup();sqlite3_close(db);return 1;}
    json_t*j=json_loads(b.d,0,0);free(b.d);if(!j||!json_is_object(j)){if(j)json_decref(j);curl_global_cleanup();sqlite3_close(db);return 1;}
    json_t*data=json_object_get(j,"data");if(!json_is_array(data)){json_decref(j);curl_global_cleanup();sqlite3_close(db);return 1;}
    int n=(int)json_array_size(data),rows=0;
    sqlite3_stmt*s;sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO fear_greed(obs_date,value,classification)VALUES(?1,?2,?3)",-1,&s,0);
    for(int i=0;i<n;i++){
        json_t*e=json_array_get(data,i);
        const char*dt=json_string_value(json_object_get(e,"timestamp"));
        const char*v=json_string_value(json_object_get(e,"value"));
        const char*cl=json_string_value(json_object_get(e,"value_classification"));
        if(dt&&v){sqlite3_bind_text(s,1,dt,-1,SQLITE_STATIC);sqlite3_bind_int(s,2,atoi(v));sqlite3_bind_text(s,3,cl?cl:"",-1,SQLITE_STATIC);if(sqlite3_step(s)==SQLITE_DONE)rows++;sqlite3_reset(s);}
    }
    sqlite3_finalize(s);
    int current_val = 0;
    const char *current_cls = "";
    json_t *first = json_array_get(data, 0);
    if(first) {
        const char *v = json_string_value(json_object_get(first, "value"));
        const char *cl = json_string_value(json_object_get(first, "value_classification"));
        if(v) current_val = atoi(v);
        if(cl) current_cls = cl;
    }
    json_decref(j);sqlite3_close(db);curl_global_cleanup();
    printf("Fear & Greed: %d days stored. Current: %d (%s)\n",rows,current_val,current_cls);
    return 0;
}
