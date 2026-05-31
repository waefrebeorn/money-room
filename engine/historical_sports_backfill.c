/**
 * historical_sports_backfill.c — Historical Sports Data Backfill
 * Fetches 5+ years of game data from ESPN for all leagues
 *
 * Backfill pattern: iterate year by year, dates in key months per sport
 *
 * Compile: gcc -O2 -Wall -o historical_sports_backfill historical_sports_backfill.c -lcurl -ljansson -lsqlite3 -lm
 * Usage:   ./historical_sports_backfill [--year 2024] [--league mlb]
 */
#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE
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

#define DB_PATH "/home/wubu2/.hermes/pm_logs/outcomes.db"
#define OUT_DIR  "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/historical_sports.json"

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

typedef struct {
    const char *name, *sport, *league;
    int start_year;
    int months[12]; // 1=active that month, 0=offseason
} LeagueCfg;

static LeagueCfg LEAGUES[] = {
    {"NBA",  "basketball", "nba",  2018, {0,1,1,1,1,1,0,0,0,0,1,1}},
    {"NFL",  "football",   "nfl",  2018, {0,0,0,0,0,0,1,1,1,1,1,1}},
    {"MLB",  "baseball",   "mlb",  2018, {0,0,1,1,1,1,1,1,1,1,0,0}},
    {"NHL",  "hockey",     "nhl",  2018, {0,1,1,1,1,1,0,0,0,0,1,1}},
    {"EPL",  "soccer",     "eng.1", 2018, {1,1,1,1,0,0,0,1,1,1,1,1}},
    {"LaLiga","soccer",    "esp.1", 2018, {1,1,1,1,0,0,0,1,1,1,1,1}},
    {"SerieA","soccer",    "ita.1", 2018, {1,1,1,1,0,0,0,1,1,1,1,1}},
    {"MLS",  "soccer",     "usa.1", 2019, {0,0,0,0,1,1,1,1,1,1,0,0}},
};
#define N_LEAGUES 8

static double safe_num(const json_t *o, const char *k) {
    json_t *v = o ? json_object_get(o, k) : NULL;
    if (!v) return 0;
    if (json_is_real(v)) return json_real_value(v);
    if (json_is_integer(v)) return (double)json_integer_value(v);
    if (json_is_string(v)) return atof(json_string_value(v));
    return 0;
}

static const char *safe_str(const json_t *o, const char *k) {
    json_t *v = o ? json_object_get(o, k) : NULL;
    if (!v || !json_is_string(v)) return "";
    return json_string_value(v);
}

