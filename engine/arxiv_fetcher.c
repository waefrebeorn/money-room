/**
 * arxiv_fetcher.c — ArXiv quant-fin paper fetcher (T1401-T1403)
 * Free API, no key needed. Fetches latest q-fin.ST, q-fin.CP, cs.CE papers.
 * 
 * Compile: gcc -O3 -Wall -Wextra -o arxiv_fetcher arxiv_fetcher.c -lcurl -lsqlite3 -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <time.h>

#define DB_PATH "/home/wubu2/money-room/engine/timeline.db"
#define API "http://export.arxiv.org/api/query"

typedef struct { char *d; size_t s; } MB;
static size_t wcb(void *p, size_t sz, size_t n, void *u) {
    MB *b=(MB*)u; size_t t=sz*n; char *nw=realloc(b->d,b->s+t+1);
    if(!nw) return 0; b->d=nw; memcpy(b->d+b->s,p,t); b->s+=t; b->d[b->s]=0; return t;
}

static sqlite3 *odb() {
    sqlite3 *db; if(sqlite3_open(DB_PATH,&db)!=SQLITE_OK) return NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS arxiv_papers ("
        "  paper_id TEXT PRIMARY KEY, title TEXT, authors TEXT,"
        "  published TEXT, category TEXT, abstract TEXT,"
        "  updated_at TEXT DEFAULT (datetime('now'))"
        ");", 0, 0, 0);
    return db;
}

static int fetch(const char *cat, const char *sort, MB *b, int max) {
    char url[1024]; snprintf(url,sizeof(url),"%s?search_query=cat:%s&max_results=%d&sortBy=%s&sortOrder=descending",
                            API,cat,max,sort);
    b->s=0; CURL *c=curl_easy_init(); if(!c) return -1;
    curl_easy_setopt(c,CURLOPT_URL,url); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wcb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,b); curl_easy_setopt(c,CURLOPT_TIMEOUT,30L);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"MoneyRoom/1.0 (mailto:wubu@research)"); curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
    CURLcode r=curl_easy_perform(c); long h=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&h);
    curl_easy_cleanup(c); return (r==CURLE_OK&&h==200)?0:-1;
}

static void extract(const char *xml, const char *tag, char *out, int max) {
    const char *s=xml,*e; out[0]=0;
    char open[64]; snprintf(open,sizeof(open),"<%s>",tag);
    char close[64]; snprintf(close,sizeof(close),"</%s>",tag);
    s=strstr(xml,open); if(!s) return;
    s+=strlen(open); e=strstr(s,close); if(!e) return;
    int n=(int)(e-s); if(n>max-1)n=max-1;
    strncpy(out,s,n); out[n]=0;
}

static int parse_and_store(sqlite3 *db, MB *b, const char *category) {
    char *p = b->d;
    int count=0;
    sqlite3_stmt *s; sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO arxiv_papers (paper_id,title,authors,published,category,abstract) "
        "VALUES(?1,?2,?3,?4,?5,?6)",-1,&s,0);
    
    while((p=strstr(p,"<entry>"))!=NULL) {
        char entry[32768]; int n=0; char *e=strstr(p,"</entry>");
        if(!e) break; n=(int)(e-p+8); if(n>32767)n=32767;
        strncpy(entry,p,n); entry[n]=0;
        
        char id[256]="", title[512]="", authors[4096]="", published[32]="", abstr[4096]="";
        extract(entry,"id",id,sizeof(id));       // http://arxiv.org/abs/XXXX.XXXXX
        char *slash=strrchr(id,'/'); if(slash) { char *tmp=slash+1; memmove(id,tmp,strlen(tmp)+1); }
        extract(entry,"title",title,sizeof(title));
        // Remove newlines from title
        for(char *t=title;*t;t++) if(*t=='\n'||*t=='\r'||*t=='\t') *t=' ';
        // Collapse spaces
        char *src=title,*dst=title; int sp=0;
        while(*src){if(*src==' '){if(!sp)*dst++=' ';sp=1;}else{*dst++=*src;sp=0;}src++;}*dst=0;
        
        extract(entry,"published",published,sizeof(published));
        extract(entry,"summary",abstr,sizeof(abstr));
        // Clean abstract
        for(char *t=abstr;*t;t++) if(*t=='\n') *t=' ';
        
        // Extract authors
        char *ap = entry; int first=1;
        while((ap=strstr(ap,"<author>"))!=NULL) {
            char aname[128]=""; extract(ap,"name",aname,sizeof(aname));
            if(!first) strcat(authors,", ");
            strcat(authors,aname); first=0;
            ap+=8;
        }
        
        sqlite3_bind_text(s,1,id,-1,SQLITE_STATIC);
        sqlite3_bind_text(s,2,title,-1,SQLITE_STATIC);
        sqlite3_bind_text(s,3,authors,-1,SQLITE_STATIC);
        sqlite3_bind_text(s,4,published,-1,SQLITE_STATIC);
        sqlite3_bind_text(s,5,category,-1,SQLITE_STATIC);
        sqlite3_bind_text(s,6,abstr,-1,SQLITE_STATIC);
        if(sqlite3_step(s)==SQLITE_DONE) count++;
        sqlite3_reset(s);
        
        p = e + 8;
    }
    sqlite3_finalize(s);
    return count;
}

static void print_stats(sqlite3 *db) {
    sqlite3_stmt *s; sqlite3_prepare_v2(db,
        "SELECT category,COUNT(*),MAX(published) FROM arxiv_papers GROUP BY category",-1,&s,0);
    printf("\n%-15s %8s %24s\n","CATEGORY","PAPERS","LATEST");
    while(sqlite3_step(s)==SQLITE_ROW)
        printf("%-15s %6d %24s\n",sqlite3_column_text(s,0),sqlite3_column_int(s,1),sqlite3_column_text(s,2));
    sqlite3_finalize(s);
    sqlite3_prepare_v2(db,"SELECT title,published,authors FROM arxiv_papers ORDER BY published DESC LIMIT 10",-1,&s,0);
    printf("\n--- LATEST 10 PAPERS ---\n");
    while(sqlite3_step(s)==SQLITE_ROW)
        printf("  [%s] %s\n  ˑ %s\n\n",sqlite3_column_text(s,1),sqlite3_column_text(s,0),sqlite3_column_text(s,2));
    sqlite3_finalize(s);
}

int main(int argc, char **argv) {
    sqlite3 *db=odb(); if(!db) return 1;
    if(argc>1&&strcmp(argv[1],"stats")==0){print_stats(db);sqlite3_close(db);return 0;}
    
    const char *cats[] = {"q-fin.ST","q-fin.CP","q-fin.GN","cs.CE","cs.AI","stat.ML"};
    const char *names[] = {"Statistical Finance","Computational Finance","General Finance","Computational Engineering","AI","ML"};
    int n = sizeof(cats)/sizeof(cats[0]);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    MB b={0}; int total=0;
    for(int i=0;i<n;i++) {
        printf("[%d/%d] Fetching %s...\n",i+1,n,names[i]);
        if(b.d){b.s=0;}
        if(fetch(cats[i],"submittedDate",&b,20)==0){
            int c=parse_and_store(db,&b,names[i]);
            printf("  -> %d papers stored\n",c);
            total+=c;
        } else printf("  -> FAILED\n");
    }
    free(b.d); sqlite3_close(db); curl_global_cleanup();
    printf("\n=== ARXIV FETCHER RESULT ===\nTOTAL: %d papers\n",total);
    return 0;
}
