/**
 * polymarket_profile.c — T134+T135: Polymarket Volume Profile & Liquidity Scoring
 *
 * Reads Polymarket data from timeline.db and computes:
 *   - Total volume across all prediction markets
 *   - Category volume breakdown (% per category)
 *   - Volume concentration index (Herfindahl)
 *   - Thin market ratio (events with <$1K volume)
 *   - Liquidity scoring per category
 *
 * Output: ~/.hermes/vp_cache/polymarket_profile.json
 * (Merged into market_feed.json by site_snapshot)
 *
 * Build: gcc -O2 -o polymarket_profile polymarket_profile.c -lsqlite3 -ljansson -lm
 * Usage: ./polymarket_profile
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <jansson.h>

#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define OUTPUT_FILE "~/.hermes/vp_cache/polymarket_profile.json"
#define MAX_CATEGORIES 16
#define MAX_CHAR 256

typedef struct {
    char name[MAX_CHAR];
    double total_volume;
    int market_count;
    double avg_liquidity;
    int thin_events;       /* events with volume < $1K */
    int event_count;       /* total events in this category */
} CategoryProfile;

static void expand_path(const char *in, char *out, size_t sz) {
    const char *h = getenv("HOME");
    if (!h) h = "/home/wubu2";
    if (in[0] == '~') snprintf(out, sz, "%s%s", h, in + 1);
    else snprintf(out, sz, "%s", in);
}

static double extract_double(const char *data, const char *field) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", field);
    const char *p = strstr(data, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    return atof(p);
}

