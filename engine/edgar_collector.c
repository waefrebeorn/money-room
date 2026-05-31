/**
 * edgar_collector.c — SEC EDGAR 10-K/13F scraper (T442-T452)
 * Free SEC API (data.sec.gov), no key needed.
 * 
 * Compile: gcc -O3 -Wall -Wextra -o edgar_collector edgar_collector.c -lcurl -lsqlite3 -lm -ljansson
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
static size_t wcb(void *p, size_t sz, size_t n, void *u) {
    MB *b=(MB*)u; size_t t=sz*n; char *nw=realloc(b->d,b->s+t+1);
    if(!nw) return 0; b->d=nw; memcpy(b->d+b->s,p,t); b->s+=t; b->d[b->s]=0; return t;
}

static sqlite3 *odb() {
    sqlite3 *db; if(sqlite3_open(DB_PATH,&db)!=SQLITE_OK) return NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS edgar_filings ("
        "  cik INTEGER, company TEXT, form TEXT, filing_date TEXT,"
        "  description TEXT, accession TEXT, primary_doc TEXT,"
        "  updated_at TEXT DEFAULT (datetime('now')),"
        "  PRIMARY KEY (cik, accession)"
        ");", 0, 0, 0);
    return db;
}

static int http(const char *url, MB *b) {
    b->s=0; CURL *c=curl_easy_init(); if(!c) return -1;
    curl_easy_setopt(c,CURLOPT_URL,url); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wcb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,b); curl_easy_setopt(c,CURLOPT_TIMEOUT,20L);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"MoneyRoom/1.0 (mailto:research@moneyroom.io)");
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    CURLcode r=curl_easy_perform(c); long h=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&h);
    curl_easy_cleanup(c); return (r==CURLE_OK&&h==200)?0:-1;
}

/* Key companies to track: CIK numbers */
typedef struct { int cik; const char *name; const char *ticker; } Company;
Company COMPANIES[] = {
    {320193, "Apple Inc", "AAPL"},
    {789019, "Microsoft Corp", "MSFT"},
    {1018724, "Amazon.com Inc", "AMZN"},
    {1652044, "Alphabet Inc (GOOGL)", "GOOGL"},
    {1045810, "NVIDIA Corp", "NVDA"},
    {1326801, "Meta Platforms Inc", "META"},
    {1318605, "Tesla Inc", "TSLA"},
    {315189, "JPMorgan Chase & Co", "JPM"},
    {1403161, "Visa Inc", "V"},
    {1067983, "Berkshire Hathaway", "BRK-B"},
};
const int N_COMP = sizeof(COMPANIES)/sizeof(COMPANIES[0]);

static void ins_filing(sqlite3 *db, int cik, const char *co, const char *form,
                        const char *date, const char *desc, const char *acc) {
    sqlite3_stmt *s; sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO edgar_filings (cik,company,form,filing_date,description,accession)"
        " VALUES(?1,?2,?3,?4,?5,?6)",-1,&s,0);
    sqlite3_bind_int(s,1,cik); sqlite3_bind_text(s,2,co,-1,SQLITE_STATIC);
    sqlite3_bind_text(s,3,form,-1,SQLITE_STATIC); sqlite3_bind_text(s,4,date,-1,SQLITE_STATIC);
    sqlite3_bind_text(s,5,desc,-1,SQLITE_STATIC); sqlite3_bind_text(s,6,acc,-1,SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);
}

static int fetch_company(sqlite3 *db, const Company *co, MB *b) {
    char url[256]; snprintf(url,sizeof(url),"https://data.sec.gov/submissions/CIK%010d.json",co->cik);
    if(http(url,b)!=0) return -1;
    
    json_t *j=json_loads(b->d,0,0); if(!j) return -1;
    json_t *filings=json_object_get(j,"filings"); if(!filings){json_decref(j);return -1;}
    json_t *recent=json_object_get(filings,"recent"); if(!recent){json_decref(j);return -1;}
    
    json_t *forms=json_object_get(recent,"form");
    json_t *dates=json_object_get(recent,"filingDate");
    json_t *desc=json_object_get(recent,"primaryDocument");
    json_t *accs=json_object_get(recent,"accessionNumber");
    
    if(!json_is_array(forms)||!json_is_array(dates)){json_decref(j);return -1;}
    
    int n=(int)json_array_size(forms), count=0;
    int max= n<100 ? n : 100;
    
    for(int i=0;i<max;i++) {
        const char *form = json_string_value(json_array_get(forms,i));
        const char *date = json_string_value(json_array_get(dates,i));
        const char *acsn = accs ? json_string_value(json_array_get(accs,i)) : "";
        const char *doc = desc ? json_string_value(json_array_get(desc,i)) : "";
        // Filter: 10-K, 10-Q, 13F-HR, 8-K, Form 4
        if(form && (strstr(form,"10-K")||strstr(form,"10-Q")||strstr(form,"13F-HR")||
                    strstr(form,"8-K")||strcmp(form,"4")==0)) {
            ins_filing(db,co->cik,co->name,form,date,doc,acsn);
            count++;
        }
    }
    json_decref(j);
    return count;
}

static void print_stats(sqlite3 *db) {
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT company,form,COUNT(*),MAX(filing_date) FROM edgar_filings "
        "GROUP BY company,form ORDER BY company,form",-1,&s,0);
    printf("\n%-25s %-12s %6s %16s\n","COMPANY","FORM","COUNT","LATEST");
    while(sqlite3_step(s)==SQLITE_ROW)
        printf("%-25s %-12s %4d %16s\n",sqlite3_column_text(s,0),sqlite3_column_text(s,1),
               sqlite3_column_int(s,2),sqlite3_column_text(s,3));
    sqlite3_finalize(s);
    
    sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM edgar_filings",-1,&s,0);
    if(sqlite3_step(s)==SQLITE_ROW)
        printf("\nTOTAL filings: %d\n",sqlite3_column_int(s,0));
    sqlite3_finalize(s);
}

int main(int argc, char **argv) {
    sqlite3 *db=odb(); if(!db) return 1;
    if(argc>1&&strcmp(argv[1],"stats")==0){print_stats(db);sqlite3_close(db);return 0;}
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    MB b={0}; int total=0, ok=0, fail=0;
    for(int i=0;i<N_COMP;i++) {
        printf("[%d/%d] %s (CIK=%d)\n",i+1,N_COMP,COMPANIES[i].name,COMPANIES[i].cik);
        if(b.d){b.s=0;}
        int r=fetch_company(db,&COMPANIES[i],&b);
        if(r>0){ok++;total+=r;printf("  -> %d filings\n",r);}
        else fail++;
    }
    free(b.d); sqlite3_close(db); curl_global_cleanup();
    printf("\n=== EDGAR RESULT ===\nCompanies: %d OK, %d FAIL\nTotal filings: %d\n",ok,fail,total);
    return 0;
}
