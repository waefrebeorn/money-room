/**
 * sports_collector.c — Fetch game results from ESPN free API
 * Binary outcomes: home team score > away team score = YES(1)
 * 
 * Compile: gcc -O2 -o sports_collector sports_collector.c -lcurl -ljansson -lm
 * Usage:   ./sports_collector [--days N] [--league nba]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <strings.h>

#define OUT_DIR  "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/sports_data.json"

typedef struct { char *data; size_t len; } http_buf_t;

static size_t write_cb(void *ptr, size_t s, size_t n, void *u) {
    size_t t = s * n; http_buf_t *b = (http_buf_t*)u;
    char *np = realloc(b->data, b->len + t + 1);
    if (!np) return 0; b->data = np;
    memcpy(b->data + b->len, ptr, t); b->len += t; b->data[b->len] = '\0';
    return t;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    http_buf_t b = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; sports-collector/1.0)");
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) { fprintf(stderr, "[SPORTS] HTTP error: %s\n", curl_easy_strerror(r)); free(b.data); return NULL; }
    return b.data;
}

// League configs
typedef struct { const char *name, *sport, *league; } LeagueCfg;
static LeagueCfg LEAGUES[] = {
    {"NBA",  "basketball", "nba"},
    {"NFL",  "football",   "nfl"},
    {"MLB",  "baseball",   "mlb"},
    {"NHL",  "hockey",     "nhl"},
    {"EPL",  "soccer",     "eng.1"},
    {"LaLiga","soccer",    "esp.1"},
    {"SerieA","soccer",    "ita.1"},
};
#define N_LEAGUES 7

int main(int argc, char **argv) {
    int days_back = 30;  // ESPN has ~30-60 days of history
    const char *filter_league = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--days") == 0 && i+1 < argc) days_back = atoi(argv[++i]);
        else if (strcmp(argv[i], "--league") == 0 && i+1 < argc) filter_league = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strncmp(argv[i], "-h", 2) == 0) {
            printf("Usage: %s [--days N] [--league nba|nfl|mlb|nhl|epl]\n", argv[0]); return 0;
        }
    }
    
    curl_global_init(CURL_GLOBAL_ALL);
    
    json_t *root = json_array();
    int total_games = 0, active_leagues = 0;
    
    time_t now = time(NULL);
    
    for (int l = 0; l < N_LEAGUES; l++) {
        if (filter_league && strcasecmp(filter_league, LEAGUES[l].name) != 0) continue;
        
        printf("[SPORTS] Scanning %s...\n", LEAGUES[l].name);
        int league_games = 0;
        
        for (int d = 0; d < days_back; d++) {
            time_t day_ts = now - (time_t)d * 86400;
            struct tm *tp = gmtime(&day_ts);
            char date_str[16];
            snprintf(date_str, sizeof(date_str), "%04d%02d%02d", tp->tm_year+1900, tp->tm_mon+1, tp->tm_mday);
            
            char url[512];
            snprintf(url, sizeof(url),
                "https://site.api.espn.com/apis/site/v2/sports/%s/%s/scoreboard?dates=%s",
                LEAGUES[l].sport, LEAGUES[l].league, date_str);
            
            char *resp = http_get(url);
            if (!resp) continue;
            
            json_error_t err;
            json_t *j = json_loads(resp, 0, &err);
            free(resp);
            if (!j) continue;
            
            json_t *events = json_object_get(j, "events");
            if (!events || !json_is_array(events)) { json_decref(j); continue; }
            
            int n = (int)json_array_size(events);
            for (int i = 0; i < n; i++) {
                json_t *ev = json_array_get(events, i);
                if (!ev) continue;
                
                const char *ev_name = json_string_value(json_object_get(ev, "name"));
                const char *ev_date = json_string_value(json_object_get(ev, "date"));
                if (!ev_name || !ev_date) continue;
                
                json_t *comp = json_object_get(ev, "competitions");
                if (!comp || !json_is_array(comp) || json_array_size(comp) == 0) continue;
                json_t *comp0 = json_array_get(comp, 0);
                
                // Get competitors
                json_t *competitors = json_object_get(comp0, "competitors");
                if (!competitors || !json_is_array(competitors) || json_array_size(competitors) < 2) continue;
                
                json_t *home = NULL, *away = NULL;
                for (int c = 0; c < (int)json_array_size(competitors); c++) {
                    json_t *comp_c = json_array_get(competitors, c);
                    json_t *jhome = json_object_get(comp_c, "homeAway");
                    const char *ha = json_string_value(jhome);
                    if (ha && strcmp(ha, "home") == 0) home = comp_c;
                    else away = comp_c;
                }
                if (!home || !away) continue;
                
                // Team names are nested under "team" object
                json_t *home_team = json_object_get(home, "team");
                json_t *away_team = json_object_get(away, "team");
                const char *home_name = home_team ? json_string_value(json_object_get(home_team, "displayName")) : NULL;
                const char *away_name = away_team ? json_string_value(json_object_get(away_team, "displayName")) : NULL;
                if (!home_name) home_name = json_string_value(json_object_get(home, "name"));
                if (!away_name) away_name = json_string_value(json_object_get(away, "name"));
                json_t *home_score = json_object_get(home, "score");
                json_t *away_score = json_object_get(away, "score");
                json_t *home_rank = json_object_get(home, "rank");
                json_t *away_rank = json_object_get(away, "rank");
                
                if (!home_name || !away_name || !home_score || !away_score) continue;
                if (!json_is_string(home_score) && !json_is_number(home_score)) continue;
                
                double hs = json_is_string(home_score) ? atof(json_string_value(home_score)) : json_number_value(home_score);
                double as = json_is_string(away_score) ? atof(json_string_value(away_score)) : json_number_value(away_score);
                // Skip games that haven't started
                if (hs == 0 && as == 0) continue;
                
                int outcome = (hs > as) ? 1 : 0;  // Home win = YES
                int h_rank = (home_rank && json_is_number(home_rank)) ? (int)json_number_value(home_rank) : 0;
                int a_rank = (away_rank && json_is_number(away_rank)) ? (int)json_number_value(away_rank) : 0;
                
                // Parse date to timestamp
                struct tm tm = {0};
                sscanf(ev_date, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
                tm.tm_year -= 1900; tm.tm_mon -= 1;
                int64_t ts = (int64_t)mktime(&tm);
                
                // Features for ML
                double score_diff = hs - as;
                double total_score = hs + as;
                double home_win_pct = (double)outcome;  // Current result
                
                json_t *entry = json_object();
                json_object_set_new(entry, "league", json_string(LEAGUES[l].name));
                json_object_set_new(entry, "sport", json_string(LEAGUES[l].sport));
                json_object_set_new(entry, "game", json_string(ev_name));
                json_object_set_new(entry, "home_team", json_string(home_name));
                json_object_set_new(entry, "away_team", json_string(away_name));
                json_object_set_new(entry, "home_score", json_real(hs));
                json_object_set_new(entry, "away_score", json_real(as));
                json_object_set_new(entry, "timestamp", json_integer(ts));
                json_object_set_new(entry, "date", json_string(ev_date));
                json_object_set_new(entry, "outcome", json_integer(outcome));
                if (h_rank > 0) json_object_set_new(entry, "home_rank", json_integer(h_rank));
                if (a_rank > 0) json_object_set_new(entry, "away_rank", json_integer(a_rank));
                
                // Features for training
                json_t *feats = json_array();
                json_array_append_new(feats, json_real(score_diff));  // F0: score diff
                json_array_append_new(feats, json_real(total_score));  // F1: total score
                json_array_append_new(feats, json_real(hs / fmax(total_score, 1)));  // F2: home share
                json_array_append_new(feats, json_real(h_rank > 0 ? (30.0 - h_rank) / 30.0 : 0.5));  // F3: home rank
                json_array_append_new(feats, json_real(a_rank > 0 ? (30.0 - a_rank) / 30.0 : 0.5));  // F4: away rank
                json_object_set_new(entry, "features", feats);
                
                json_array_append_new(root, entry);
                league_games++;
                total_games++;
            }
            json_decref(j);
        }
        
        if (league_games > 0) active_leagues++;
        printf("[SPORTS] %s: %d games\n", LEAGUES[l].name, league_games);
    }
    
    // Write output
    mkdir(OUT_DIR, 0755);
    if (json_dump_file(root, OUT_FILE, JSON_INDENT(2)) == 0) {
        printf("\n[SPORTS] Written: %s (%d games, %d leagues)\n", OUT_FILE, total_games, active_leagues);
    } else {
        fprintf(stderr, "[SPORTS] Failed to write %s\n", OUT_FILE);
    }
    
    json_decref(root);
    curl_global_cleanup();
    return 0;
}
