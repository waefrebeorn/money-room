/**
 * sports_collector.c — Enhanced sports data collector
 * Fetches game results + player stats + team stats from ESPN free API
 * 10 leagues: NBA, NFL, MLB, NHL, EPL, LaLiga, SerieA, MLS, NCAAF, NCAAB
 *
 * Compile: gcc -O2 -Wall -o sports_collector sports_collector.c -lcurl -ljansson -lsqlite3 -lm
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
#include <sqlite3.h>
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
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    h = curl_slist_append(h, "Accept: application/json");
    h = curl_slist_append(h, "Referer: https://www.espn.com/");
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

static double safe_num(const json_t *o, const char *k) {
    if (!o) return 0;
    json_t *v = json_object_get(o, k);
    if (!v) return 0;
    if (json_is_real(v)) return json_real_value(v);
    if (json_is_integer(v)) return (double)json_integer_value(v);
    if (json_is_string(v)) return atof(json_string_value(v));
    return 0;
}

static const char *safe_str(const json_t *o, const char *k) {
    if (!o) return "";
    json_t *v = json_object_get(o, k);
    if (!v || !json_is_string(v)) return "";
    return json_string_value(v);
}

// Skip by ref: competitors[0|1].athletes[].items[] -> player stats
static void extract_player_stats(json_t *comp, json_t *game_json, const char *side) {
    if (!comp) return;
    json_t *athletes = json_object_get(comp, "athletes");
    if (!athletes || !json_is_array(athletes)) return;
    
    size_t n = json_array_size(athletes);
    if (n == 0) return;
    
    json_t *players = json_array();
    
    for (size_t i = 0; i < n && i < 15; i++) {
        json_t *ath = json_array_get(athletes, i);
        if (!ath) continue;
        
        json_t *athlete = json_object_get(ath, "athlete");
        if (!athlete) continue;
        
        const char *name = safe_str(athlete, "fullName");
        if (!name[0]) continue;
        
        const char *pos = safe_str(athlete, "position");
        
        json_t *stats = json_object_get(ath, "stats");
        json_t *player = json_pack("{s:s,s:s}", "name", name, "position", pos);
        
        if (stats && json_is_array(stats)) {
            for (size_t s = 0; s < json_array_size(stats); s++) {
                json_t *stat = json_array_get(stats, s);
                const char *sn = safe_str(stat, "name");
                const char *sv = safe_str(stat, "value");
                if (sn[0]) json_object_set_new(player, sn, json_string(sv));
            }
        }
        json_array_append_new(players, player);
    }
    json_object_set_new(game_json, side, players);
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

// --- DB Writer ---
#define DB_PATH "/home/wubu2/.hermes/pm_logs/outcomes.db"

static int write_game_to_db(const json_t *game) {
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "  DB open failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    const char *sql =
        "INSERT OR REPLACE INTO sports_outcomes "
        "(game_id, league, home_team, away_team, home_score, away_score, "
         "winner, spread, over_under, status, game_time, collected_at) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "  Prepare failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    // Game ID from league + teams
    const char *league = json_string_value(json_object_get(game, "league"));
    const char *home = json_string_value(json_object_get(game, "home_team"));
    const char *away = json_string_value(json_object_get(game, "away_team"));
    const char *date = json_string_value(json_object_get(game, "date"));
    char game_id[256];
    snprintf(game_id, sizeof(game_id), "%s_%s_%s_%s", league ? league : "?", date ? date : "?", away ? away : "?", home ? home : "?");

    double home_score = json_number_value(json_object_get(game, "home_score"));
    double away_score = json_number_value(json_object_get(game, "away_score"));

    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, league, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, home, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, away, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, (int)home_score);
    sqlite3_bind_int(st, 6, (int)away_score);
    sqlite3_bind_text(st, 7, (home_score > away_score) ? home : away, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 8, json_number_value(json_object_get(game, "spread")));
    sqlite3_bind_double(st, 9, json_number_value(json_object_get(game, "over_under")));
    sqlite3_bind_text(st, 10, json_string_value(json_object_get(game, "status")), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 11, (sqlite3_int64)json_number_value(json_object_get(game, "game_time")));
    sqlite3_bind_int64(st, 12, (sqlite3_int64)time(NULL));

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}

// Also write to timeline.db for unified view
#define TL_PATH "/home/wubu2/money-room/engine/timeline.db"

static int write_to_timeline(const json_t *game) {
    sqlite3 *db;
    if (sqlite3_open(TL_PATH, &db) != SQLITE_OK) return -1;

    // Ensure sports_data table exists
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS sports_data ("
        "ts INTEGER NOT NULL, league TEXT, home_team TEXT, away_team TEXT, "
        "home_score INTEGER, away_score INTEGER, spread REAL, "
        "winner TEXT, source TEXT DEFAULT 'espn', "
        "PRIMARY KEY (ts, league, home_team))",
        NULL, NULL, NULL);

    const char *league = json_string_value(json_object_get(game, "league"));
    const char *home = json_string_value(json_object_get(game, "home_team"));
    const char *away = json_string_value(json_object_get(game, "away_team"));
    double home_score = json_number_value(json_object_get(game, "home_score"));
    double away_score = json_number_value(json_object_get(game, "away_score"));
    time_t ts = (time_t)json_number_value(json_object_get(game, "game_time"));

    const char *sql = "INSERT OR REPLACE INTO sports_data "
        "(ts, league, home_team, away_team, home_score, away_score, spread, winner) "
        "VALUES (?,?,?,?,?,?,?,?)";

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
    sqlite3_bind_text(st, 2, league, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, home, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, away, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 5, (int)home_score);
    sqlite3_bind_int(st, 6, (int)away_score);
    sqlite3_bind_double(st, 7, json_number_value(json_object_get(game, "spread")));
    sqlite3_bind_text(st, 8, (home_score > away_score) ? home : away, -1, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}

int main(int argc, char **argv) {
    int days_back = 30;
    const char *filter_league = NULL;
    int include_players = 1;
    int write_db = 1;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--days") == 0 && i+1 < argc) days_back = atoi(argv[++i]);
        else if (strcmp(argv[i], "--league") == 0 && i+1 < argc) filter_league = argv[++i];
        else if (strcmp(argv[i], "--no-players") == 0) include_players = 0;
        else if (strcmp(argv[i], "--no-db") == 0) write_db = 0;
        else if (strcmp(argv[i], "--help") == 0 || strncmp(argv[i], "-h", 2) == 0) {
            printf("Usage: %s [--days N] [--league nba|nfl|...] [--no-players] [--no-db]\n", argv[0]);
            printf("Leagues: nba,nfl,mlb,nhl,epl,laliga,seriea,mls,ncaaf,ncaab\n");
            return 0;
        }
    }
    
    curl_global_init(CURL_GLOBAL_ALL);
    json_t *root = json_array();
    int total_games = 0;
    
    time_t now = time(NULL);
    
    for (int l = 0; l < N_LEAGUES; l++) {
        if (filter_league && strcasecmp(filter_league, LEAGUES[l].name) != 0) continue;
        
        printf("[SPORTS] %s... ", LEAGUES[l].name);
        fflush(stdout);
        int lg_games = 0;
        
        for (int d = 0; d < days_back; d++) {
            time_t day_ts = now - (time_t)d * 86400;
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
                size_t n = json_array_size(events);
                for (size_t e = 0; e < n; e++) {
                    json_t *ev = json_array_get(events, e);
                    if (!ev) continue;
                    
                    const char *status = safe_str(
                        json_object_get(json_object_get(ev, "status"), "type"), "description");
                    if (strcmp(status, "Final") != 0) continue;
                    
                    json_t *comp_a = json_object_get(ev, "competitions");
                    json_t *comp = (comp_a && json_array_size(comp_a) > 0)
                        ? json_array_get(comp_a, 0) : NULL;
                    if (!comp) continue;
                    
                    json_t *compets = json_object_get(comp, "competitors");
                    if (!compets || json_array_size(compets) < 2) continue;
                    
                    // Need at least valid score data
                    json_t *home = json_array_get(compets, 0);
                    json_t *away = json_array_get(compets, 1);
                    if (!home || !away) continue;
                    
                    // Determine home/away based on homeAway field
                    const char *ha_home = safe_str(home, "homeAway");
                    if (strcmp(ha_home, "away") == 0) {
                        json_t *tmp = home; home = away; away = tmp;
                    }
                    
                    double home_score = safe_num(home, "score");
                    double away_score = safe_num(away, "score");
                    const char *home_name = safe_str(
                        json_object_get(home, "team"), "displayName");
                    const char *away_name = safe_str(
                        json_object_get(away, "team"), "displayName");
                    const char *home_abbr = safe_str(
                        json_object_get(home, "team"), "abbreviation");
                    const char *away_abbr = safe_str(
                        json_object_get(away, "team"), "abbreviation");
                    
                    // Venue info
                    const char *venue = safe_str(
                        json_object_get(comp, "venue"), "fullName");
                    
                    // Odds
                    json_t *odds_a = json_object_get(comp, "odds");
                    double spread = 0, over_under = 0;
                    const char *spread_line = "";
                    if (odds_a && json_array_size(odds_a) > 0) {
                        json_t *odd = json_array_get(odds_a, 0);
                        spread = safe_num(odd, "spread");
                        over_under = safe_num(odd, "overUnder");
                        // Get spread line from pointSpread
                        json_t *ps = json_object_get(odd, "pointSpread");
                        if (ps) {
                            json_t *hc = json_object_get(ps, "home");
                            if (hc) {
                                json_t *cls = json_object_get(hc, "close");
                                spread_line = cls ? safe_str(cls, "line") : safe_str(hc, "line");
                            }
                        }
                    }
                    
                    // Attendance
                    double attendance = safe_num(comp, "attendance");
                    
                    // Build game entry
                    char game_label[256];
                    snprintf(game_label, sizeof(game_label), "%s vs %s", away_name, home_name);
                    json_t *game = json_pack(
                        "{s:s, s:s, s:s, s:s, s:s, s:s, s:s,"
                         "s:f, s:f, s:f, s:f, s:s, s:s,"
                         "s:f, s:s, s:f, s:s, s:f, s:o}",
                        "league", LEAGUES[l].name,
                        "sport", LEAGUES[l].sport,
                        "game", game_label,
                        "home_team", home_name,
                        "away_team", away_name,
                        "home_abbr", home_abbr,
                        "away_abbr", away_abbr,
                        "home_score", home_score,
                        "away_score", away_score,
                        "spread", spread,
                        "over_under", over_under,
                        "spread_line", spread_line,
                        "venue", venue,
                        "game_time", (double)day_ts,
                        "date", date_str,
                        "attendance", attendance,
                        "status", status,
                        "outcome", (home_score > away_score) ? 1.0 : 0.0,
                        "features", json_array()
                    );
                    
                    // Add 20 features for the trainer
                    json_t *feats = json_object_get(game, "features");
                    double h_win_pct = 0.5, a_win_pct = 0.5;
                    // Spread-based features
                    double spread_norm = spread != 0 ? spread / 20.0 : 0;
                    double ou_norm = (over_under > 0) ? over_under / 300.0 : 0.5;
                    double score_diff = home_score - away_score;
                    double total_score = home_score + away_score;
                    
                    json_array_append_new(feats, json_real(spread_norm));
                    json_array_append_new(feats, json_real(ou_norm));
                    json_array_append_new(feats, json_real(home_score / 150.0));
                    json_array_append_new(feats, json_real(away_score / 150.0));
                    json_array_append_new(feats, json_real(score_diff / 50.0));
                    json_array_append_new(feats, json_real(total_score / 300.0));
                    json_array_append_new(feats, json_real(attendance / 100000.0));
                    json_array_append_new(feats, json_real(h_win_pct));
                    json_array_append_new(feats, json_real(a_win_pct));
                    json_array_append_new(feats, json_real(1.0)); // at home advantage
                    // Placeholder features (will be filled by sentiment/context)
                    for (int f = 10; f < 20; f++)
                        json_array_append_new(feats, json_real(0.5));
                    
                    // Player stats (optional, makes response larger)
                    if (include_players) {
                        json_t *hp = json_object();
                        json_t *ap = json_object();
                        extract_player_stats(home, hp, "players");
                        extract_player_stats(away, ap, "players");
                        if (json_object_get(hp, "players"))
                            json_object_set(game, "home_players", json_object_get(hp, "players"));
                        if (json_object_get(ap, "players"))
                            json_object_set(game, "away_players", json_object_get(ap, "players"));
                        json_decref(hp); json_decref(ap);
                    }
                    
                    json_array_append_new(root, game);
                    lg_games++;
                }
            }
            json_decref(j);
        }
        total_games += lg_games;
        printf("%d games\n", lg_games);
    }
    
    // Write output
    mkdir(OUT_DIR, 0755);
    json_dump_file(root, OUT_FILE, JSON_INDENT(2));
    printf("[SPORTS] Total: %d games across %d leagues -> %s\n",
           total_games, N_LEAGUES, OUT_FILE);
    
    // Write to databases
    if (write_db) {
        size_t idx;
        json_t *game;
        int db_ok = 0, db_fail = 0;
        json_array_foreach(root, idx, game) {
            if (write_game_to_db(game) == 0 && write_to_timeline(game) == 0)
                db_ok++;
            else
                db_fail++;
        }
        printf("[SPORTS] DB: %d written, %d failed\n", db_ok, db_fail);
    }
    
    json_decref(root);
    curl_global_cleanup();
    return 0;
}