int main(int argc, char **argv) {
    int filter_year = 0;
    const char *filter_league = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--year") == 0 && i+1 < argc) filter_year = atoi(argv[++i]);
        else if (strcmp(argv[i], "--league") == 0 && i+1 < argc) filter_league = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strncmp(argv[i], "-h", 2) == 0) {
            printf("Usage: %s [--year 2024] [--league mlb|nba|...]\n", argv[0]); return 0;
        }
    }

    curl_global_init(CURL_GLOBAL_ALL);

    sqlite3 *db;
    sqlite3_open(DB_PATH, &db);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS sports_outcomes ("
        "game_id TEXT PRIMARY KEY, league TEXT, home_team TEXT, away_team TEXT, "
        "home_score INTEGER, away_score INTEGER, winner TEXT, spread REAL, "
        "over_under REAL, status TEXT, game_time INTEGER, collected_at INTEGER)",
        NULL, NULL, NULL);

    json_t *root = json_array();
    int total_games = 0, new_games = 0;
    int current_year = 2026; // current season

    for (int l = 0; l < N_LEAGUES; l++) {
        if (filter_league && strcasecmp(filter_league, LEAGUES[l].name) != 0) continue;
        printf("[BACKFILL] %s...\n", LEAGUES[l].name);

        int end_year = current_year;
        int start = filter_year ? filter_year : LEAGUES[l].start_year;

        for (int year = start; year <= end_year; year++) {
            for (int m = 0; m < 12; m++) {
                if (!LEAGUES[l].months[m]) continue;

                // Sample 4-6 dates per month (every ~5-7 days)
                int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                int nd = days_in_month[m];
                int step = nd / 5;
                if (step < 1) step = 1;

                for (int d = 1; d <= nd; d += step) {
                    char date_str[16];
                    snprintf(date_str, sizeof(date_str), "%04d%02d%02d", year, m+1, d);

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
                            const char *status = safe_str(
                                json_object_get(json_object_get(ev, "status"), "type"), "description");
                            if (strcmp(status, "Final") != 0) continue;

                            json_t *comp_a = json_object_get(ev, "competitions");
                            json_t *comp = (comp_a && json_array_size(comp_a) > 0)
                                ? json_array_get(comp_a, 0) : NULL;
                            if (!comp) continue;

                            json_t *compets = json_object_get(comp, "competitors");
                            if (!compets || json_array_size(compets) < 2) continue;

                            json_t *home_c = json_array_get(compets, 0);
                            json_t *away_c = json_array_get(compets, 1);
                            const char *ha = safe_str(home_c, "homeAway");
                            if (strcmp(ha, "away") == 0) {
                                json_t *tmp = home_c; home_c = away_c; away_c = tmp;
                            }

                            double hs = safe_num(home_c, "score");
                            double as = safe_num(away_c, "score");
                            const char *hn = safe_str(json_object_get(home_c, "team"), "displayName");
                            const char *an = safe_str(json_object_get(away_c, "team"), "displayName");
                            const char *ha_a = safe_str(json_object_get(home_c, "team"), "abbreviation");
                            const char *aa = safe_str(json_object_get(away_c, "team"), "abbreviation");

                            // Get game time from competition
                            const char *comp_date = safe_str(comp, "date");
                            time_t game_ts = 0;
                            if (comp_date[0]) {
                                struct tm tm = {0};
                                if (strptime(comp_date, "%Y-%m-%dT%H:%MZ", &tm))
                                    game_ts = timegm(&tm);
                            }
                            if (game_ts == 0) game_ts = (time_t)(year * 365 * 86400 + m * 30 * 86400 + d * 86400);

                            char game_id[256];
                            snprintf(game_id, sizeof(game_id), "%s_%04d%02d%02d_%s_%s",
                                     LEAGUES[l].name, year, m+1, d, an, hn);

                            // Build JSON
                            json_t *game = json_pack("{s:s, s:s, s:s, s:s, s:s, s:s,"
                                                      "s:f, s:f, s:s, s:f, s:s, s:s}",
                                "league", LEAGUES[l].name,
                                "game", an, " vs ", hn,
                                "home_team", hn, "away_team", an,
                                "home_abbr", ha_a, "away_abbr", aa,
                                "home_score", hs, "away_score", as,
                                "date", date_str,
                                "game_time", (double)game_ts,
                                "status", "Final",
                                "outcome", (hs > as) ? "home" : "away");
                            json_array_append_new(root, game);

                            // Write to DB if new
                            sqlite3_stmt *ck;
                            int exists = 0;
                            if (sqlite3_prepare_v2(db, "SELECT 1 FROM sports_outcomes WHERE game_id=?",
                                                  -1, &ck, NULL) == SQLITE_OK) {
                                sqlite3_bind_text(ck, 1, game_id, -1, SQLITE_STATIC);
                                if (sqlite3_step(ck) == SQLITE_ROW) exists = 1;
                                sqlite3_finalize(ck);
                            }

                            if (!exists) {
                                sqlite3_stmt *ins;
                                const char *isql = "INSERT OR REPLACE INTO sports_outcomes "
                                    "(game_id, league, home_team, away_team, home_score, away_score, "
                                    "winner, status, game_time, collected_at) VALUES (?,?,?,?,?,?,?,?,?,?)";
                                if (sqlite3_prepare_v2(db, isql, -1, &ins, NULL) == SQLITE_OK) {
                                    sqlite3_bind_text(ins, 1, game_id, -1, SQLITE_STATIC);
                                    sqlite3_bind_text(ins, 2, LEAGUES[l].name, -1, SQLITE_STATIC);
                                    sqlite3_bind_text(ins, 3, hn, -1, SQLITE_STATIC);
                                    sqlite3_bind_text(ins, 4, an, -1, SQLITE_STATIC);
                                    sqlite3_bind_int(ins, 5, (int)hs);
                                    sqlite3_bind_int(ins, 6, (int)as);
                                    sqlite3_bind_text(ins, 7, (hs > as) ? hn : an, -1, SQLITE_STATIC);
                                    sqlite3_bind_text(ins, 8, "Final", -1, SQLITE_STATIC);
                                    sqlite3_bind_int64(ins, 9, (sqlite3_int64)game_ts);
                                    sqlite3_bind_int64(ins, 10, (sqlite3_int64)time(NULL));
                                    sqlite3_step(ins);
                                    sqlite3_finalize(ins);
                                    new_games++;
                                }
                            }
                            total_games++;
                        }
                    }
                    json_decref(j);
                }
            }
            printf("[BACKFILL] %s %d: %d total so far (%d new)\n",
                   LEAGUES[l].name, year, total_games, new_games);
        }
    }

    mkdir(OUT_DIR, 0755);
    json_dump_file(root, OUT_FILE, JSON_INDENT(2));
    printf("[BACKFILL] Total: %d games (%d new) -> %s\n", total_games, new_games, OUT_FILE);

    json_decref(root);
    sqlite3_close(db);
    curl_global_cleanup();
    return 0;
}
