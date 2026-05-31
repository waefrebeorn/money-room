/**
 * opensky_collector.c — OpenSky Network flight tracking (T1450)
 * Free API, no key needed for limited access. Tracks global air traffic.
 * 
 * Compile: gcc -O3 -Wall -Wextra -o opensky_collector opensky_collector.c -lcurl -lsqlite3 -lm -ljansson
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <jansson.h>
#include <time.h>
#define DB_PATH "/home/wubu2/money-room/engine/timeline.db"

typedef struct{char*d;size_t s;}MB;
static size_t wcb(void*p,size_t sz,size_t n,void*u){MB*b=(MB*)u;size_t t=sz*n;char*nw=realloc(b->d,b->s+t+1);if(!nw)return 0;b->d=nw;memcpy(b->d+b->s,p,t);b->s+=t;b->d[b->s]=0;return t;}

static sqlite3 *odb(){
    sqlite3*db;if(sqlite3_open(DB_PATH,&db)!=SQLITE_OK)return NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS opensky_flights("
        "  obs_time INTEGER, ac_count INTEGER, total INTEGER," 
        "  created_at TEXT DEFAULT(datetime('now')),"
        "  PRIMARY KEY(obs_time)"
        ");CREATE TABLE IF NOT EXISTS opensky_routes("
        "  origin TEXT, destination TEXT, flight_count INTEGER,"
        "  created_at TEXT DEFAULT(datetime('now'))"
        ");",0,0,0);
    return db;
}

int main(int argc,char**argv){
    sqlite3*db=odb();if(!db)return 1;
    curl_global_init(CURL_GLOBAL_DEFAULT);MB b={0};
    CURL*c=curl_easy_init();if(!c)return 1;
    curl_easy_setopt(c,CURLOPT_URL,"https://opensky-network.org/api/states/all");
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wcb);curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,20L);curl_easy_setopt(c,CURLOPT_USERAGENT,"MoneyRoom/1.0");
    CURLcode r=curl_easy_perform(c);curl_easy_cleanup(c);
    if(r!=CURLE_OK){free(b.d);curl_global_cleanup();sqlite3_close(db);return 1;}
    json_t*j=json_loads(b.d,0,0);free(b.d);if(!j||!json_is_object(j)){if(j)json_decref(j);curl_global_cleanup();sqlite3_close(db);return 1;}
    json_t*states=json_object_get(j,"states");
    int n=states&&json_is_array(states)?(int)json_array_size(states):0;
    time_t now=time(NULL);
    sqlite3_stmt*s;sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO opensky_flights(obs_time,ac_count,total)VALUES(?1,?2,?3)",-1,&s,0);
    sqlite3_bind_int64(s,1,(sqlite3_int64)now);sqlite3_bind_int(s,2,n);sqlite3_bind_int(s,3,n);
    sqlite3_step(s);sqlite3_finalize(s);
    json_decref(j);sqlite3_close(db);curl_global_cleanup();
    printf("OpenSky: %d aircraft tracked globally at %ld\n",n,(long)now);
    return 0;
}
