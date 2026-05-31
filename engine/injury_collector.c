/**
 * injury_collector.c — Sports Injury Report Collector
 * Fetches injury reports from ESPN for all leagues
 *
 * Compile: gcc -O2 -Wall -o injury_collector injury_collector.c -lcurl -ljansson -lsqlite3 -lm
 * Usage:   ./injury_collector [--league nba]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <strings.h>
#include <sys/stat.h>

#define OUT_DIR  "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/injuries.json"
#define DB_PATH  "/home/wubu2/money-room/engine/timeline.db"

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

// League config
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
    {"NCAAF","football",   "college-football"},
    {"NCAAB","basketball", "mens-college-basketball"},
};
#define N_LEAGUES 10

int main(int argc, char **argv) {
    const char *filter_league = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--league") == 0 && i+1 < argc) filter_league = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strncmp(argv[i], "-h", 2) == 0) {
            printf("Usage: %s [--league nba|nfl|...]\n", argv[0]);
            return 0;
        }
    }

    curl_global_init(CURL_GLOBAL_ALL);

    // Setup DB
    sqlite3 *db;
    sqlite3_open(DB_PATH, &db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS injuries ("
        "id TEXT PRIMARY KEY, league TEXT, team TEXT, "
        "player_name TEXT, position TEXT, "
        "injury_type TEXT, status TEXT, "
        "comment TEXT, date TEXT, fetched_at INTEGER)",
        NULL, NULL, NULL);

    json_t *root = json_array();
    int total_injuries = 0;

    for (int l = 0; l < N_LEAGUES; l++) {
        if (filter_league && strcasecmp(filter_league, LEAGUES[l].name) != 0) continue;
        printf("[INJURY] %s... ", LEAGUES[l].name);
        fflush(stdout);

        char url[256];
        snprintf(url, sizeof(url),
            "https://site.api.espn.com/apis/site/v2/sports/%s/%s/injuries",
            LEAGUES[l].sport, LEAGUES[l].league);

        char *resp = http_get(url);
        if (!resp) { printf("no data\n"); continue; }

        json_error_t err;
        json_t *j = json_loads(resp, 0, &err);
        free(resp);
        if (!j) { printf("parse fail\n"); continue; }

        json_t *injuries_list = json_object_get(j, "injuries");
        if (!injuries_list || !json_is_array(injuries_list)) {
            json_decref(j);
            printf("0 teams\n");
            continue;
        }

        int team_count = 0, inj_count = 0;

        size_t ti;
        json_t *team_entry;
        json_array_foreach(injuries_list, ti, team_entry) {
            const char *team_name = json_string_value(json_object_get(team_entry, "displayName"));
            if (!team_name) continue;
            team_count++;

            json_t *team_injuries = json_object_get(team_entry, "injuries");
            if (!team_injuries || !json_is_array(team_injuries)) continue;

            size_t ii;
            json_t *injury;
            json_array_foreach(team_injuries, ii, injury) {
                const char *status = json_string_value(json_object_get(injury, "status"));
                if (!status) continue;

                json_t *ath = json_object_get(injury, "athlete");
                const char *pname = ath ? json_string_value(json_object_get(ath, "displayName")) : "Unknown";
                const char *pos = "";
                json_t *pos_obj = ath ? json_object_get(ath, "position") : NULL;
                if (pos_obj) {
                    json_t *pos_name = json_object_get(pos_obj, "name");
                    if (pos_name) pos = json_string_value(pos_name);
                }

                const char *injury_id = json_string_value(json_object_get(injury, "id"));
                const char *stype = json_string_value(json_object_get(injury, "type"));
                const char *comment = json_string_value(json_object_get(injury, "shortComment"));
                if (!comment) comment = json_string_value(json_object_get(injury, "longComment"));
                if (!stype) stype = "";
                if (!comment) comment = "";

                json_t *entry = json_pack(
                    "{s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s}",
                    "id", injury_id ? injury_id : "",
                    "league", LEAGUES[l].name,
                    "team", team_name,
                    "player", pname,
                    "position", pos,
                    "type", stype,
                    "status", status,
                    "comment", comment
                );
                json_array_append_new(root, entry);

                // Write to DB
                const char *sql = "INSERT OR REPLACE INTO injuries "
                    "(id, league, team, player_name, position, injury_type, status, comment, fetched_at) "
                    "VALUES (?,?,?,?,?,?,?,?,?)";
                sqlite3_stmt *st;
                if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, injury_id, -1, SQLITE_STATIC);
                    sqlite3_bind_text(st, 2, LEAGUES[l].name, -1, SQLITE_STATIC);
                    sqlite3_bind_text(st, 3, team_name, -1, SQLITE_STATIC);
                    sqlite3_bind_text(st, 4, pname, -1, SQLITE_STATIC);
                    sqlite3_bind_text(st, 5, pos, -1, SQLITE_STATIC);
                    sqlite3_bind_text(st, 6, stype, -1, SQLITE_STATIC);
                    sqlite3_bind_text(st, 7, status, -1, SQLITE_STATIC);
                    sqlite3_bind_text(st, 8, comment, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(st, 9, (sqlite3_int64)time(NULL));
                    sqlite3_step(st);
                    sqlite3_finalize(st);
                }
                inj_count++;
            }
        }
        total_injuries += inj_count;
        printf("%d teams, %d injuries\n", team_count, inj_count);
        json_decref(j);
    }

    // Write output
    mkdir(OUT_DIR, 0755);
    json_dump_file(root, OUT_FILE, JSON_INDENT(2));
    printf("[INJURY] Total: %d injuries -> %s\n", total_injuries, OUT_FILE);

    json_decref(root);
    sqlite3_close(db);
    curl_global_cleanup();
    return 0;
}
