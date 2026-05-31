/**
 * world_bank_collector.c — World Bank data API collector (T829-T830)
 * Free API, no key needed. Covers GDP, population, GINI, inflation, trade.
 * 
 * Compile: gcc -O3 -Wall -Wextra -o world_bank_collector world_bank_collector.c -lcurl -lsqlite3 -lm -ljansson
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <jansson.h>
#include <time.h>

#define DB_PATH "/home/wubu2/money-room/engine/timeline.db"
#define API "https://api.worldbank.org/v2/country"

typedef struct { char *d; size_t s; } MB;
static size_t wcb(void *p, size_t sz, size_t n, void *u) {
    MB *b=(MB*)u; size_t t=sz*n; char *nw=realloc(b->d,b->s+t+1);
    if(!nw) return 0; b->d=nw; memcpy(b->d+b->s,p,t); b->s+=t; b->d[b->s]=0; return t;
}

static sqlite3 *odb() {
    sqlite3 *db; if(sqlite3_open(DB_PATH,&db)!=SQLITE_OK) return NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS world_bank ("
        "  indicator TEXT, country TEXT, obs_year INTEGER, value REAL,"
        "  name TEXT, updated_at TEXT DEFAULT (datetime('now')),"
        "  PRIMARY KEY (indicator, country, obs_year)"
        ");", 0, 0, 0);
    return db;
}

static void ins(sqlite3 *db, const char *ind, const char *ctry, int yr, double v, const char *n) {
    sqlite3_stmt *s; sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO world_bank (indicator,country,obs_year,value,name) VALUES(?1,?2,?3,?4,?5)",-1,&s,0);
    sqlite3_bind_text(s,1,ind,-1,SQLITE_STATIC); sqlite3_bind_text(s,2,ctry,-1,SQLITE_STATIC);
    sqlite3_bind_int(s,3,yr); sqlite3_bind_double(s,4,v); sqlite3_bind_text(s,5,n,-1,SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);
}

typedef struct { const char *code; const char *name; } Indicator;
Indicator INDS[] = {
    {"NY.GDP.MKTP.CD", "GDP (current US$)"},
    {"NY.GDP.PCAP.CD", "GDP per capita (current US$)"},
    {"SP.POP.TOTL", "Population total"},
    {"SI.POV.GINI", "GINI Index"},
    {"FP.CPI.TOTL.ZG", "CPI inflation (%)"},
    {"NE.EXP.GNFS.CD", "Exports of goods/services"},
    {"NE.IMP.GNFS.CD", "Imports of goods/services"},
    {"NY.GDP.MKTP.KD.ZG", "GDP growth (annual %)"},
    {"SL.UEM.TOTL.ZS", "Unemployment (% of labor force)"},
    {"BN.CAB.XOKA.CD", "Current account balance (US$)"},
    {"DT.DOD.DECT.CD", "External debt (current US$)"},
    {"SE.XPD.TOTL.GD.ZS", "Education spending (% GDP)"},
    {"SH.XPD.CHEX.GD.ZS", "Health spending (% GDP)"},
    {"BX.KLT.DINV.WD.GD.ZS", "Foreign direct investment (% GDP)"},
    {"GC.DOD.TOTL.GD.ZS", "Government debt (% GDP)"},
};
const char *COUNTRIES[] = {"US","CN","JP","DE","GB","FR","IN","BR","CA","AU","KR","RU"};
const int N_IND = sizeof(INDS)/sizeof(INDS[0]), N_CTRY = sizeof(COUNTRIES)/sizeof(COUNTRIES[0]);

static int fetch_indicator(sqlite3 *db, const Indicator *ind, const char *ctry, MB *b) {
    char url[512]; snprintf(url,sizeof(url),"%s/%s/indicator/%s?format=json&per_page=50",API,ctry,ind->code);
    b->s = 0;
    CURL *c = curl_easy_init(); if(!c) return -1;
    curl_easy_setopt(c,CURLOPT_URL,url); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wcb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,b); curl_easy_setopt(c,CURLOPT_TIMEOUT,15L);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"MoneyRoom/1.0"); curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    CURLcode r = curl_easy_perform(c); long h=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&h);
    curl_easy_cleanup(c);
    if(r!=CURLE_OK||h!=200) return -1;
    return 0;
}

static int parse_indicator(sqlite3 *db, const Indicator *ind, const char *ctry, MB *b) {
    json_error_t e; json_t *j = json_loads(b->d,0,&e);
    if(!j||!json_is_array(j)||json_array_size(j)<2) { if(j) json_decref(j); return -1; }
    json_t *data = json_array_get(j,1);
    if(!json_is_array(data)) { json_decref(j); return -1; }
    int n = json_array_size(data), rows=0;
    for(int i=0;i<n;i++) {
        json_t *r = json_array_get(data,i);
        json_t *yr = json_object_get(r,"date"), *val = json_object_get(r,"value");
        if(!yr||!val) continue;
        int year = atoi(json_string_value(yr));
        if(!json_is_number(val)&&!json_is_string(val)) continue;
        double v = json_is_number(val) ? json_number_value(val) : atof(json_string_value(val));
        ins(db,ind->code,ctry,year,v,ind->name);
        rows++;
    }
    json_decref(j);
    return rows;
}

static void print_stats(sqlite3 *db) {
    sqlite3_stmt *s; sqlite3_prepare_v2(db,
        "SELECT indicator,country,COUNT(*),MAX(obs_year),ROUND(AVG(value),2),name "
        "FROM world_bank GROUP BY indicator,country ORDER BY indicator,country",-1,&s,0);
    printf("\n%-20s %-12s %8s %8s %12s  %s\n","INDICATOR","COUNTRY","ROWS","YEAR","AVG","NAME");
    printf("-------------------- ------------ -------- -------- ------------  "
           "------------------------------\n");
    int t=0;
    while(sqlite3_step(s)==SQLITE_ROW) {
        printf("%-20s %-12s %6d %8d %11.2f  %s\n",
               sqlite3_column_text(s,0),sqlite3_column_text(s,1),
               sqlite3_column_int(s,2),sqlite3_column_int(s,3),
               sqlite3_column_double(s,4),sqlite3_column_text(s,5));
        t+=sqlite3_column_int(s,2);
    }
    printf("--------------------\nTOTAL: %d rows\n",t);
    sqlite3_finalize(s);
}

int main(int argc, char **argv) {
    sqlite3 *db = odb(); if(!db) return 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    if(argc>1 && strcmp(argv[1],"stats")==0) { print_stats(db); sqlite3_close(db); return 0; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    MB b = {0};
    int ok=0,fail=0,tr=0;
    for(int i=0;i<N_IND;i++) {
        for(int j=0;j<N_CTRY;j++) {
            printf("[%d/%d][%d/%d] %s/%s\n",i+1,N_IND,j+1,N_CTRY,INDS[i].code,COUNTRIES[j]);
            if(b.d){b.s=0;}
            if(fetch_indicator(db,&INDS[i],COUNTRIES[j],&b)==0) {
                int r = parse_indicator(db,&INDS[i],COUNTRIES[j],&b);
                if(r>0){ok++;tr+=r;}else fail++;
            } else fail++;
        }
    }
    free(b.d); sqlite3_close(db); curl_global_cleanup();
    printf("\n=== WORLD BANK RESULT ===\nOK: %d, FAIL: %d, ROWS: %d\n",ok,fail,tr);
    return 0;
}
