/*
 * orderbook_depth.c — T34/E15: Order Book Depth + Archive
 *
 * Fetches Level 2 order book from Coinbase Pro (public, no auth).
 * Computes depth features: imbalance, cumulative depth, wall detection, spread.
 * Archives historical snapshots in SQLite with 90-day retention.
 *
 * Features (F77-F80):
 *   F77: Order book imbalance — bid vol / (bid+ask vol) at top 10 levels (0-1)
 *   F78: Depth volume ratio — bid depth in 0.5% / ask depth in 0.5% (0-1 norm)
 *   F79: Wall concentration — largest single level / top-10 total (0-1)
 *   F80: Bid-ask spread — (best_ask - best_bid) / mid * 10000 (bps, normalized)
 *
 * Build: gcc -O3 -march=native orderbook_depth.c -o orderbook_depth -lcurl -ljansson -lsqlite3 -lm
 * Usage: ./orderbook_depth fetch      — fetch and store snapshot
 *        ./orderbook_depth features   — print current features JSON
 *        ./orderbook_depth book       — print raw order book summary
 *        ./orderbook_depth archive     — archive summary (count, date range, avg fields)
 *        ./orderbook_depth trend <h>   — compare current vs N-hour ago avg (default 24h)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <time.h>

#define CACHE_DIR   "/home/wubu2/.hermes/orderbook_cache"
#define DB_PATH     CACHE_DIR "/orderbook.db"
#define FEAT_PATH   CACHE_DIR "/orderbook_features.json"
#define API_URL     "https://api.exchange.coinbase.com/products/BTC-USD/book?level=2"
#define TOP_N       10
#define DEPTH_PCT   0.5

typedef struct { char *data; size_t len; } HttpBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    HttpBuf *b = (HttpBuf*)user;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    memcpy(p + b->len, ptr, n);
    b->data = p;
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static sqlite3 *open_db(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return NULL;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    const char *sql =
        "CREATE TABLE IF NOT EXISTS snapshots ("
        "  ts INTEGER PRIMARY KEY,"
        "  best_bid REAL, best_ask REAL,"
        "  bid_vol_10 REAL, ask_vol_10 REAL,"
        "  imbalance REAL, depth_ratio REAL,"
        "  wall_conc REAL, spread_bps REAL,"
        "  bid_depth_05 REAL, ask_depth_05 REAL"
        ");";
    char *err = NULL;
    sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) { sqlite3_free(err); }
    return db;
}

/* ─── Write features JSON ─── */
static int write_features(double imbalance, double depth_ratio, double wall_conc, 
                           double spread_bps, double best_bid, double best_ask,
                           double bid_vol_10, double ask_vol_10,
                           double bid_depth_05, double ask_depth_05) {
    json_t *root = json_object();
    json_object_set_new(root, "fetch_time", json_integer((long long)time(NULL)));

    /* Raw values */
    json_object_set_new(root, "best_bid", json_real(best_bid));
    json_object_set_new(root, "best_ask", json_real(best_ask));
    json_object_set_new(root, "spread_bps", json_real(spread_bps));
    json_object_set_new(root, "bid_vol_10", json_real(bid_vol_10));
    json_object_set_new(root, "ask_vol_10", json_real(ask_vol_10));
    json_object_set_new(root, "bid_depth_05", json_real(bid_depth_05));
    json_object_set_new(root, "ask_depth_05", json_real(ask_depth_05));

    /* Normalized features [0,1] */
    json_object_set_new(root, "ob_imbalance_norm", json_real(imbalance));
    json_object_set_new(root, "ob_depth_ratio_norm", json_real(depth_ratio));
    json_object_set_new(root, "ob_wall_conc_norm", json_real(wall_conc));
    json_object_set_new(root, "ob_spread_norm", json_real(fmin(spread_bps / 100.0, 1.0)));

    char *out = json_dumps(root, JSON_INDENT(2));
    FILE *f = fopen(FEAT_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", out);
        fclose(f);
        printf("[orderbook] Written to %s\n", FEAT_PATH);
    }
    free(out);
    json_decref(root);
    return 0;
}

