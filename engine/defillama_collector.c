/**
 * defillama_collector.c — C101: DefiLlama TVL Collector
 *
 * Fetches top DeFi chain TVL data from DefiLlama free API.
 * Writes to timeline.db with source='defillama_chain_<name>',
 * 'defillama_dex_<name>'.
 *
 * Build: gcc -O2 -o defillama_collector defillama_collector.c -lcurl -ljansson -lsqlite3
 * Usage: ./defillama_collector
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <jansson.h>

#define DL_API "https://api.llama.fi"
#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define MAX_CHAINS 100
#define MAX_DEXS 50

typedef struct { char *data; size_t len; size_t cap; } RespBuf;

static size_t write_cb(void *p, size_t s, size_t n, void *u) {
    size_t t = s*n; RespBuf *r = u;
    if (r->len + t >= r->cap) {
        r->cap = r->len + t + 131072;
        r->data = realloc(r->data, r->cap);
    }
    memcpy(r->data + r->len, p, t);
    r->len += t; r->data[r->len] = 0;
    return t;
}

static RespBuf http_get(const char *url) {
    RespBuf r = {calloc(1, 131072), 0, 131072};
    CURL *c = curl_easy_init();
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "defillama-collector/1.0");
        curl_easy_perform(c);
        curl_easy_cleanup(c);
    }
    return r;
}

static sqlite3 *g_db = NULL;
static void db_init(void) {
    sqlite3_open(DB_PATH, &g_db);
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
}

static void db_insert(const char *source, long long ts, const char *cat, const char *json_data) {
    if (!g_db) return;
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO timeline (ts, source, category, data, collected_at) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, cat, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, json_data, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (long long)time(NULL));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void db_close(void) { if (g_db) sqlite3_close(g_db); }

static void sanitize(char *out, const char *in, int max) {
    int j = 0;
    for (int i = 0; in[i] && j < max-1; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) out[j++] = c;
        else if (c == ' ' || c == '-' || c == '.') out[j++] = '_';
    }
    out[j] = 0;
    /* lowercase */
    for (int i = 0; out[i]; i++) if (out[i] >= 'A' && out[i] <= 'Z') out[i] += 32;
}

int main(void) {
    printf("[DL] DefiLlama Collector\n");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    db_init();

    long long now = (long long)time(NULL);
    int total = 0;

    /* Phase 1: Fetch chains TVL */
    {
        RespBuf resp = http_get(DL_API "/chains");
        json_error_t err;
        json_t *root = json_loads(resp.data, 0, &err);
        free(resp.data);

        if (root && json_is_array(root)) {
            int n = (int)json_array_size(root);
            int limit = n < MAX_CHAINS ? n : MAX_CHAINS;
            for (int i = 0; i < limit; i++) {
                json_t *ch = json_array_get(root, i);
                const char *name = json_string_value(json_object_get(ch, "name"));
                double tvl = json_number_value(json_object_get(ch, "tvl"));
                if (!name) continue;

                char safe[64], source[128], json_data[256];
                sanitize(safe, name, 60);
                snprintf(source, sizeof(source), "defillama_chain_%s", safe);
                snprintf(json_data, sizeof(json_data),
                    "{\"chain\":\"%s\",\"tvl_usd\":%.0f}", name, tvl);

                db_insert(source, now, "defi", json_data);
                total++;
            }
        }
        json_decref(root);
    }

    /* Phase 2: Fetch DEX volumes */
    {
        RespBuf resp = http_get(DL_API "/overview/dexs?excludeTotalDataChart=true&excludeTotalDataChartBreakdown=true&dataType=dailyVolume");
        json_error_t err;
        json_t *root = json_loads(resp.data, 0, &err);
        free(resp.data);

        if (root) {
            json_t *dexs = json_object_get(root, "protocols");
            if (dexs && json_is_array(dexs)) {
                int n = (int)json_array_size(dexs);
                int limit = n < MAX_DEXS ? n : MAX_DEXS;
                for (int i = 0; i < limit; i++) {
                    json_t *d = json_array_get(dexs, i);
                    const char *name = json_string_value(json_object_get(d, "name"));
                    if (!name) continue;
                    /* The TVL field in DEX response is usually at different depth */
                    double vol = json_number_value(json_object_get(d, "total1dVolume"));

                    char safe[64], source[128], json_data[256];
                    sanitize(safe, name, 60);
                    snprintf(source, sizeof(source), "defillama_dex_%s", safe);
                    snprintf(json_data, sizeof(json_data),
                        "{\"dex\":\"%s\",\"volume_24h_usd\":%.0f}", name, vol);

                    db_insert(source, now, "defi", json_data);
                    total++;
                }
            }
        }
        json_decref(root);
    }

    db_close();
    curl_global_cleanup();
    printf("[DL] %d defi sources updated\n", total);
    return 0;
}
