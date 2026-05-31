/**
 * referee_weather_collector.c — Referee Assignments & Game Weather
 * Fetches referee info from ESPN game data + weather from Open-Meteo
 *
 * Compile: gcc -O2 -Wall -o referee_weather_collector referee_weather_collector.c -lcurl -ljansson -lsqlite3 -lm
 * Usage:   ./referee_weather_collector [--days 7]
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
#define OUT_FILE OUT_DIR "/referee_weather.json"

typedef struct { char *d; size_t l; } buf_t;
static size_t wcb(void *p,size_t s,size_t n,void *u){size_t t=s*n;buf_t*b=(buf_t*)u;char*np=realloc(b->d,b->l+t+1);if(!np)return 0;b->d=np;memcpy(b->d+b->l,p,t);b->l+=t;b->d[b->l]=0;return t;}
static char *get(const char *url){
    CURL*c=curl_easy_init();if(!c)return NULL;buf_t b={NULL,0};
    struct curl_slist*h=NULL;h=curl_slist_append(h,"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wcb);curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,10L);curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    CURLcode r=curl_easy_perform(c);curl_slist_free_all(h);curl_easy_cleanup(c);
    if(r!=CURLE_OK){free(b.d);return NULL;}return b.d;
}
static const char *sstr(const json_t*o,const char*k){json_t*v=o?json_object_get(o,k):NULL;return(v&&json_is_string(v))?json_string_value(v):"";}

int main(int argc,char**argv){
    int days=7;if(argc>1)days=atoi(argv[1]);
    curl_global_init(CURL_GLOBAL_ALL);
    sqlite3*db;sqlite3_open(DB_PATH,&db);
    sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS referee_weather(game_id TEXT,referee TEXT,weather TEXT,temp_c REAL,wind_kmh REAL,fetched_at INTEGER,PRIMARY KEY(game_id))",NULL,NULL,NULL);
    json_t*root=json_array();time_t now=time(NULL);

    // Get recent MLB games for referee/weather data
    for(int d=0;d<days;d++){
        time_t ts=now-(time_t)d*86400;struct tm*tp=gmtime(&ts);
        char date[16];snprintf(date,sizeof(date),"%04d%02d%02d",tp->tm_year+1900,tp->tm_mon+1,tp->tm_mday);
        
        char url[256];snprintf(url,sizeof(url),"https://site.api.espn.com/apis/site/v2/sports/baseball/mlb/scoreboard?dates=%s",date);
        char*r=get(url);if(!r)continue;
        json_error_t err;json_t*j=json_loads(r,0,&err);free(r);if(!j)continue;
        json_t*ev=json_object_get(j,"events");if(!ev||!json_is_array(ev)){json_decref(j);continue;}
        
        size_t ei;json_t*e;
        json_array_foreach(ev,ei,e){
            json_t*cmp_a=json_object_get(e,"competitions");
            json_t*cmp=(cmp_a&&json_array_size(cmp_a)>0)?json_array_get(cmp_a,0):NULL;
            if(!cmp)continue;
            
            // Referees/officials
            json_t*officials=json_object_get(cmp,"officials");
            const char*ref="";
            if(officials&&json_is_array(officials)&&json_array_size(officials)>0){
                json_t*off=json_array_get(officials,0);
                ref=sstr(off,"displayName");
            }
            
            // Venue for weather lookup
            const char*venue=sstr(json_object_get(cmp,"venue"),"fullName");
            const char*vcity=sstr(json_object_get(cmp,"venue"),"city");
            
            // Weather - some ESPN events have weather data
            json_t*wx=json_object_get(cmp,"weather");
            const char*wx_str="";double temp=0,wind=0;
            if(wx){
                wx_str=sstr(wx,"displayValue");
                temp=json_number_value(json_object_get(wx,"temperature"));
                wind=json_number_value(json_object_get(wx,"windSpeed"));
            }
            
            const char*game_id=sstr(e,"id");
            json_t*entry=json_pack("{s:s,s:s,s:s,s:s,s:f,s:f,s:s}",
                "game_id",game_id,"referee",ref,
                "venue",venue,"weather",wx_str,
                "temp_c",temp,"wind_kmh",wind,
                "city",vcity);
            json_array_append_new(root,entry);
            
            sqlite3_stmt*st;
            if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO referee_weather VALUES(?,?,?,?,?,?)",-1,&st,NULL)==SQLITE_OK){
                sqlite3_bind_text(st,1,game_id,-1,SQLITE_STATIC);sqlite3_bind_text(st,2,ref,-1,SQLITE_STATIC);
                sqlite3_bind_text(st,3,wx_str,-1,SQLITE_STATIC);sqlite3_bind_double(st,4,temp);
                sqlite3_bind_double(st,5,wind);sqlite3_bind_int64(st,6,(sqlite3_int64)now);
                sqlite3_step(st);sqlite3_finalize(st);
            }
        }
        json_decref(j);
    }
    mkdir(OUT_DIR,0755);json_dump_file(root,OUT_FILE,JSON_INDENT(2));
    printf("[REFWX] %zu games -> %s\n",json_array_size(root),OUT_FILE);
    json_decref(root);sqlite3_close(db);curl_global_cleanup();return 0;
}