/* ─── Fetch and compute ─── */
static int cmd_fetch(void) {
    CURL *curl = curl_easy_init();
    if (!curl) { fprintf(stderr, "curl init failed\n"); return 1; }

    HttpBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, API_URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "orderbook-depth/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || !buf.data) {
        fprintf(stderr, "HTTP error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return 1;
    }

    json_error_t err;
    json_t *root = json_loads(buf.data, 0, &err);
    free(buf.data);
    if (!root) {
        fprintf(stderr, "JSON parse error: %s\n", err.text);
        return 1;
    }

    json_t *jbids = json_object_get(root, "bids");
    json_t *jasks = json_object_get(root, "asks");
    if (!json_is_array(jbids) || !json_is_array(jasks)) {
        fprintf(stderr, "Invalid response format\n");
        json_decref(root);
        return 1;
    }

    size_t n_bids = json_array_size(jbids);
    size_t n_asks = json_array_size(jasks);
    printf("[orderbook] Levels: %zu bids, %zu asks\n", n_bids, n_asks);

    /* Compute top-N bid/ask volumes */
    double bid_vol_10 = 0, ask_vol_10 = 0;
    double max_bid_level = 0, max_ask_level = 0;
    size_t top_n_bids = n_bids < TOP_N ? n_bids : TOP_N;
    size_t top_n_asks = n_asks < TOP_N ? n_asks : TOP_N;

    for (size_t i = 0; i < top_n_bids; i++) {
        json_t *entry = json_array_get(jbids, i);
        if (!entry) continue;
        json_t *jprice = json_array_get(entry, 0);
        json_t *jsize = json_array_get(entry, 1);
        if (!json_is_string(jprice) || !json_is_string(jsize)) continue;
        double vol = atof(json_string_value(jsize));
        bid_vol_10 += vol;
        if (vol > max_bid_level) max_bid_level = vol;
    }

    for (size_t i = 0; i < top_n_asks; i++) {
        json_t *entry = json_array_get(jasks, i);
        if (!entry) continue;
        json_t *jprice = json_array_get(entry, 0);
        json_t *jsize = json_array_get(entry, 1);
        if (!json_is_string(jprice) || !json_is_string(jsize)) continue;
        double vol = atof(json_string_value(jsize));
        ask_vol_10 += vol;
        if (vol > max_ask_level) max_ask_level = vol;
    }

    /* Best bid/ask */
    double best_bid = 0, best_ask = 0;
    json_t *first_bid = json_array_get(jbids, 0);
    json_t *first_ask = json_array_get(jasks, 0);
    if (first_bid) {
        json_t *jp = json_array_get(first_bid, 0);
        if (json_is_string(jp)) best_bid = atof(json_string_value(jp));
    }
    if (first_ask) {
        json_t *jp = json_array_get(first_ask, 0);
        if (json_is_string(jp)) best_ask = atof(json_string_value(jp));
    }

    double mid = (best_bid + best_ask) / 2.0;
    double spread_bps = mid > 0 ? (best_ask - best_bid) / mid * 10000.0 : 0;

    /* F79: Order book imbalance (0-1) */
    double total_vol = bid_vol_10 + ask_vol_10;
    double imbalance = total_vol > 0 ? bid_vol_10 / total_vol : 0.5;

    /* F80: Depth volume ratio — bid depth / ask depth within 0.5% of mid */
    double bid_depth_05 = 0, ask_depth_05 = 0;
    double price_low = mid * (1 - DEPTH_PCT / 100.0);
    double price_high = mid * (1 + DEPTH_PCT / 100.0);

    for (size_t i = 0; i < n_bids; i++) {
        json_t *entry = json_array_get(jbids, i);
        if (!entry) continue;
        json_t *jp = json_array_get(entry, 0);
        json_t *js = json_array_get(entry, 1);
        if (!json_is_string(jp) || !json_is_string(js)) continue;
        double price = atof(json_string_value(jp));
        if (price < price_low) break; /* bids sorted descending */
        bid_depth_05 += atof(json_string_value(js));
    }
    for (size_t i = 0; i < n_asks; i++) {
        json_t *entry = json_array_get(jasks, i);
        if (!entry) continue;
        json_t *jp = json_array_get(entry, 0);
        json_t *js = json_array_get(entry, 1);
        if (!json_is_string(jp) || !json_is_string(js)) continue;
        double price = atof(json_string_value(jp));
        if (price > price_high) break; /* asks sorted ascending */
        ask_depth_05 += atof(json_string_value(js));
    }
    double depth_ratio = (bid_depth_05 + ask_depth_05) > 0 
        ? bid_depth_05 / (bid_depth_05 + ask_depth_05) : 0.5;

    /* F81: Wall concentration — largest single level / top-10 total */
    double max_level = max_bid_level > max_ask_level ? max_bid_level : max_ask_level;
    double wall_conc = total_vol > 0 ? fmin(max_level / total_vol, 1.0) : 0;

    printf("[orderbook] Mid=%.2f Spread=%.4fbps\n", mid, spread_bps);
    printf("[orderbook] F79(imbalance)=%.4f F80(depth_ratio)=%.4f F81(wall)=%.4f F82(spread)=%.4f\n",
           imbalance, depth_ratio, wall_conc, spread_bps);
    printf("[orderbook] Top-10: bid_vol=%.4f ask_vol=%.4f\n", bid_vol_10, ask_vol_10);
    printf("[orderbook] Depth 0.5%%: bid=%.4f ask=%.4f\n", bid_depth_05, ask_depth_05);

    /* Store in SQLite */
    sqlite3 *db = open_db();
    if (db) {
        sqlite3_stmt *stmt = NULL;
        sqlite3_prepare_v2(db,
            "INSERT INTO snapshots (ts, best_bid, best_ask, bid_vol_10, ask_vol_10, "
            "imbalance, depth_ratio, wall_conc, spread_bps, bid_depth_05, ask_depth_05) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?);",
            -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, (long long)time(NULL));
        sqlite3_bind_double(stmt, 2, best_bid);
        sqlite3_bind_double(stmt, 3, best_ask);
        sqlite3_bind_double(stmt, 4, bid_vol_10);
        sqlite3_bind_double(stmt, 5, ask_vol_10);
        sqlite3_bind_double(stmt, 6, imbalance);
        sqlite3_bind_double(stmt, 7, depth_ratio);
        sqlite3_bind_double(stmt, 8, wall_conc);
        sqlite3_bind_double(stmt, 9, spread_bps);
        sqlite3_bind_double(stmt, 10, bid_depth_05);
        sqlite3_bind_double(stmt, 11, ask_depth_05);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        printf("[orderbook] Stored in SQLite\n");
    }

    /* Write features JSON */
    write_features(imbalance, depth_ratio, wall_conc, spread_bps,
                   best_bid, best_ask, bid_vol_10, ask_vol_10,
                   bid_depth_05, ask_depth_05);

    json_decref(root);
    return 0;
}

