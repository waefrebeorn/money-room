/**
 * travel_distance_calc.c — Travel Distance Between Venues
 * Computes approximate great-circle distances between team venues
 * Uses venue coordinates from ESPN's team/venue API
 *
 * Compile: gcc -O2 -Wall -o travel_distance_calc travel_distance_calc.c -lcurl -ljansson -lm
 * Usage:   ./travel_distance_calc
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sys/stat.h>

#define OUT_DIR  "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/travel_distances.json"

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
    h = curl_slist_append(h, "User-Agent: Mozilla/5.0");
    curl_easy_setopt(c, CURLOPT_URL, url); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb); curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L); curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c); curl_slist_free_all(h); curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(b.d); return NULL; } return b.d;
}
static const char *sstr(const json_t *o, const char *k) {
    json_t *v = o ? json_object_get(o, k) : NULL;
    return (v && json_is_string(v)) ? json_string_value(v) : "";
}
static double dist_km(double lat1, double lon1, double lat2, double lon2) {
    double R = 6371;
    double dlat = (lat2-lat1)*M_PI/180.0, dlon = (lon2-lon1)*M_PI/180.0;
    double a = sin(dlat/2)*sin(dlat/2) + cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dlon/2)*sin(dlon/2);
    return R * 2 * atan2(sqrt(a), sqrt(1-a));
}

int main(void) {
    curl_global_init(CURL_GLOBAL_ALL);
    json_t *root = json_array();
    
    typedef struct { const char *sport, *league, *label; } Src;
    Src sources[] = {{"baseball","mlb","MLB"},{"basketball","nba","NBA"},{"football","nfl","NFL"},{"hockey","nhl","NHL"}};
    int ns = 4;
    
    for(int si=0; si<ns; si++) {
        char url[256]; snprintf(url,sizeof(url),"https://site.api.espn.com/apis/site/v2/sports/%s/%s/teams",sources[si].sport,sources[si].league);
        char *resp = get(url); if(!resp) continue;
        json_error_t err; json_t *j = json_loads(resp,0,&err); free(resp); if(!j) continue;
        json_t *ta = json_object_get(json_array_get(json_object_get(json_array_get(json_object_get(j,"sports"),0),"leagues"),0),"teams");
        
        typedef struct { char name[128]; double lat, lon; } TeamVenue;
        TeamVenue tvs[100]; int nt = 0;
        
        size_t ti; json_t *te;
        json_array_foreach(ta,ti,te) {
            json_t *team = json_object_get(te,"team");
            const char *tn = sstr(team,"displayName");
            json_t *venue = json_object_get(team,"venue");
            if(!venue) continue;
            double lat = json_number_value(json_object_get(venue,"latitude"));
            double lon = json_number_value(json_object_get(venue,"longitude"));
            if(lat==0&&lon==0) continue;
            strncpy(tvs[nt].name, tn, 127);
            tvs[nt].lat = lat; tvs[nt].lon = lon; nt++;
        }
        json_decref(j);
        
        int pairs = 0;
        for(int i=0; i<nt && i<20; i++) {
            for(int j=i+1; j<nt && j<20; j++) {
                double d = dist_km(tvs[i].lat, tvs[i].lon, tvs[j].lat, tvs[j].lon);
                if(d < 100) continue; // Skip nearby teams
                json_t *e = json_pack("{s:s,s:s,s:s,s:s,s:f}",
                    "league", sources[si].label,
                    "from", tvs[i].name, "to", tvs[j].name,
                    "distance_km", d);
                json_array_append_new(root, e);
                pairs++;
            }
        }
        printf("[TRAVEL] %s: %d venues, %d pairs\n", sources[si].label, nt, pairs);
    }
    
    mkdir(OUT_DIR,0755); json_dump_file(root,OUT_FILE,JSON_INDENT(2));
    printf("[TRAVEL] Total distances -> %s\n", OUT_FILE);
    json_decref(root); curl_global_cleanup(); return 0;
}
