/**
 * schedule_collector.c — Upcoming Game Schedules
 * Fetches upcoming games from ESPN scoreboard for all leagues
 *
 * Compile: gcc -O2 -Wall -o schedule_collector schedule_collector.c -lcurl -ljansson -lm
 * Usage:   ./schedule_collector [--days 7] [--league nba]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <strings.h>
#include <sys/stat.h>

#define OUT_DIR  "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/schedule.json"

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
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    h = curl_slist_append(h, "Accept: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(b.data); return NULL; }
    return b.data;
}

typedef struct { const char *name, *sport, *league; } LeagueCfg;
static LeagueCfg LEAGUES[] = {
    {"NBA",  "basketball", "nba"},
    {"NFL",  "football",   "nfl"},
    {"MLB",  "baseball",   "mlb"},
    {"NHL",  "hockey",     "nhl"},
    {"EPL",  "soccer",     "eng.1"},
    {"LaLiga","soccer",    "esp.1"},
    {"SerieA","soccer",    "ita.1"},
    {"MLS",  "soccer",     "usa.1"},
};
#define N_LEAGUES 8

int main(int argc, char **argv) {
    int days_ahead = 7;
    const char *filter_league = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--days") == 0 && i+1 < argc) days_ahead = atoi(argv[++i]);
        else if (strcmp(argv[i], "--league") == 0 && i+1 < argc) filter_league = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strncmp(argv[i], "-h", 2) == 0) {
            printf("Usage: %s [--days N] [--league nba|nfl|...]\n", argv[0]); return 0;
        }
    }

    curl_global_init(CURL_GLOBAL_ALL);
    json_t *root = json_array();
    int total = 0;
    time_t now = time(NULL);

    for (int l = 0; l < N_LEAGUES; l++) {
        if (filter_league && strcasecmp(filter_league, LEAGUES[l].name) != 0) continue;
        printf("[SCHEDULE] %s... ", LEAGUES[l].name);
        fflush(stdout);
        int lg_count = 0;

        for (int d = 0; d < days_ahead; d++) {
            time_t day_ts = now + (time_t)d * 86400;
            struct tm *tp = gmtime(&day_ts);
            char date_str[16];
            snprintf(date_str, sizeof(date_str), "%04d%02d%02d",
                     tp->tm_year+1900, tp->tm_mon+1, tp->tm_mday);

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
            if (events && json_is_array(events)) {
                size_t ei;
                json_t *ev;
                json_array_foreach(events, ei, ev) {
                    const char *name = json_string_value(json_object_get(ev, "name"));
                    json_t *status_obj = json_object_get(ev, "status");
                    json_t *status_type = status_obj ? json_object_get(status_obj, "type") : NULL;
                    const char *status = status_type ? json_string_value(json_object_get(status_type, "description")) : "";
                    if (!name) continue;

                    json_t *comp_a = json_object_get(ev, "competitions");
                    json_t *comp = (comp_a && json_array_size(comp_a) > 0) ? json_array_get(comp_a, 0) : NULL;
                    if (!comp) continue;

                    json_t *compets = json_object_get(comp, "competitors");
                    const char *venue = "", *tv = "";
                    json_t *venue_o = json_object_get(comp, "venue");
                    if (venue_o) venue = json_string_value(json_object_get(venue_o, "fullName"));
                    if (!venue) venue = "";

                    // TV/Broadcast
                    json_t *broadcasts = json_object_get(comp, "broadcasts");
                    if (broadcasts && json_is_array(broadcasts) && json_array_size(broadcasts) > 0) {
                        json_t *b0 = json_array_get(broadcasts, 0);
                        json_t *names = json_object_get(b0, "names");
                        if (names && json_is_array(names) && json_array_size(names) > 0)
                            tv = json_string_value(json_array_get(names, 0));
                        if (!tv) tv = "";
                    }

                    const char *home_team = "", *away_team = "";
                    double home_score = 0, away_score = 0;
                    if (compets && json_is_array(compets) && json_array_size(compets) >= 2) {
                        json_t *c0 = json_array_get(compets, 0);
                        json_t *c1 = json_array_get(compets, 1);
                        const char *ha0 = json_string_value(json_object_get(c0, "homeAway"));
                        json_t *h_t = json_object_get(c0, "team");
                        json_t *a_t = json_object_get(c1, "team");
                        if (strcmp(ha0, "home") == 0) {
                            home_team = json_string_value(json_object_get(h_t, "displayName"));
                            away_team = json_string_value(json_object_get(a_t, "displayName"));
                            home_score = json_number_value(json_object_get(c0, "score"));
                            away_score = json_number_value(json_object_get(c1, "score"));
                        } else {
                            home_team = json_string_value(json_object_get(a_t, "displayName"));
                            away_team = json_string_value(json_object_get(h_t, "displayName"));
                            home_score = json_number_value(json_object_get(c1, "score"));
                            away_score = json_number_value(json_object_get(c0, "score"));
                        }
                    }

                    json_t *entry = json_pack("{s:s, s:s, s:s, s:s, s:s, s:s, s:f, s:f, s:s, s:s, s:f}",
                        "league", LEAGUES[l].name,
                        "game", name ? name : "",
                        "date", date_str,
                        "status", status ? status : "",
                        "home_team", home_team,
                        "away_team", away_team,
                        "home_score", home_score,
                        "away_score", away_score,
                        "venue", venue,
                        "tv", tv,
                        "game_time", (double)day_ts);
                    json_array_append_new(root, entry);
                    lg_count++;
                }
            }
            json_decref(j);
        }
        total += lg_count;
        printf("%d upcoming games\n", lg_count);
    }

    mkdir(OUT_DIR, 0755);
    json_dump_file(root, OUT_FILE, JSON_INDENT(2));
    printf("[SCHEDULE] Total: %d upcoming games -> %s\n", total, OUT_FILE);
    json_decref(root);
    curl_global_cleanup();
    return 0;
}
