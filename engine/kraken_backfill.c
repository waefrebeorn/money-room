/**
 * kraken_backfill.c — T203: Kraken BTC 1-min OHLCV historical backfill
 *
 * Fetches BTC/USD 1-min candles from Kraken public REST API (no key needed).
 * Uses the 'since' parameter for pagination (720 candles per request max).
 * Supports resumption: checks latest timestamp in historical.db.
 *
 * Usage: ./kraken_backfill                # resume from latest
 *        ./kraken_backfill 1577836800      # backfill from 2020-01-01
 *        ./kraken_backfill 0               # full backfill from earliest (~2017)
 *
 * Build: gcc -O2 kraken_backfill.c -o kraken_backfill -lcurl -ljansson -lsqlite3 -lm
 *
 * Schema: candles_multi (pair='Kraken_BTC', interval=60, ts, OHLC+V)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#define DB_PATH     "/home/wubu2/.hermes/pm_logs/historical/historical.db"
#define HB_DIR      "/home/wubu2/.hermes/infra/heartbeats"
#define HB_PATH     HB_DIR "/kraken-backfill.heartbeat"
#define PAIR_NAME   "Kraken_BTC"
#define KRAKEN_PAIR "XXBTZUSD"
#define INTERVAL    60          /* 1 minute in seconds */
#define KRAKEN_INT  1           /* Kraken's interval parameter for 1-min */
#define API_LIMIT   720         /* Max candles per Kraken API call */
#define REQ_DELAY_MS 500        /* 0.5s delay between requests (rate limit) */

struct MemBuf { char *data; size_t size; };

static size_t write_cb(void *p, size_t s, size_t n, void *u) {
    size_t t = s * n;
    struct MemBuf *m = u;
    char *np = realloc(m->data, m->size + t + 1);
    if (!np) return 0;
    m->data = np;
    memcpy(m->data + m->size, p, t);
    m->size += t;
    m->data[m->size] = 0;
    return t;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    struct MemBuf mb = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "kraken-backfill/1.0");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(mb.data); return NULL; }
    return mb.data;
}

/* Find latest timestamp for our pair in DB (for resume) */
static time_t find_latest(sqlite3 *db) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT MAX(ts) FROM candles_multi WHERE pair=? AND interval=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, PAIR_NAME, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, INTERVAL);
    time_t latest = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        latest = (time_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return latest;
}

/* Get total candle count for progress reporting */
static int get_count(sqlite3 *db) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT COUNT(*) FROM candles_multi WHERE pair=? AND interval=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, PAIR_NAME, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, INTERVAL);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

/* Write one heartbeat line to progress indicator */
static void write_hb(void) {
    mkdir(HB_DIR, 0755);
    FILE *hf = fopen(HB_PATH, "w");
    if (hf) { fprintf(hf, "%ld\n", (long)time(NULL)); fclose(hf); }
}

