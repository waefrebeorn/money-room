/**
 * polymarket_collector.c — Collect Polymarket events into DB
 * Uses public Gamma Markets API (free, no key).
 * Reads resolved events with outcomes → writes to polymarket_events.db
 *
 * Compile: gcc -O2 polymarket_collector.c -o polymarket_collector -lcurl -ljansson -lsqlite3 -lm
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>

#define PM_DB   "/home/wubu2/.hermes/pm_logs/historical/polymarket_events.db"
#define PM_API  "https://gamma-api.polymarket.com/events?closed=true&limit=100&offset="

typedef struct { char *data; size_t len; } http_buf_t;
static size_t write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t total = sz * nm; http_buf_t *b = (http_buf_t*)ud;
    char *np = realloc(b->data, b->len + total + 1);
    if (!np) return 0; b->data = np;
    memcpy(b->data + b->len, ptr, total);
    b->len += total; b->data[b->len] = '\0';
    return total;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    http_buf_t buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0");
    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (res != CURLE_OK) { free(buf.data); return NULL; }
    return buf.data;
}

int main(void) {
    sqlite3 *db;
    if (sqlite3_open(PM_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open DB: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    curl_global_init(CURL_GLOBAL_ALL);

    int total = 0, inserted = 0;
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

    for (int offset = 0; offset < 500; offset += 100) {
        char url[512];
        snprintf(url, sizeof(url), "%s%d", PM_API, offset);
        printf("[PM] Fetching offset %d...\n", offset);

        char *resp = http_get(url);
        if (!resp) { printf("[PM] FAILED offset %d\n", offset); break; }

        json_error_t err;
        json_t *events = json_loads(resp, 0, &err);
        free(resp);
        if (!events) { printf("[PM] JSON error: %s\n", err.text); break; }
        if (!json_is_array(events)) { json_decref(events); break; }

        int n = (int)json_array_size(events);
        if (n == 0) { json_decref(events); break; }

        for (int i = 0; i < n; i++) {
            json_t *ev = json_array_get(events, i);
            if (!ev) continue;

            // Get event ID to deduplicate
            json_t *jid = json_object_get(ev, "id");
            if (!jid) continue;
            const char *id_str = json_string_value(jid);
            if (!id_str) continue;
            int64_t event_id = atoll(id_str);

            // Check if already exists
            sqlite3_stmt *chk;
            sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM polymarket_events WHERE poly_event_id=?1", -1, &chk, NULL);
            sqlite3_bind_int64(chk, 1, event_id);
            int exists = 0;
            if (sqlite3_step(chk) == SQLITE_ROW) exists = sqlite3_column_int(chk, 0) > 0;
            sqlite3_finalize(chk);
            if (exists) continue;  // Skip duplicates

            // Parse fields
            json_t *jtitle = json_object_get(ev, "title");
            json_t *jdesc = json_object_get(ev, "description");
            json_t *jclosed = json_object_get(ev, "close_time");
            json_t *jcreated = json_object_get(ev, "created_at");
            json_t *jvol = json_object_get(ev, "volume");
            json_t *jcat = json_object_get(ev, "category");

            int64_t ts = jclosed ? json_integer_value(jclosed) / 1000 : time(NULL);
            int64_t created_ts = jcreated ? json_integer_value(jcreated) / 1000 : ts;
            double volume = jvol ? json_number_value(jvol) : 0;
            const char *cat = jcat ? json_string_value(jcat) : "unknown";
            const char *title = jtitle ? json_string_value(jtitle) : "";

            // Get outcomes from markets array
            json_t *markets = json_object_get(ev, "markets");
            double true_prob = 0.5;
            int outcome = -1;
            if (markets && json_is_array(markets) && json_array_size(markets) > 0) {
                json_t *mkt = json_array_get(markets, 0);
                if (mkt) {
                    json_t *jp = json_object_get(mkt, "outcome");
                    if (jp) {
                        const char *out_str = json_string_value(jp);
                        if (out_str) {
                            if (strcmp(out_str, "YES") == 0) { outcome = 1; true_prob = 1.0; }
                            else if (strcmp(out_str, "NO") == 0) { outcome = 0; true_prob = 0.0; }
                        }
                    }
                    json_t *jprice = json_object_get(mkt, "price");
                    if (jprice) true_prob = json_number_value(jprice);
                }
            }

            // Insert
            sqlite3_stmt *ins;
            sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO polymarket_events(poly_event_id,timestamp,category,true_probability,volume,outcome) "
                "VALUES(?1,?2,?3,?4,?5,?6)", -1, &ins, NULL);
            sqlite3_bind_int64(ins, 1, event_id);
            sqlite3_bind_int64(ins, 2, ts);
            sqlite3_bind_text(ins, 3, cat, -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(ins, 4, true_prob);
            sqlite3_bind_double(ins, 5, volume);
            if (outcome >= 0) sqlite3_bind_int(ins, 6, outcome);
            else sqlite3_bind_null(ins, 6);

            if (sqlite3_step(ins) == SQLITE_DONE) inserted++;
            sqlite3_finalize(ins);
            total++;
        }
        json_decref(events);

        if (n < 100) break;  // Last page
        struct timespec d = {1, 0}; nanosleep(&d, NULL);  // Rate limit
    }

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_close(db);
    curl_global_cleanup();

    printf("[PM] DONE: %d new events, %d total processed\n", inserted, total);
    return 0;
}