/* ─── Print features JSON ─── */
static int cmd_features(void) {
    FILE *f = fopen(FEAT_PATH, "r");
    if (!f) { printf("No data. Run 'fetch' first.\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *b = malloc(sz + 1);
    fread(b, 1, sz, f);
    b[sz] = '\0';
    fclose(f);
    printf("%s\n", b);
    free(b);
    return 0;
}

/* ─── Print raw book summary ─── */
static int cmd_book(void) {
    CURL *curl = curl_easy_init();
    if (!curl) return 1;
    HttpBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, API_URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "orderbook/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || !buf.data) { free(buf.data); return 1; }

    json_t *root = json_loads(buf.data, 0, NULL);
    free(buf.data);
    if (!root) return 1;

    json_t *jbids = json_object_get(root, "bids");
    json_t *jasks = json_object_get(root, "asks");
    size_t n_b = json_array_size(jbids);
    size_t n_a = json_array_size(jasks);
    size_t show = 5;

    printf("Order Book: BTC-USD (%zu bids, %zu asks)\n\n", n_b, n_a);
    printf("Asks (top %zu):\n", show);
    double ask_sum = 0;
    for (size_t i = 0; i < show && i < n_a; i++) {
        json_t *e = json_array_get(jasks, i);
        const char *p = json_string_value(json_array_get(e, 0));
        const char *s = json_string_value(json_array_get(e, 1));
        int oc = json_integer_value(json_array_get(e, 2));
        double vol = atof(s);
        ask_sum += vol;
        printf("  $%s x %s (%d orders)\n", p, s, oc);
    }
    printf("  ...\n");
    printf("  Total top-%zu ask vol: %.4f\n\n", show, ask_sum);

    printf("Bids (top %zu):\n", show);
    double bid_sum = 0;
    for (size_t i = 0; i < show && i < n_b; i++) {
        json_t *e = json_array_get(jbids, i);
        const char *p = json_string_value(json_array_get(e, 0));
        const char *s = json_string_value(json_array_get(e, 1));
        int oc = json_integer_value(json_array_get(e, 2));
        double vol = atof(s);
        bid_sum += vol;
        printf("  $%s x %s (%d orders)\n", p, s, oc);
    }
    printf("  ...\n");
    printf("  Total top-%zu bid vol: %.4f\n\n", show, bid_sum);

    json_t *fb = json_array_get(jbids, 0);
    json_t *fa = json_array_get(jasks, 0);
    double bb = fb ? atof(json_string_value(json_array_get(fb, 0))) : 0;
    double ba = fa ? atof(json_string_value(json_array_get(fa, 0))) : 0;
    double mid = (bb + ba) / 2;
    printf("Best bid: $%.2f  Best ask: $%.2f  Mid: $%.2f\n", bb, ba, mid);
    printf("Spread: %.4f bps  (%.4f%%)\n", (ba-bb)/mid*10000, (ba-bb)/mid*100);
    printf("Imbalance (top-5): bid %.1f%% / ask %.1f%%\n",
           bid_sum/(bid_sum+ask_sum)*100, ask_sum/(bid_sum+ask_sum)*100);
    printf("Ratio: %.4f\n", (bid_sum+ask_sum) > 0 ? bid_sum/(bid_sum+ask_sum) : 0.5);

    json_decref(root);
    return 0;
}

/* ─── Archive summary (E15) ─── */
static int cmd_archive(void) {
    sqlite3 *db = open_db();
    if (!db) { printf("No database found.\\n"); return 1; }

    sqlite3_stmt *stmt = NULL;

    /* Count, range */
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*), COALESCE(MIN(ts),0), COALESCE(MAX(ts),0) FROM snapshots;",
        -1, &stmt, NULL);
    sqlite3_step(stmt);
    int count = sqlite3_column_int(stmt, 0);
    time_t ts_min = (time_t)sqlite3_column_int64(stmt, 1);
    time_t ts_max = (time_t)sqlite3_column_int64(stmt, 2);
    sqlite3_finalize(stmt);

    if (count == 0) {
        printf("[archive] No snapshots in database.\\n");
        sqlite3_close(db);
        return 0;
    }

    printf("=== Order Book Archive ===\\n");
    printf("Snapshots:    %d\\n", count);
    printf("Date range:   %s  →  %s\\n",
           ts_min ? ctime(&ts_min) : "N/A",
           ts_max ? ctime(&ts_max) : "N/A");
    double hours = (ts_max - ts_min) / 3600.0;
    double per_day = hours > 0 ? (double)count / (hours / 24.0) : 0;
    printf("Coverage:     %.1f hours (%.1f snapshots/day)\\n", hours, per_day);

    /* Averages */
    sqlite3_prepare_v2(db,
        "SELECT AVG(imbalance), AVG(depth_ratio), AVG(wall_conc), "
        "AVG(spread_bps), AVG(bid_vol_10), AVG(ask_vol_10), "
        "AVG(bid_depth_05), AVG(ask_depth_05) FROM snapshots;",
        -1, &stmt, NULL);
    sqlite3_step(stmt);
    double avg_imb   = sqlite3_column_double(stmt, 0);
    double avg_dr    = sqlite3_column_double(stmt, 1);
    double avg_wall  = sqlite3_column_double(stmt, 2);
    double avg_sprd  = sqlite3_column_double(stmt, 3);
    double avg_bv10  = sqlite3_column_double(stmt, 4);
    double avg_av10  = sqlite3_column_double(stmt, 5);
    double avg_bd05  = sqlite3_column_double(stmt, 6);
    double avg_ad05  = sqlite3_column_double(stmt, 7);
    sqlite3_finalize(stmt);

    /* Std dev */
    sqlite3_prepare_v2(db,
        "SELECT AVG(imbalance*imbalance), AVG(spread_bps*spread_bps) FROM snapshots;",
        -1, &stmt, NULL);
    sqlite3_step(stmt);
    double avg_imb2 = sqlite3_column_double(stmt, 0);
    double avg_sprd2 = sqlite3_column_double(stmt, 1);
    sqlite3_finalize(stmt);

    double std_imb = sqrt(fmax(avg_imb2 - avg_imb*avg_imb, 0));
    double std_sprd = sqrt(fmax(avg_sprd2 - avg_sprd*avg_sprd, 0));

    printf("\\n=== Averages (all time) ===\\n");
    printf("Imbalance:    %.4f ± %.4f  (bid fraction, >0.5 = bid-heavy)\\n", avg_imb, std_imb);
    printf("Depth ratio:  %.4f  (bid/(bid+ask) within 0.5%% of mid)\\n", avg_dr);
    printf("Wall conc:    %.4f  (max level / top-10 total, %%)\\n", avg_wall);
    printf("Spread:       %.4f ± %.4f bps\\n", avg_sprd, std_sprd);
    printf("Top-10 bid:   %.2f BTC\\n", avg_bv10);
    printf("Top-10 ask:   %.2f BTC\\n", avg_av10);
    printf("Depth 0.5%% bid:  %.2f BTC\\n", avg_bd05);
    printf("Depth 0.5%% ask:  %.2f BTC\\n", avg_ad05);

    /* Most recent snapshot */
    sqlite3_prepare_v2(db,
        "SELECT imbalance, depth_ratio, wall_conc, spread_bps FROM snapshots "
        "ORDER BY ts DESC LIMIT 1;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    double cur_imb  = sqlite3_column_double(stmt, 0);
    double cur_dr   = sqlite3_column_double(stmt, 1);
    double cur_wall = sqlite3_column_double(stmt, 2);
    double cur_sprd = sqlite3_column_double(stmt, 3);
    sqlite3_finalize(stmt);

    printf("\\n=== Current vs Average ===\\n");
    printf("Imbalance:    %.4f (avg %.4f) — %s bid-heavy than avg\n",
           cur_imb, avg_imb, cur_imb > avg_imb ? "MORE" : "LESS");
    printf("Depth ratio:  %.4f (avg %.4f)\\n", cur_dr, avg_dr);
    printf("Wall:         %.4f (avg %.4f)\\n", cur_wall, avg_wall);
    printf("Spread:       %.4f (avg %.4f) bps\\n", cur_sprd, avg_sprd);

    sqlite3_close(db);
    return 0;
}

/* ─── Trend: compare current vs N-hour ago avg ─── */
static int cmd_trend(int hours) {
    sqlite3 *db = open_db();
    if (!db) { printf("No database found.\\n"); return 1; }

    time_t now = time(NULL);
    time_t cutoff = now - hours * 3600;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "SELECT AVG(imbalance), AVG(depth_ratio), AVG(wall_conc), AVG(spread_bps), "
        "AVG(bid_vol_10), AVG(ask_vol_10), AVG(bid_depth_05), AVG(ask_depth_05), "
        "COUNT(*) FROM snapshots WHERE ts >= ?;",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (long long)cutoff);
    sqlite3_step(stmt);
    double avg_imb   = sqlite3_column_double(stmt, 0);
    double avg_dr    = sqlite3_column_double(stmt, 1);
    double avg_wall  = sqlite3_column_double(stmt, 2);
    double avg_sprd  = sqlite3_column_double(stmt, 3);
    double avg_bv10  = sqlite3_column_double(stmt, 4);
    double avg_av10  = sqlite3_column_double(stmt, 5);
    double avg_bd05  = sqlite3_column_double(stmt, 6);
    double avg_ad05  = sqlite3_column_double(stmt, 7);
    int count = sqlite3_column_int(stmt, 8);
    sqlite3_finalize(stmt);

    if (count == 0) {
        printf("[trend] No data in last %d hours.\\n", hours);
        sqlite3_close(db);
        return 0;
    }

    /* Current snapshot */
    sqlite3_prepare_v2(db,
        "SELECT imbalance, depth_ratio, wall_conc, spread_bps, "
        "bid_vol_10, ask_vol_10, bid_depth_05, ask_depth_05 "
        "FROM snapshots ORDER BY ts DESC LIMIT 1;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    double cur_imb  = sqlite3_column_double(stmt, 0);
    double cur_dr   = sqlite3_column_double(stmt, 1);
    double cur_wall = sqlite3_column_double(stmt, 2);
    double cur_sprd = sqlite3_column_double(stmt, 3);
    double cur_bv10 = sqlite3_column_double(stmt, 4);
    double cur_av10 = sqlite3_column_double(stmt, 5);
    double cur_bd05 = sqlite3_column_double(stmt, 6);
    double cur_ad05 = sqlite3_column_double(stmt, 7);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    printf("=== Order Book Trend (last %d hours, %d snapshots) ===\\n", hours, count);
    printf("Metric                Current    %dh Avg    Delta     Direction\\n", hours);
    printf("-------------------   --------   --------   -------   ---------\\n");
    printf("Imbalance (F77)       %8.4f   %8.4f   %+8.4f   %s\\n",
           cur_imb, avg_imb, cur_imb - avg_imb,
           fabs(cur_imb - avg_imb) < 0.05 ? "neutral" :
           cur_imb > avg_imb ? "↑ bidder" : "↓ seller");
    printf("Depth ratio (F78)     %8.4f   %8.4f   %+8.4f   %s\\n",
           cur_dr, avg_dr, cur_dr - avg_dr,
           fabs(cur_dr - avg_dr) < 0.05 ? "neutral" :
           cur_dr > avg_dr ? "↑ bid-side" : "↓ ask-side");
    printf("Wall conc (F79)       %8.4f   %8.4f   %+8.4f   %s\\n",
           cur_wall, avg_wall, cur_wall - avg_wall,
           cur_wall > avg_wall ? "concentrating" : "spreading");
    printf("Spread (F80, bps)     %8.4f   %8.4f   %+8.4f   %s\\n",
           cur_sprd, avg_sprd, cur_sprd - avg_sprd,
           cur_sprd < avg_sprd ? "tightening" : "widening");
    printf("Top-10 bid vol        %8.2f   %8.2f   %+8.2f\\n",
           cur_bv10, avg_bv10, cur_bv10 - avg_bv10);
    printf("Top-10 ask vol        %8.2f   %8.2f   %+8.2f\\n",
           cur_av10, avg_av10, cur_av10 - avg_av10);
    printf("Depth 0.5%% bid        %8.2f   %8.2f   %+8.2f\\n",
           cur_bd05, avg_bd05, cur_bd05 - avg_bd05);
    printf("Depth 0.5%% ask        %8.2f   %8.2f   %+8.2f\\n",
           cur_ad05, avg_ad05, cur_ad05 - avg_ad05);

    /* Signal */
    int signals = 0;
    if (cur_imb > avg_imb + 0.1) { printf("\\n⚠  SIGNAL: Bidder aggression — imbalance +%.4f above avg\\n", cur_imb - avg_imb); signals++; }
    if (cur_sprd > avg_sprd * 2 && avg_sprd > 0.01) { printf("\\n⚠  SIGNAL: Spread widening — %.4f bps (%.1fx avg)\\n", cur_sprd, cur_sprd / avg_sprd); signals++; }
    if (cur_wall > avg_wall * 1.5 && avg_wall > 0.01) { printf("\\n⚠  SIGNAL: Wall forming — conc %.4f (%.1fx avg)\\n", cur_wall, cur_wall / avg_wall); signals++; }
    if (cur_bd05 > avg_bd05 * 2) { printf("\\n⚠  SIGNAL: Bid depth surge — %.1f BTC (%.1fx avg)\\n", cur_bd05, cur_bd05 / avg_bd05); signals++; }
    if (signals == 0) printf("\\n✓  No significant signals.\\n");

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s fetch|features|book|archive|trend [hours]\n", argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "fetch")) return cmd_fetch();
    else if (!strcmp(argv[1], "features")) return cmd_features();
    else if (!strcmp(argv[1], "book")) return cmd_book();
    else if (!strcmp(argv[1], "archive")) return cmd_archive();
    else if (!strcmp(argv[1], "trend")) {
        int hours = 24;
        if (argc > 2) hours = atoi(argv[2]);
        if (hours < 1) hours = 24;
        return cmd_trend(hours);
    }
    else { printf("Unknown command: %s\n", argv[1]); return 1; }
}