int main(int argc, char **argv) {
    printf("[KRAKEN_BACKFILL] Kraken BTC 1-min historical backfill\n");

    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "ERROR: Can't open DB: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    /* Ensure table exists */
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS candles_multi ("
        "  pair TEXT NOT NULL DEFAULT 'Kraken_BTC',"
        "  interval INTEGER NOT NULL DEFAULT 60,"
        "  ts INTEGER NOT NULL,"
        "  open REAL, high REAL, low REAL, close REAL,"
        "  volume REAL, trades INTEGER DEFAULT 0,"
        "  UNIQUE(pair, interval, ts)"
        ");",
        NULL, NULL, NULL);

    /* Determine start timestamp */
    time_t start_ts = 0;
    if (argc > 1) {
        start_ts = (time_t)atol(argv[1]);
    } else {
        start_ts = find_latest(db);
        if (start_ts > 0)
            printf("[KRAKEN_BACKFILL] Resuming from %ld (%s)", 
                   (long)start_ts, ctime(&start_ts));
    }
    if (start_ts == 0) {
        /* Default: start from 2017-01-01 (BTC ~$1K era on Kraken) */
        start_ts = 1483228800;
        printf("[KRAKEN_BACKFILL] Starting from 2017-01-01\n");
    }

    /* Remove existing old data so we can re-fill cleanly */
    if (start_ts == 1483228800) {
        sqlite3_exec(db, "DELETE FROM candles_multi WHERE pair='Kraken_BTC' AND interval=60;", NULL, NULL, NULL);
        printf("[KRAKEN_BACKFILL] Cleaned existing Kraken_BTC data for fresh backfill\n");
    }

    int total_requests = 0;
    int total_candles = 0;
    time_t since = start_ts;
    int stalled = 0;

    while (1) {
        /* Build URL with since param */
        char url[256];
        snprintf(url, sizeof(url),
            "https://api.kraken.com/0/public/OHLC?pair=%s&interval=%d&since=%ld",
            KRAKEN_PAIR, KRAKEN_INT, (long)since);

        char *raw = http_get(url);
        if (!raw) {
            fprintf(stderr, "[KRAKEN_BACKFILL] Request failed at since=%ld, retrying...\n", (long)since);
            sleep(2);
            continue;
        }

        json_error_t err;
        json_t *root = json_loads(raw, 0, &err);
        free(raw);

        if (!root) {
            fprintf(stderr, "[KRAKEN_BACKFILL] JSON parse error at since=%ld\n", (long)since);
            sleep(2);
            continue;
        }

        /* Check for API errors */
        json_t *err_arr = json_object_get(root, "error");
        if (err_arr && json_array_size(err_arr) > 0) {
            json_t *e = json_array_get(err_arr, 0);
            if (e) fprintf(stderr, "[KRAKEN_BACKFILL] API error: %s\n", json_string_value(e));
            json_decref(root);
            sleep(5);
            continue;
        }

        json_t *result = json_object_get(root, "result");
        if (!json_is_object(result)) {
            json_decref(root);
            fprintf(stderr, "[KRAKEN_BACKFILL] No result object\n");
            break;
        }

        /* Extract candles array (the key is the pair name) */
        json_t *candles = json_object_get(result, KRAKEN_PAIR);
        json_t *last_j = json_object_get(result, "last");
        time_t new_since = last_j ? (time_t)json_integer_value(last_j) : 0;

        if (!candles || !json_is_array(candles) || json_array_size(candles) == 0) {
            json_decref(root);
            printf("[KRAKEN_BACKFILL] No more candles — done!\n");
            break;
        }

        size_t batch_size = json_array_size(candles);
        total_requests++;

        /* Insert into DB */
        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO candles_multi "
            "(pair, interval, ts, open, high, low, close, volume, trades) "
            "VALUES (?, 60, ?, ?, ?, ?, ?, ?, ?)",
            -1, &stmt, NULL);

        int inserted = 0;
        for (size_t i = 0; i < batch_size; i++) {
            json_t *c = json_array_get(candles, i);
            if (!json_is_array(c) || json_array_size(c) < 8) continue;

            time_t ts = (time_t)json_integer_value(json_array_get(c, 0));
            double open  = json_number_value(json_array_get(c, 1));
            double high  = json_number_value(json_array_get(c, 2));
            double low   = json_number_value(json_array_get(c, 3));
            double close = json_number_value(json_array_get(c, 4));
            double volume = json_number_value(json_array_get(c, 6));
            int trades = (int)json_integer_value(json_array_get(c, 7));

            if (close <= 0.0) continue;  /* Skip empty candles */

            sqlite3_bind_text(stmt, 1, PAIR_NAME, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)ts);
            sqlite3_bind_double(stmt, 3, open);
            sqlite3_bind_double(stmt, 4, high);
            sqlite3_bind_double(stmt, 5, low);
            sqlite3_bind_double(stmt, 6, close);
            sqlite3_bind_double(stmt, 7, volume);
            sqlite3_bind_int(stmt, 8, trades);
            if (sqlite3_step(stmt) == SQLITE_DONE) inserted++;
            sqlite3_reset(stmt);
        }

        sqlite3_finalize(stmt);
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        total_candles += inserted;

        /* Progress */
        int pct = 0;
        time_t now = time(NULL);
        if (now > start_ts) {
            pct = (int)(100.0 * (since - start_ts) / (now - start_ts));
            if (pct > 100) pct = 100;
            if (pct < 0) pct = 0;
        }

        printf("\r  [%3d%%] ts=%ld (%d candles, %d remaining, %d requests)",
               pct, (long)since, inserted, 
               (int)((now - since) / 60), total_requests);
        fflush(stdout);

        /* Check if we've caught up to present */
        if (new_since <= since) break;

        /* Count candles inserted — if we're getting < 720, catching up */
        int is_last_batch = (int)batch_size < API_LIMIT;

        since = new_since;
        json_decref(root);

        /* Stall detection */
        if (inserted == 0) { stalled++; } else { stalled = 0; }
        if (stalled > 10) {
            printf("\n[KRAKEN_BACKFILL] Stalled — no new candles after 10 attempts. Breaking.\n");
            break;
        }

        /* Rate limit: 0.5s delay per Kraken free tier guidelines */
        struct timespec ts = {0, REQ_DELAY_MS * 1000000L};
        nanosleep(&ts, NULL);
        write_hb();

        /* If last batch was short, we're done */
        if (is_last_batch) {
            printf("\n[KRAKEN_BACKFILL] Final batch caught up to present.\n");
            break;
        }
    }

    /* Final stats */
    int final_count = get_count(db);
    time_t latest = find_latest(db);
    sqlite3_close(db);

    printf("\n[KRAKEN_BACKFILL] DONE\n");
    printf("  Requests:    %d\n", total_requests);
    printf("  Candles:     %d\n", total_candles);
    printf("  Total in DB: %d\n", final_count);
    printf("  Range:       %ld → %ld (%s)",
           (long)start_ts, (long)latest, ctime(&latest));
    printf("  Coverage:    %.1f days\n", (latest - start_ts) / 86400.0);

    /* Heartbeat */
    write_hb();

    return 0;
}
