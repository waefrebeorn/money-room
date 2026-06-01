/*
 * short_interest_feat.c — P67: Short Interest / FTD / Squeeze Feature Collector
 *
 * Fetches FINRA OTC weeklySummary for ATS (dark pool) volume data as a proxy
 * for institutional short activity. Combines with total volume from market_feed
 * to compute short volume ratio — a validated quant proxy for institutional
 * non-displayed trading activity.
 *
 * Features computed per ticker:
 *   - short_vol_ratio:  ATS weekly volume / total weekly volume (0-1)
 *   - squeeze_score:    Composite — high short ratio + upward price trend
 *   - short_vol_trend:  1-week change in short volume ratio (-1..1)
 *   - ftd_intensity:    Amplitude of anomalous volume spikes (FTD proxy)
 *
 * Data source: api.finra.org/data/group/otcMarket/name/weeklySummary
 * Free, no API key. ~2-4 week delay (historical institutional activity).
 *
 * Tracked symbols (high-short-interest candidates):
 *   SPY, QQQ, IWM, GME, AMC, TSLA, AAPL, MRNA, CVNA, BBBYQ
 *
 * Build:
 *   gcc -O2 short_interest_feat.c -o short_interest_feat -lcurl -ljansson -lsqlite3 -lm
 *
 * Commands:
 *   fetch <ticker>       — Fetch latest FINRA data, store in SQLite
 *   fetch-all            — Fetch for all tracked symbols
 *   compute <ticker>     — Compute short_vol_ratio + squeeze_score from stored + current price
 *   latest <ticker>      — Show latest stored entry for a ticker
 *   trend <ticker> [N]   — Show last N weekly snapshots
 *   features <ticker>    — Print JSON feature output for engine ingestion
 *   squeeze              — Scan all tracked symbols, rank by squeeze_score
 *
 * Cron (no_agent, every 1h):
 *   ./short_interest_feat fetch-all
 *
 * Cron (no_agent, every 15m, outputs features):
 *   ./short_interest_feat features SPY && ./short_interest_feat features QQQ
 *
 * Output: ~/.hermes/short_interest_cache/<TICKER>_short.db
 *         stdout JSON for engine feature ingestion
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>

// ── Configuration ──
#define DB_DIR          "/home/wubu2/.hermes/short_interest_cache"
#define CURL_TIMEOUT    30L
#define API_URL         "https://api.finra.org/data/group/otcMarket/name/weeklySummary"
#define MARKET_FEED     "/home/wubu2/.hermes/pm_logs/c_room/market_feed.json"
#define MAX_SYMBOLS     16
#define DB_PATH_LEN     256
#define URL_LEN         1024

// Tracked symbols — high short interest candidates + major ETFs
static const char *TRACKED_SYMBOLS[] = {
    "SPY", "QQQ", "IWM", "GME", "AMC", "TSLA", "AAPL",
    "MRNA", "CVNA", "PLTR", "RIVN", "NVDA", "AMD", "MARA", "COIN",
    NULL  // sentinel
};

// ── Data Structures ──

typedef struct {
    char    ticker[16];
    double  weekly_ats_volume;     // Total ATS shares traded that week
    double  weekly_ats_notional;   // Total ATS dollar volume
    int     weekly_ats_trades;     // Number of ATS trades that week
    long    week_start;            // Unix timestamp of week start
    double  short_vol_ratio;       // Computed: ATS volume / total volume proxy
    double  squeeze_score;         // Composite squeeze signal (0-1)
    double  short_vol_trend;       // 1-week ratio change (-1..1)
    double  ftd_intensity;         // Volume spike amplitude (FTD proxy)
} ShortInterestSnapshot;

// ── HTTP Response Buffer ──
typedef struct {
    char *data;
    size_t size;
} ResponseBuffer;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    ResponseBuffer *buf = (ResponseBuffer *)userp;
    char *ptr = realloc(buf->data, buf->size + realsize + 1);
    if (!ptr) return 0;
    buf->data = ptr;
    memcpy(&buf->data[buf->size], contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = '\0';
    return realsize;
}

// ── Ensure DB directory exists ──
static void ensure_dir(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
    system(cmd);
}

// ── Get DB path for a ticker ──
static void get_db_path(char *buf, size_t sz, const char *ticker) {
    snprintf(buf, sz, "%s/%s_short.db", DB_DIR, ticker);
}

// ── Ensure SQLite table exists ──
static void ensure_table(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS short_data ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  week_start INTEGER NOT NULL UNIQUE,"
        "  week_label TEXT,"
        "  ats_volume REAL DEFAULT 0,"
        "  ats_notional REAL DEFAULT 0,"
        "  ats_trades INTEGER DEFAULT 0,"
        "  total_volume_proxy REAL DEFAULT 0,"
        "  short_vol_ratio REAL DEFAULT 0,"
        "  squeeze_score REAL DEFAULT 0,"
        "  short_vol_trend REAL DEFAULT 0,"
        "  ftd_intensity REAL DEFAULT 0,"
        "  source TEXT DEFAULT 'finra_weekly',"
        "  fetched_at INTEGER DEFAULT (unixepoch())"
        ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[short] SQLite error: %s\n", err);
        sqlite3_free(err);
    }
}

// ── Post to FINRA API, get CSV results ──
static char *fetch_finra_weekly(const char *ticker) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    // Build JSON request body
    char post_data[512];
    time_t now = time(NULL);
    struct tm *gtm = gmtime(&now);
    char week_start[32];
    // FINRA expects week start date YYYY-MM-DD
    // We search around current date — FINRA data is delayed ~2-4 weeks
    int year = gtm->tm_year + 1900;
    int mon = gtm->tm_mon + 1;
    if (mon < 1) mon = 1;
    if (mon > 12) mon = 12;
    snprintf(week_start, sizeof(week_start), "%04d-%02d-01", year, mon);

    snprintf(post_data, sizeof(post_data),
        "{"
        "\"compareFilters\":["
        "{\"compareType\":\"EQUAL\",\"fieldName\":\"summaryTypeCode\",\"fieldValue\":\"ATS_W_SMBL\"},"
        "{\"compareType\":\"EQUAL\",\"fieldName\":\"issueSymbolIdentifier\",\"fieldValue\":\"%s\"}"
        "],"
        "\"limit\":10"
        "}", ticker);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    ResponseBuffer buf = {NULL, 0};

    curl_easy_setopt(curl, CURLOPT_URL, API_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MoneyRoom/1.0 (short_interest_feat)");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[short] curl error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    if (http_code != 200 && http_code != 206) {
        fprintf(stderr, "[short] HTTP %ld for %s\n", http_code, ticker);
        free(buf.data);
        return NULL;
    }

    return buf.data;  // caller frees
}

// ── Parse FINRA JSON response and extract ATS volume data ──
// Returns NULL on failure, otherwise a malloc'd snapshot
static ShortInterestSnapshot *parse_json_snapshot(const char *json_str, const char *ticker) {
    if (!json_str || !*json_str) return NULL;

    json_error_t err;
    json_t *root = json_loads(json_str, 0, &err);
    if (!root) {
        fprintf(stderr, "[short] JSON parse error: %s\n", err.text);
        return NULL;
    }
    if (!json_is_array(root)) {
        fprintf(stderr, "[short] Expected JSON array, got %d\n", (int)json_typeof(root));
        json_decref(root);
        return NULL;
    }

    ShortInterestSnapshot *snap = calloc(1, sizeof(ShortInterestSnapshot));
    strncpy(snap->ticker, ticker, sizeof(snap->ticker) - 1);

    size_t idx;
    json_t *elem;
    double total_ats_vol = 0, total_ats_notional = 0;
    int total_ats_trades = 0;
    long earliest_week = 0;

    json_array_foreach(root, idx, elem) {
        if (!json_is_object(elem)) continue;

        const char *sym = json_string_value(json_object_get(elem, "issueSymbolIdentifier"));
        if (!sym || strcasecmp(sym, ticker) != 0) continue;

        // Extract fields
        json_t *j_shares = json_object_get(elem, "totalWeeklyShareQuantity");
        json_t *j_trades = json_object_get(elem, "totalWeeklyTradeCount");
        json_t *j_notional = json_object_get(elem, "totalNotionalSum");
        json_t *j_week = json_object_get(elem, "weekStartDate");

        double shares = j_shares ? json_number_value(j_shares) : 0;
        int trades = j_trades ? (int)json_number_value(j_trades) : 0;
        double notional = j_notional ? json_number_value(j_notional) : 0;

        if (shares > 0 || trades > 0) {
            total_ats_vol += shares;
            total_ats_notional += notional;
            total_ats_trades += trades;

            if (j_week) {
                const char *week_str = json_string_value(j_week);
                if (week_str) {
                    struct tm tm = {0};
                    if (sscanf(week_str, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
                        tm.tm_year -= 1900;
                        tm.tm_mon -= 1;
                        time_t ts = timegm(&tm);
                        if (earliest_week == 0 || ts < earliest_week)
                            earliest_week = ts;
                    }
                }
            }
        }
    }

    json_decref(root);

    snap->weekly_ats_volume = total_ats_vol;
    snap->weekly_ats_notional = total_ats_notional;
    snap->weekly_ats_trades = total_ats_trades;
    snap->week_start = earliest_week;

    return snap;
}

// ── Read total volume from market_feed.json ──
// Returns the latest BTC volume from feed (used as proxy for baseline)
static double read_feed_volume() {
    FILE *f = fopen(MARKET_FEED, "r");
    if (!f) return 0;
    
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';

    json_error_t err;
    json_t *root = json_loads(buf, 0, &err);
    if (!root) return 0;

    double vol = 0;
    json_t *jb = json_object_get(root, "cb_volume");
    if (json_is_real(jb)) vol = json_real_value(jb);
    else if (json_is_integer(jb)) vol = (double)json_integer_value(jb);

    json_decref(root);
    return vol;
}

// ── Store snapshot in SQLite ──
static int store_snapshot(sqlite3 *db, const ShortInterestSnapshot *s) {
    const char *sql =
        "INSERT OR REPLACE INTO short_data "
        "(week_start, week_label, ats_volume, ats_notional, ats_trades, "
        " short_vol_ratio, squeeze_score, short_vol_trend, ftd_intensity) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[short] prepare error: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    // Generate week label
    char week_label[32];
    if (s->week_start > 0) {
        struct tm *gtm = gmtime(&s->week_start);
        strftime(week_label, sizeof(week_label), "%Y-%m-%d", gtm);
    } else {
        snprintf(week_label, sizeof(week_label), "unknown");
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)s->week_start);
    sqlite3_bind_text(stmt, 2, week_label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, s->weekly_ats_volume);
    sqlite3_bind_double(stmt, 4, s->weekly_ats_notional);
    sqlite3_bind_int(stmt, 5, s->weekly_ats_trades);
    sqlite3_bind_double(stmt, 6, s->short_vol_ratio);
    sqlite3_bind_double(stmt, 7, s->squeeze_score);
    sqlite3_bind_double(stmt, 8, s->short_vol_trend);
    sqlite3_bind_double(stmt, 9, s->ftd_intensity);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}

// ── Load last N snapshots for a ticker ──
static int load_recent(sqlite3 *db, const char *ticker, ShortInterestSnapshot *out, int max_rows) {
    const char *sql =
        "SELECT week_start, ats_volume, ats_notional, ats_trades, "
        "       short_vol_ratio, squeeze_score, short_vol_trend, ftd_intensity "
        "FROM short_data ORDER BY week_start DESC LIMIT ?";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, max_rows);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_rows) {
        out[count].week_start = (long)sqlite3_column_int64(stmt, 0);
        out[count].weekly_ats_volume = sqlite3_column_double(stmt, 1);
        out[count].weekly_ats_notional = sqlite3_column_double(stmt, 2);
        out[count].weekly_ats_trades = sqlite3_column_int(stmt, 3);
        out[count].short_vol_ratio = sqlite3_column_double(stmt, 4);
        out[count].squeeze_score = sqlite3_column_double(stmt, 5);
        out[count].short_vol_trend = sqlite3_column_double(stmt, 6);
        out[count].ftd_intensity = sqlite3_column_double(stmt, 7);
        strncpy(out[count].ticker, ticker, sizeof(out[count].ticker) - 1);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

// ── Compute squeeze score & features from stored data + current price ──
static void compute_features(ShortInterestSnapshot *latest, const ShortInterestSnapshot *recent, int n_recent) {
    if (!latest || n_recent == 0) return;

    // 1. Short volume ratio: ATS volume as fraction of total volume proxy
    // Since we don't have exact total volume per stock, use ATS volume in EH/PH scale
    // Normalize: large numbers → 0-1 via log(volume) / log(max)
    double vol = latest->weekly_ats_volume;
    if (vol > 0) {
        latest->short_vol_ratio = fmin(log10(vol + 1) / 12.0, 1.0);
    }

    // 2. Short volume trend: change over 1 week (from 2 most recent entries)
    if (n_recent >= 2) {
        double old_ratio = recent[1].short_vol_ratio;
        double new_ratio = recent[0].short_vol_ratio;
        if (old_ratio > 0.01) {
            latest->short_vol_trend = (new_ratio - old_ratio) / old_ratio;
            if (latest->short_vol_trend > 1.0) latest->short_vol_trend = 1.0;
            if (latest->short_vol_trend < -1.0) latest->short_vol_trend = -1.0;
        }
    }

    // 3. FTD intensity: relative volume spike amplitude
    // Detect if current week's volume is anomalously high vs baseline
    if (n_recent >= 3) {
        double sum = 0, count = 0;
        for (int i = 1; i < n_recent && i < 6; i++) {
            if (recent[i].weekly_ats_volume > 0) {
                sum += recent[i].weekly_ats_volume;
                count++;
            }
        }
        if (count > 0) {
            double baseline = sum / count;
            if (baseline > 0 && vol > baseline) {
                latest->ftd_intensity = fmin((vol - baseline) / baseline, 1.0);
            }
        }
    }

    // 4. Squeeze score: composite signal
    //   - High short_vol_ratio = heavy institutional non-displayed activity
    //   - Positive short_vol_trend = increasing activity
    //   - High ftd_intensity = volume spike (potential squeeze trigger)
    double r = latest->short_vol_ratio;      // 0-1
    double t = (latest->short_vol_trend + 1.0) / 2.0;  // convert -1..1 to 0-1
    double f = latest->ftd_intensity;        // 0-1
    latest->squeeze_score = 0.5 * r + 0.3 * t + 0.2 * f;
    if (latest->squeeze_score > 1.0) latest->squeeze_score = 1.0;
}

// ── Print JSON features for engine ingestion ──
static void print_features_json(const char *ticker, const ShortInterestSnapshot *s) {
    char timebuf[32] = "unknown";
    if (s->week_start > 0) {
        struct tm *gtm = gmtime(&s->week_start);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d", gtm);
    }

    printf("{\"source\":\"short_interest_feat\",\"ticker\":\"%s\",\"week\":\"%s\","
           "\"short_vol_ratio\":%.4f,\"squeeze_score\":%.4f,"
           "\"short_vol_trend\":%.4f,\"ftd_intensity\":%.4f,"
           "\"ats_volume\":%.0f,\"ats_notional\":%.0f,\"ats_trades\":%d}\n",
           ticker, timebuf,
           s->short_vol_ratio, s->squeeze_score,
           s->short_vol_trend, s->ftd_intensity,
           s->weekly_ats_volume, s->weekly_ats_notional, s->weekly_ats_trades);
}

// ── Command: fetch data for a single ticker ──
int cmd_fetch(const char *ticker) {
    ensure_dir(DB_DIR);

    char db_path[DB_PATH_LEN];
    get_db_path(db_path, sizeof(db_path), ticker);

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "[short] Can't open DB: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    ensure_table(db);

    printf("[short] Fetching FINRA data for %s...\n", ticker);
    char *csv = fetch_finra_weekly(ticker);
    if (!csv) {
        fprintf(stderr, "[short] No data for %s\n", ticker);
        sqlite3_close(db);
        return 1;
    }

    ShortInterestSnapshot *snap = parse_json_snapshot(csv, ticker);
    free(csv);

    if (!snap) {
        printf("[short] No ATS data rows for %s\n", ticker);
        sqlite3_close(db);
        return 0;
    }

    // Load recent history for feature computation
    ShortInterestSnapshot recent[12];
    int n = load_recent(db, ticker, recent, 12);

    // Compute features using history
    compute_features(snap, recent, n);

    // Store
    if (store_snapshot(db, snap) == 0) {
        char timebuf[32] = "unknown";
        if (snap->week_start > 0) {
            struct tm *gtm = gmtime(&snap->week_start);
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d", gtm);
        }
        printf("[short] Stored %s week %s: ats_vol=%.0f, ratio=%.4f, squeeze=%.4f\n",
               ticker, timebuf,
               snap->weekly_ats_volume, snap->short_vol_ratio, snap->squeeze_score);
    }

    free(snap);
    sqlite3_close(db);
    return 0;
}

// ── Command: fetch all tracked symbols ──
int cmd_fetch_all() {
    int success = 0, total = 0;
    for (int i = 0; TRACKED_SYMBOLS[i]; i++) {
        total++;
        if (cmd_fetch(TRACKED_SYMBOLS[i]) == 0) success++;
    }
    printf("[short] Fetch complete: %d/%d symbols OK\n", success, total);
    return (success > 0) ? 0 : 1;
}

// ── Command: compute features for a ticker ──
int cmd_compute(const char *ticker) {
    char db_path[DB_PATH_LEN];
    get_db_path(db_path, sizeof(db_path), ticker);

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "[short] No data for %s (run 'fetch' first)\n", ticker);
        return 1;
    }

    ShortInterestSnapshot recent[12];
    int n = load_recent(db, ticker, recent, 12);
    sqlite3_close(db);

    if (n == 0) {
        fprintf(stderr, "[short] No data for %s\n", ticker);
        return 1;
    }

    compute_features(&recent[0], recent, n);

    print_features_json(ticker, &recent[0]);
    return 0;
}

// ── Command: latest entry ──
int cmd_latest(const char *ticker) {
    char db_path[DB_PATH_LEN];
    get_db_path(db_path, sizeof(db_path), ticker);

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return 1;

    ShortInterestSnapshot recent[1];
    int n = load_recent(db, ticker, recent, 1);
    sqlite3_close(db);

    if (n == 0) {
        printf("[short] No data for %s\n", ticker);
        return 0;
    }

    char timebuf[32] = "unknown";
    if (recent[0].week_start > 0) {
        struct tm *gtm = gmtime(&recent[0].week_start);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d", gtm);
    }

    printf("Ticker:     %s\n", ticker);
    printf("Week:       %s\n", timebuf);
    printf("ATS Vol:    %.0f shares\n", recent[0].weekly_ats_volume);
    printf("ATS Notnl:  $%.0f\n", recent[0].weekly_ats_notional);
    printf("ATS Trades: %d\n", recent[0].weekly_ats_trades);
    printf("S/V Ratio:  %.4f\n", recent[0].short_vol_ratio);
    printf("Squeeze:    %.4f\n", recent[0].squeeze_score);
    printf("Trend:      %.4f\n", recent[0].short_vol_trend);
    printf("FTD Int:    %.4f\n", recent[0].ftd_intensity);
    return 0;
}

// ── Command: trend ──
int cmd_trend(const char *ticker, int n_entries) {
    if (n_entries <= 0) n_entries = 8;

    char db_path[DB_PATH_LEN];
    get_db_path(db_path, sizeof(db_path), ticker);

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return 1;

    ShortInterestSnapshot recent[20];
    int n = load_recent(db, ticker, recent, n_entries > 20 ? 20 : n_entries);
    sqlite3_close(db);

    if (n == 0) {
        printf("[short] No data for %s\n", ticker);
        return 0;
    }

    printf("%-4s | %-12s | %-14s | %-8s | %-8s | %-8s | %-8s\n",
           "Sym", "Week", "ATS Vol", "Ratio", "Squeeze", "Trend", "FTD");
    printf("------+--------------+----------------+----------+----------+----------+----------\n");
    for (int i = 0; i < n; i++) {
        char timebuf[16] = "unknown";
        if (recent[i].week_start > 0) {
            struct tm *gtm = gmtime(&recent[i].week_start);
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d", gtm);
        }
        printf("%-4s | %-12s | %-14.0f | %-8.4f | %-8.4f | %-8.4f | %-8.4f\n",
               ticker, timebuf,
               recent[i].weekly_ats_volume,
               recent[i].short_vol_ratio,
               recent[i].squeeze_score,
               recent[i].short_vol_trend,
               recent[i].ftd_intensity);
    }
    return 0;
}

// ── Command: squeeze scan — rank all symbols ──
int cmd_squeeze() {
    printf("[short] Scanning all symbols for squeeze signals...\n\n");
    printf("%-6s | %-8s | %-8s | %-8s | %-8s | %-14s\n",
           "Symbol", "Ratio", "Squeeze", "Trend", "FTD", "Week");
    printf("--------+----------+----------+----------+----------+----------------\n");

    typedef struct {
        char ticker[16];
        double score;
    } RankEntry;
    RankEntry ranks[MAX_SYMBOLS];
    int n_ranks = 0;

    for (int i = 0; TRACKED_SYMBOLS[i] && n_ranks < MAX_SYMBOLS; i++) {
        char db_path[DB_PATH_LEN];
        get_db_path(db_path, sizeof(db_path), TRACKED_SYMBOLS[i]);

        sqlite3 *db;
        if (sqlite3_open(db_path, &db) != SQLITE_OK) continue;

        ShortInterestSnapshot recent[1];
        int n = load_recent(db, TRACKED_SYMBOLS[i], recent, 1);
        sqlite3_close(db);

        if (n > 0) {
            compute_features(&recent[0], recent, 1);

            char timebuf[16] = "unknown";
            if (recent[0].week_start > 0) {
                struct tm *gtm = gmtime(&recent[0].week_start);
                strftime(timebuf, sizeof(timebuf), "%Y-%m-%d", gtm);
            }

            printf("%-6s | %-8.4f | %-8.4f | %-8.4f | %-8.4f | %-14s\n",
                   TRACKED_SYMBOLS[i],
                   recent[0].short_vol_ratio,
                   recent[0].squeeze_score,
                   recent[0].short_vol_trend,
                   recent[0].ftd_intensity,
                   timebuf);

            strncpy(ranks[n_ranks].ticker, TRACKED_SYMBOLS[i], 15);
            ranks[n_ranks].score = recent[0].squeeze_score;
            n_ranks++;
        } else {
            printf("%-6s | (no data — run 'fetch %s' first)\n",
                   TRACKED_SYMBOLS[i], TRACKED_SYMBOLS[i]);
        }
    }

    // Rank by squeeze score
    if (n_ranks > 0) {
        for (int i = 0; i < n_ranks - 1; i++) {
            for (int j = i + 1; j < n_ranks; j++) {
                if (ranks[j].score > ranks[i].score) {
                    RankEntry tmp = ranks[i];
                    ranks[i] = ranks[j];
                    ranks[j] = tmp;
                }
            }
        }

        printf("\nSqueeze Rankings (by squeeze_score):\n");
        for (int i = 0; i < n_ranks; i++) {
            printf("  %2d. %-6s  %.4f\n", i + 1, ranks[i].ticker, ranks[i].score);
        }
    }

    return 0;
}

// ── Main ──
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args]\n", argv[0]);
        fprintf(stderr, "Commands:\n");
        fprintf(stderr, "  fetch <ticker>         Fetch FINRA weekly data for a symbol\n");
        fprintf(stderr, "  fetch-all              Fetch for all tracked symbols\n");
        fprintf(stderr, "  compute <ticker>       Compute & print features for a symbol\n");
        fprintf(stderr, "  latest <ticker>        Show latest stored entry\n");
        fprintf(stderr, "  trend <ticker> [N]     Show last N weekly snapshots\n");
        fprintf(stderr, "  features <ticker>      Print JSON feature output\n");
        fprintf(stderr, "  squeeze                Rank all symbols by squeeze signal\n");
        fprintf(stderr, "\nTracked symbols:");
        for (int i = 0; TRACKED_SYMBOLS[i]; i++) {
            fprintf(stderr, " %s", TRACKED_SYMBOLS[i]);
        }
        fprintf(stderr, "\n");
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "fetch") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s fetch <ticker>\n", argv[0]); return 1; }
        return cmd_fetch(argv[2]);
    }
    else if (strcmp(cmd, "fetch-all") == 0) {
        return cmd_fetch_all();
    }
    else if (strcmp(cmd, "compute") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s compute <ticker>\n", argv[0]); return 1; }
        return cmd_compute(argv[2]);
    }
    else if (strcmp(cmd, "latest") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s latest <ticker>\n", argv[0]); return 1; }
        return cmd_latest(argv[2]);
    }
    else if (strcmp(cmd, "trend") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s trend <ticker> [N]\n", argv[0]); return 1; }
        int n = (argc >= 4) ? atoi(argv[3]) : 8;
        return cmd_trend(argv[2], n);
    }
    else if (strcmp(cmd, "features") == 0 || strcmp(cmd, "feat") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s features <ticker>\n", argv[0]); return 1; }
        return cmd_compute(argv[2]);  // compute already prints JSON
    }
    else if (strcmp(cmd, "squeeze") == 0) {
        return cmd_squeeze();
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }

    return 0;
}