int main(void) {
    printf("[POLYPROF] Polymarket Volume Profile\n");

    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "  DB open fail: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    /* Query all Polymarket event sources */
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT source, category, data, ts FROM timeline "
        "WHERE source LIKE 'polymarket_event_%' "
        "ORDER BY ts DESC LIMIT 10000";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "  Query prep fail: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    CategoryProfile cats[MAX_CATEGORIES];
    int n_cats = 0;
    double total_volume = 0;
    double total_liquidity = 0;
    int total_markets = 0;
    int total_events = 0;
    int thin_events_total = 0;
    char *ts_values = NULL;
    int ts_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        total_events++;
        const char *source = (const char *)sqlite3_column_text(stmt, 0);
        const char *category = (const char *)sqlite3_column_text(stmt, 1);
        const char *data = (const char *)sqlite3_column_text(stmt, 2);
        long long ts = sqlite3_column_int64(stmt, 3);

        if (!source || !category || !data) continue;

        double vol = extract_double(data, "volume");
        double liq = extract_double(data, "liquidity");
        int n_markets = (int)extract_double(data, "n_markets");

        const char *cat_name = category;
        if (strncmp(cat_name, "polymarket_", 11) == 0)
            cat_name = category + 11;
        if (strcmp(cat_name, "other") == 0) cat_name = "general";

        total_volume += vol;
        total_liquidity += liq;
        total_markets += n_markets;
        if (vol < 1000) thin_events_total++;

        int ci = -1;
        for (int i = 0; i < n_cats; i++) {
            if (strcmp(cats[i].name, cat_name) == 0) { ci = i; break; }
        }
        if (ci < 0 && n_cats < MAX_CATEGORIES) {
            ci = n_cats;
            strncpy(cats[ci].name, cat_name, MAX_CHAR-1);
            cats[ci].name[MAX_CHAR-1] = 0;
            cats[ci].total_volume = 0;
            cats[ci].market_count = 0;
            cats[ci].avg_liquidity = 0;
            cats[ci].thin_events = 0;
            cats[ci].event_count = 0;
            n_cats++;
        }

        if (ci >= 0) {
            cats[ci].total_volume += vol;
            cats[ci].market_count += n_markets;
            cats[ci].avg_liquidity += liq;
            cats[ci].event_count++;
            if (vol < 1000) cats[ci].thin_events++;
        }
    }
    sqlite3_finalize(stmt);

    /* Compute Herfindahl Index for volume concentration */
    double hhi = 0;
    for (int i = 0; i < n_cats; i++) {
        if (total_volume > 0) {
            double share = cats[i].total_volume / total_volume;
            hhi += share * share;
        }
        cats[i].avg_liquidity = cats[i].event_count > 0
            ? cats[i].avg_liquidity / cats[i].event_count : 0;
    }
    double concentration = hhi;

    /* Build output JSON */
    json_t *root = json_object();
    json_object_set_new(root, "poly_total_volume", json_real(total_volume));
    json_object_set_new(root, "poly_total_liquidity", json_real(total_liquidity));
    json_object_set_new(root, "poly_total_markets", json_integer(total_markets));
    json_object_set_new(root, "poly_total_events", json_integer(total_events));
    json_object_set_new(root, "poly_concentration_hhi", json_real(concentration));
    json_object_set_new(root, "poly_thin_event_ratio",
        json_real(total_events > 0 ? (double)thin_events_total / total_events : 0));
    json_object_set_new(root, "poly_thin_event_count", json_integer(thin_events_total));
    json_object_set_new(root, "poly_avg_volume_per_event",
        json_real(total_events > 0 ? total_volume / total_events : 0));
    json_object_set_new(root, "poly_avg_markets_per_event",
        json_real(total_events > 0 ? (double)total_markets / total_events : 0));

    /* Liquidity scoring */
    double thin_score = total_events > 0
        ? 100.0 * (1.0 - (double)thin_events_total / total_events) : 50;
    double vol_score = total_volume > 1000000 ? 100
        : (total_volume / 1000000.0) * 100;
    double conc_score = (1.0 - concentration) * 100;
    double liq_score = thin_score * 0.4 + vol_score * 0.3 + conc_score * 0.3;
    if (liq_score > 100) liq_score = 100;
    if (liq_score < 0) liq_score = 0;

    json_object_set_new(root, "poly_liquidity_score", json_real(liq_score));
    json_object_set_new(root, "poly_liquidity_zone",
        json_string(liq_score >= 70 ? "healthy" :
                    liq_score >= 40 ? "moderate" : "thin"));

    /* Category breakdown */
    json_t *cat_arr = json_array();
    for (int i = 0; i < n_cats; i++) {
        json_t *c = json_object();
        json_object_set_new(c, "name", json_string(cats[i].name));
        json_object_set_new(c, "volume", json_real(cats[i].total_volume));
        json_object_set_new(c, "event_count", json_integer(cats[i].event_count));
        json_object_set_new(c, "market_count", json_integer(cats[i].market_count));
        json_object_set_new(c, "volume_share",
            json_real(total_volume > 0 ? cats[i].total_volume / total_volume : 0));
        json_object_set_new(c, "avg_liquidity", json_real(cats[i].avg_liquidity));
        json_object_set_new(c, "thin_events", json_integer(cats[i].thin_events));
        json_object_set_new(c, "liquidity_score",
            json_real(cats[i].event_count > 0
                ? 100.0 * (1.0 - (double)cats[i].thin_events / cats[i].event_count)
                : 50));
        json_array_append_new(cat_arr, c);
    }
    json_object_set_new(root, "categories", cat_arr);

    char tb[64]; time_t now = time(NULL);
    strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    json_object_set_new(root, "fetched_at", json_string(tb));

    /* Write output */
    char out_path[512], dir_path[512];
    expand_path(OUTPUT_FILE, out_path, sizeof(out_path));
    expand_path("~/.hermes/vp_cache", dir_path, sizeof(dir_path));
    mkdir(dir_path, 0755);

    int fd = open(out_path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "  Write fail: %s\n", out_path);
        json_decref(root);
        sqlite3_close(db);
        return 1;
    }
    json_dumpfd(root, fd, JSON_INDENT(2));
    close(fd);
    json_decref(root);
    sqlite3_close(db);

    printf("[POLYPROF] %d events, %d markets, $%.0f total vol, liq=%.1f/100\n",
           total_events, total_markets, total_volume, liq_score);
    printf("  Categories: %d | Thin events: %d/%d (%.1f%%) | HHI: %.4f\n",
           n_cats, thin_events_total, total_events,
           total_events > 0 ? 100.0*thin_events_total/total_events : 0, concentration);
    return 0;
}
