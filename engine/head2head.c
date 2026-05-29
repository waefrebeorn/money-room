/**
 * head2head.c — T145: Historical Head-to-Head Records per Sport
 *
 * Queries timeline.db for historical matchups between teams,
 * computes head-to-head win/loss records for each pair.
 *
 * Output: ~/.hermes/vp_cache/head2head.json
 *
 * Build: gcc -O2 -o head2head head2head.c -lsqlite3 -ljansson -lm
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <jansson.h>

#define TL_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define OUTPUT_PATH "~/.hermes/vp_cache/head2head.json"
#define MAX_PAIRS 500

typedef struct {
    char home_team[64];
    char away_team[64];
    char league[32];
    int home_wins;
    int away_wins;
    int total_games;
    double avg_home_score;
    double avg_away_score;
    double avg_spread;
} H2HPair;

static void expand(const char *in, char *out, size_t sz) {
    const char *h = getenv("HOME");
    if (!h) h = "/home/wubu2";
    if (in[0] == '~') snprintf(out, sz, "%s%s", h, in + 1);
    else snprintf(out, sz, "%s", in);
}

static double extract_val(const char *data, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(data, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    return atof(p);
}

int main(void) {
    printf("[H2H] Head-to-Head Records\n");

    sqlite3 *db;
    if (sqlite3_open(TL_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "  DB open fail\n");
        return 1;
    }

    /* Query all sports entries */
    sqlite3_stmt *st;
    const char *sql = "SELECT source, data FROM timeline "
                      "WHERE source LIKE 'sports_%' "
                      "ORDER BY ts DESC LIMIT 5000";

    H2HPair pairs[MAX_PAIRS];
    int n_pairs = 0;
    int total_games = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "  Query fail\n");
        sqlite3_close(db);
        return 1;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *source = (const char *)sqlite3_column_text(st, 0);
        const char *data = (const char *)sqlite3_column_text(st, 1);
        if (!source || !data) continue;

        /* Extract sport name from source: sports_<sport> */
        const char *sn = source + 7;
        char league[32];
        int si = 0;
        while (sn[si] && sn[si] != '_' && si < 30) { league[si] = sn[si]; si++; }
        league[si] = 0;
        if (si == 0) continue;

        /* Extract teams */
        const char *ht = strstr(data, "\"home_team\":\"");
        const char *at = strstr(data, "\"away_team\":\"");
        if (!ht || !at) continue;
        ht += 13;
        at += 13;
        char home[64], away[64];
        const char *he = strchr(ht, '"');
        const char *ae = strchr(at, '"');
        if (!he || !ae) continue;
        int hl = (int)(he - ht); if (hl > 63) hl = 63;
        int al = (int)(ae - at); if (al > 63) al = 63;
        memcpy(home, ht, hl); home[hl] = 0;
        memcpy(away, at, al); away[al] = 0;

        double home_score = extract_val(data, "home_score");
        double away_score = extract_val(data, "away_score");
        double spread = extract_val(data, "spread");

        /* Skip games without scores */
        if (home_score == 0 && away_score == 0) continue;
        total_games++;

        /* Find or create pair */
        int pi = -1;
        for (int i = 0; i < n_pairs; i++) {
            if ((strcmp(pairs[i].home_team, home) == 0 && strcmp(pairs[i].away_team, away) == 0) ||
                (strcmp(pairs[i].home_team, away) == 0 && strcmp(pairs[i].away_team, home) == 0)) {
                pi = i;
                break;
            }
        }
        if (pi < 0 && n_pairs < MAX_PAIRS) {
            pi = n_pairs;
            /* Store in alphabetical order for consistent lookup */
            if (strcmp(home, away) < 0) {
                strncpy(pairs[pi].home_team, home, 63);
                strncpy(pairs[pi].away_team, away, 63);
            } else {
                strncpy(pairs[pi].home_team, away, 63);
                strncpy(pairs[pi].away_team, home, 63);
            }
            strncpy(pairs[pi].league, league, 31);
            pairs[pi].home_wins = 0;
            pairs[pi].away_wins = 0;
            pairs[pi].total_games = 0;
            pairs[pi].avg_home_score = 0;
            pairs[pi].avg_away_score = 0;
            pairs[pi].avg_spread = 0;
            n_pairs++;
        }

        if (pi >= 0) {
            /* Determine who was home in this actual game */
            int home_team_won = home_score > away_score;
            /* If the stored home_team matches this game's home_team */
            if (strcmp(pairs[pi].home_team, home) == 0) {
                if (home_team_won) pairs[pi].home_wins++;
                else pairs[pi].away_wins++;
            } else {
                if (home_team_won) pairs[pi].away_wins++;
                else pairs[pi].home_wins++;
            }
            pairs[pi].total_games++;
            pairs[pi].avg_home_score += home_score;
            pairs[pi].avg_away_score += away_score;
            pairs[pi].avg_spread += spread;
        }
    }
    sqlite3_finalize(st);

    /* Finalize averages */
    for (int i = 0; i < n_pairs; i++) {
        if (pairs[i].total_games > 0) {
            pairs[i].avg_home_score /= pairs[i].total_games;
            pairs[i].avg_away_score /= pairs[i].total_games;
            pairs[i].avg_spread /= pairs[i].total_games;
        }
    }

    /* Sort by total_games (most matchups first) */
    for (int i = 0; i < n_pairs - 1; i++) {
        for (int j = i + 1; j < n_pairs; j++) {
            if (pairs[j].total_games > pairs[i].total_games) {
                H2HPair t = pairs[i]; pairs[i] = pairs[j]; pairs[j] = t;
            }
        }
    }

    printf("[H2H] %d games → %d team pairings\n", total_games, n_pairs);

    /* Build output JSON */
    json_t *root = json_object();
    json_object_set_new(root, "total_games", json_integer(total_games));
    json_object_set_new(root, "total_pairs", json_integer(n_pairs));

    json_t *arr = json_array();
    int top = n_pairs > 50 ? 50 : n_pairs;
    for (int i = 0; i < top; i++) {
        json_t *p = json_object();
        json_object_set_new(p, "home_team", json_string(pairs[i].home_team));
        json_object_set_new(p, "away_team", json_string(pairs[i].away_team));
        json_object_set_new(p, "league", json_string(pairs[i].league));
        json_object_set_new(p, "home_wins", json_integer(pairs[i].home_wins));
        json_object_set_new(p, "away_wins", json_integer(pairs[i].away_wins));
        json_object_set_new(p, "total_games", json_integer(pairs[i].total_games));
        json_object_set_new(p, "home_win_pct",
            json_real(pairs[i].total_games > 0
                ? (double)pairs[i].home_wins / pairs[i].total_games : 0));
        json_object_set_new(p, "avg_home_score", json_real(pairs[i].avg_home_score));
        json_object_set_new(p, "avg_away_score", json_real(pairs[i].avg_away_score));
        json_object_set_new(p, "avg_spread", json_real(pairs[i].avg_spread));
        json_array_append_new(arr, p);
    }
    json_object_set_new(root, "top_matchups", arr);

    char tb[64]; time_t now = time(NULL);
    strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    json_object_set_new(root, "fetched_at", json_string(tb));

    char out[512], dir[512];
    expand(OUTPUT_PATH, out, sizeof(out));
    expand("~/.hermes/vp_cache", dir, sizeof(dir));
    mkdir(dir, 0755);

    json_dumpfd(root, open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644), JSON_INDENT(2));
    json_decref(root);
    sqlite3_close(db);

    printf("[H2H] Output → %s\n", out);
    printf("  Top matchup: %s vs %s (%d games)\n",
           pairs[0].home_team, pairs[0].away_team, pairs[0].total_games);

    return 0;
}
