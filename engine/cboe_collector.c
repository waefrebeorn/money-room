/**
 * cboe_collector.c — CBOE VIX futures, SKEW, put/call, 0DTE (T1248-T1260)
 * Free CBOE data: VIX index, futures curve, options data.
 * 
 * Compile: gcc -O3 -Wall -Wextra -o cboe_collector cboe_collector.c -lcurl -lsqlite3 -lm -ljansson
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
static size_t wcb(void *p, size_t sz, size_t n, void *u) { MB *b=(MB*)u; size_t t=sz*n;
    char *nw=realloc(b->d,b->s+t+1); if(!nw) return 0; b->d=nw; memcpy(b->d+b->s,p,t); b->s+=t; b->d[b->s]=0; return t; }

static int http(const char *url, MB *b) {
    CURL *c=curl_easy_init(); if(!c) return -1; b->s=0;
    curl_easy_setopt(c,CURLOPT_URL,url); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wcb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,b); curl_easy_setopt(c,CURLOPT_TIMEOUT,15L);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"MoneyRoom/1.0"); curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    CURLcode r=curl_easy_perform(c); long h=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&h);
    curl_easy_cleanup(c); return (r==CURLE_OK&&h==200)?0:-1;
}

static sqlite3 *odb() {
    sqlite3 *db; if(sqlite3_open(DB_PATH,&db)!=SQLITE_OK) return NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS cboe_data ("
        "  series TEXT, obs_time INTEGER, value REAL, name TEXT,"
        "  PRIMARY KEY (series, obs_time)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_cboe ON cboe_data(series);",0,0,0);
    return db;
}

static void ins(sqlite3 *db, const char *s, time_t t, double v, const char *n) {
    sqlite3_stmt *st; sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO cboe_data (series,obs_time,value,name) VALUES(?1,?2,?3,?4)",-1,&st,0);
    sqlite3_bind_text(st,1,s,-1,SQLITE_STATIC); sqlite3_bind_int64(st,2,t);
    sqlite3_bind_double(st,3,v); sqlite3_bind_text(st,4,n,-1,SQLITE_STATIC);
    sqlite3_step(st); sqlite3_finalize(st);
}

/* VIX index from CBOE */
static int vix_index(sqlite3 *db) {
    MB b={0}; if(http("https://cdn.cboe.com/api/global/us_indices/dashboards/VIX_VIEW_DATA.json",&b)) return -1;
    json_t *j=json_loads(b.d,0,0); free(b.d); if(!j) return -1;
    json_t *d=json_object_get(j,"data");
    if(!d) { json_decref(j); return -1; }
    json_t *vi=json_object_get(d,"vixIndex"), *vo=json_object_get(d,"vixOpen"), *vh=json_object_get(d,"vixHigh");
    json_t *vl=json_object_get(d,"vixLow"), *vc=json_object_get(d,"vixClose");
    time_t t=time(NULL);
    printf("  VIX: idx="); json_t *v=NULL;
    if((v=vi)){printf("%s",json_string_value(v));ins(db,"vix_index",t,atof(json_string_value(v)),"VIX Index");}
    if((v=vo)){printf(" open=%s",json_string_value(v));ins(db,"vix_open",t,atof(json_string_value(v)),"VIX Open");}
    if((v=vh)){printf(" high=%s",json_string_value(v));ins(db,"vix_high",t,atof(json_string_value(v)),"VIX High");}
    if((v=vl)){printf(" low=%s",json_string_value(v));ins(db,"vix_low",t,atof(json_string_value(v)),"VIX Low");}
    if((v=vc)){printf(" close=%s",json_string_value(v));ins(db,"vix_close",t,atof(json_string_value(v)),"VIX Close");}
    printf("\n");
    json_decref(j); return 0;
}

/* CBOE SKEW index */
static int skew_index(sqlite3 *db) {
    MB b={0}; if(http("https://cdn.cboe.com/api/global/us_indices/dashboards/short_term_volatility_data.json",&b)) return -1;
    json_t *j=json_loads(b.d,0,0); free(b.d); if(!j) return -1;
    json_t *d=json_object_get(j,"data");
    if(!d){json_decref(j);return -1;}
    json_t *sk=json_object_get(d,"skewIndex");
    time_t t=time(NULL);
    if(sk) {printf("  SKEW: %s\n",json_string_value(sk));ins(db,"skew_index",t,atof(json_string_value(sk)),"CBOE SKEW Index");}
    json_decref(j); return 0;
}

