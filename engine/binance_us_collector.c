#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#define DB "/home/wubu2/.hermes/pm_logs/timeline.db"
typedef struct{char*d;size_t l;}buf_t;
static size_t wcb(void*p,size_t s,size_t n,void*u){size_t t=s*n;buf_t*b=u;char*np=realloc(b->d,b->l+t+1);if(!np)return 0;b->d=np;memcpy(b->d+b->l,p,t);b->l+=t;b->d[b->l]=0;return t;}
static char*get(const char*u){CURL*c=curl_easy_init();if(!c)return NULL;buf_t b={0};curl_easy_setopt(c,CURLOPT_URL,u);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wcb);curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);curl_easy_setopt(c,CURLOPT_TIMEOUT,15L);CURLcode r=curl_easy_perform(c);curl_easy_cleanup(c);return r==CURLE_OK?b.d:(free(b.d),NULL);}
int main(void){
 sqlite3*db=0;sqlite3_open(DB,&db);
 sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS timeline(id INTEGER PRIMARY KEY AUTOINCREMENT,ts INTEGER NOT NULL,source TEXT NOT NULL,category TEXT NOT NULL,data TEXT NOT NULL,collected_at INTEGER DEFAULT(strftime('%s','now')))",0,0,0);
 sqlite3_exec(db,"CREATE INDEX IF NOT EXISTS i_ts ON timeline(ts)",0,0,0);
 const char*P[]={"BTCUSDT","ETHUSDT","SOLUSDT","XRPUSDT","ADAUSDT","DOGEUSDT","AVAXUSDT","DOTUSDT","LINKUSDT","TRXUSDT",0};
 int ins=0;time_t nw=time(0);time_t wh=nw-(nw%3600);
 for(int p=0;P[p];p++){
  char url[256];snprintf(url,sizeof(url),"https://api.binance.us/api/v3/ticker/24hr?symbol=%s",P[p]);
  char*b=get(url);if(!b){printf("[binus] %s: FAIL\n",P[p]);continue;}
  json_t*r=json_loads(b,0,0);free(b);if(!r){printf("[binus] %s: JSON fail\n",P[p]);continue;}
  json_t*jlc=json_object_get(r,"lastPrice");json_t*jv=json_object_get(r,"volume");
  json_t*jh=json_object_get(r,"highPrice");json_t*jl=json_object_get(r,"lowPrice");
  json_t*jp=json_object_get(r,"priceChangePercent");
  double lc=jlc?atof(json_string_value(jlc)):0,v=jv?atof(json_string_value(jv)):0;
  double hc=jh?atof(json_string_value(jh)):0,ll=jl?atof(json_string_value(jl)):0,pc=jp?atof(json_string_value(jp)):0;
  char src[64];snprintf(src,sizeof(src),"binance_us_%s",P[p]);
  sqlite3_stmt*st=0;
  if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO timeline(ts,source,category,data,collected_at)VALUES(?1,?2,'crypto_spot',?3,strftime('%s','now'))",-1,&st,0)==SQLITE_OK){
   char js[512];snprintf(js,sizeof(js),"{\"last\":%s,\"vol\":%s,\"high\":%s,\"low\":%s,\"chg\":%s}",jlc?json_string_value(jlc):"0",jv?json_string_value(jv):"0",jh?json_string_value(jh):"0",jl?json_string_value(jl):"0",jp?json_string_value(jp):"0");
   sqlite3_bind_int64(st,1,wh);sqlite3_bind_text(st,2,src,-1,SQLITE_STATIC);sqlite3_bind_text(st,3,js,-1,SQLITE_STATIC);
   if(sqlite3_step(st)==SQLITE_DONE)ins++;sqlite3_finalize(st);}
  json_decref(r);printf("[binus] %s: $%s\n",P[p],jlc?json_string_value(jlc):"0");}
 printf("[binus] Done. %d rows\n",ins);sqlite3_close(db);return 0;}