/* CBOE equity put/call ratio */
static int put_call_ratio(sqlite3 *db) {
    MB b={0}; if(http("https://cdn.cboe.com/api/global/us_indices/dashboards/put_call_data.json",&b)) return -1;
    json_t *j=json_loads(b.d,0,0); free(b.d); if(!j) return -1;
    json_t *d=json_object_get(j,"data");
    if(!d){json_decref(j);return -1;}
    json_t *epc=json_object_get(d,"equityPCR"), *ipc=json_object_get(d,"indexPCR");
    json_t *epct=json_object_get(d,"equityPCTotal"), *tpc=json_object_get(d,"totalPCR");
    time_t t=time(NULL);
    if(epc) {printf("  Equity PCR: %s\n",json_string_value(epc));ins(db,"equity_pcr",t,atof(json_string_value(epc)),"Equity P/C Ratio");}
    if(ipc) {printf("  Index PCR: %s\n",json_string_value(ipc));ins(db,"index_pcr",t,atof(json_string_value(ipc)),"Index P/C Ratio");}
    if(tpc) {printf("  Total PCR: %s\n",json_string_value(tpc));ins(db,"total_pcr",t,atof(json_string_value(tpc)),"Total P/C Ratio");}
    json_decref(j); return 0;
}

/* 0DTE options volume (from CBOE) */
static int zero_dte(sqlite3 *db) {
    MB b={0}; if(http("https://cdn.cboe.com/api/global/us_indices/dashboards/zero_days_to_expiration.json",&b)) return -1;
    json_t *j=json_loads(b.d,0,0); free(b.d); if(!j) return -1;
    json_t *d=json_object_get(j,"data");
    if(!d){json_decref(j);return -1;}
    json_t *vol=json_object_get(d,"notionalVolume"), *contracts=json_object_get(d,"contracts");
    time_t t=time(NULL);
    if(vol) {printf("  0DTE Notional: %s\n",json_string_value(vol));ins(db,"odte_notional",t,atof(json_string_value(vol)),"0DTE Notional Volume");}
    if(contracts) {printf("  0DTE Contracts: %s\n",json_string_value(contracts));ins(db,"odte_contracts",t,atof(json_string_value(contracts)),"0DTE Contracts");}
    json_decref(j); return 0;
}

static void print_stats(sqlite3 *db) {
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,"SELECT series,COUNT(*),ROUND(AVG(value),2),MAX(obs_time),name FROM cboe_data GROUP BY series",-1,&s,0);
    printf("\n%-20s %8s %12s %24s %s\n","SERIES","ROWS","MEAN","LAST_UPDATED","NAME");
    printf("-------------------- -------- ------------ ------------------------ --------------------\n");
    int t=0;
    while(sqlite3_step(s)==SQLITE_ROW) {
        time_t ts=(time_t)sqlite3_column_int64(s,3);
        struct tm *tm=gmtime(&ts); char buf[32]; strftime(buf,32,"%Y-%m-%d %H:%M:%S",tm);
        printf("%-20s %8d %11.2f %24s %s\n",sqlite3_column_text(s,0),sqlite3_column_int(s,1),
               sqlite3_column_double(s,2),buf,sqlite3_column_text(s,4));
        t+=sqlite3_column_int(s,1);
    }
    sqlite3_finalize(s);
    printf("-------------------- --------\nTOTAL: %d rows\n",t);
}

int main(int argc, char **argv) {
    sqlite3 *db=odb(); if(!db) return 1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    if(argc>1 && strcmp(argv[1],"stats")==0) { print_stats(db); sqlite3_close(db); return 0; }
    
    printf("=== CBOE DATA COLLECTOR ===\n");
    int ok=0,fail=0;
    if(vix_index(db)==0) ok++;else fail++;
    if(skew_index(db)==0) ok++;else fail++;
    if(put_call_ratio(db)==0) ok++;else fail++;
    if(zero_dte(db)==0) ok++;else fail++;
    printf("OK: %d, FAIL: %d\n",ok,fail);
    
    sqlite3_close(db);
    curl_global_cleanup();
    return 0;
}
