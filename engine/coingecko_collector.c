/**
 * coingecko_collector.c — C100: CoinGecko Crypto Price Collector
 *
 * Fetches top crypto prices from CoinGecko free API.
 * Writes to timeline.db with source='coingecko_<id>'.
 *
 * Build: gcc -O2 -o coingecko_collector coingecko_collector.c -lcurl -ljansson -lsqlite3
 * Usage: ./coingecko_collector
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <jansson.h>

#define CG_API "https://api.coingecko.com/api/v3"
#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"

static const char *COINS[] = {
    "bitcoin", "ethereum", "solana", "cardano", "ripple",
    "polkadot", "avalanche-2", "chainlink", "dogecoin", "uniswap",
    "tron", "litecoin", "bitcoin-cash", "stellar", "monero",
    "cosmos", "filecoin", "aptos", "sui", "arbitrum",
    "optimism", "polygon-pos", "avalanche-2", "near", "internet-computer",
    NULL
};

typedef struct { char *data; size_t len; size_t cap; } RespBuf;

static size_t write_cb(void *p, size_t s, size_t n, void *u) {
    size_t t = s*n; RespBuf *r = u;
    if (r->len + t >= r->cap) {
        r->cap = r->len + t + 65536;
        r->data = realloc(r->data, r->cap);
    }
    memcpy(r->data + r->len, p, t);
    r->len += t; r->data[r->len] = 0;
    return t;
}

static RespBuf http_get(const char *url) {
    RespBuf r = {calloc(1, 65536), 0, 65536};
    CURL *c = curl_easy_init();
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "coingecko-collector/1.0");
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
    const char *sql = "INSERT OR REPLACE INTO timeline (ts, source, category, data, collected_at) "
                      "VALUES (?,?,?,?,?)";
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

/* Slugify a coin ID for use in source name */
static void slugify(const char *in, char *out, int max) {
    int j = 0;
    for (int i = 0; in[i] && j < max-1; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out[j++] = c;
        else if (c == '-') out[j++] = '_';
    }
    out[j] = 0;
}

int main(void) {
    printf("[CG] CoinGecko Collector\n");

    curl_global_init(CURL_GLOBAL_DEFAULT);
    db_init();

    /* Phase 1: Fetch individual prices */
    char url[4096];
    int offset = 0;
    int total = 0;

    while (COINS[offset]) {
        /* Build comma-separated list of 10 coins */
        int n = 0;
        char ids[2048] = {0};
        for (int i = offset; COINS[i] && n < 10; i++, n++) {
            if (n > 0) strncat(ids, ",", sizeof(ids) - strlen(ids) - 1);
            strncat(ids, COINS[i], sizeof(ids) - strlen(ids) - 1);
        }
        if (n == 0) break;

        snprintf(url, sizeof(url), "%s/simple/price?ids=%s&vs_currencies=usd&include_24hr_vol=true&include_market_cap=true",
                 CG_API, ids);

        RespBuf resp = http_get(url);
        json_error_t err;
        json_t *root = json_loads(resp.data, 0, &err);
        free(resp.data);

        if (!root) {
            printf("  Batch %d: JSON error\n", offset / 10);
            offset += n;
            continue;
        }

        const char *key;
        json_t *val;
        json_object_foreach(root, key, val) {
            double price = json_number_value(json_object_get(val, "usd"));
            double mcap = json_number_value(json_object_get(val, "usd_market_cap"));
            double vol = json_number_value(json_object_get(val, "usd_24h_vol"));

            char source[128], safe_key[64];
            slugify(key, safe_key, 60);
            snprintf(source, sizeof(source), "coingecko_%s", safe_key);

            char json_data[512];
            snprintf(json_data, sizeof(json_data),
                "{\"id\":\"%s\",\"price_usd\":%.4f,\"market_cap_usd\":%.0f,\"volume_24h_usd\":%.0f}",
                key, price, mcap, vol);

            db_insert(source, (long long)time(NULL), "crypto", json_data);

            /* Also insert as price-only source for simpler queries */
            char price_source[128];
            snprintf(price_source, sizeof(price_source), "coingecko_price_%s", safe_key);
            char price_json[256];
            snprintf(price_json, sizeof(price_json),
                "{\"id\":\"%s\",\"price_usd\":%.4f}", key, price);
            db_insert(price_source, (long long)time(NULL), "crypto", price_json);

            total++;
        }

        json_decref(root);
        offset += n;

        /* Rate limit: 1s between batches */
        if (COINS[offset]) {
            struct timespec d = {1, 0};
            nanosleep(&d, NULL);
        }
    }

    /* Phase 2: Fetch global market data */
    {
        snprintf(url, sizeof(url), "%s/global", CG_API);
        RespBuf resp = http_get(url);
        json_error_t err;
        json_t *root = json_loads(resp.data, 0, &err);
        free(resp.data);

        if (root) {
            json_t *data = json_object_get(root, "data");
            if (data) {
                double total_mcap = json_number_value(json_object_get(data, "total_market_cap"));
                double btc_dom = json_number_value(json_object_get(data, "market_cap_percentage"));
                int active_cryptos = (int)json_number_value(json_object_get(data, "active_cryptocurrencies"));

                json_t *mcap_obj = json_object_get(data, "total_market_cap");
                double usd_mcap = 0;
                if (mcap_obj) usd_mcap = json_number_value(json_object_get(mcap_obj, "usd"));

                char json_data[512];
                snprintf(json_data, sizeof(json_data),
                    "{\"total_market_cap_usd\":%.0f,\"btc_dominance_pct\":%.2f,"
                    "\"active_cryptocurrencies\":%d}",
                    usd_mcap, btc_dom, active_cryptos);

                db_insert("coingecko_global", (long long)time(NULL), "crypto", json_data);
                total++;
            }
            json_decref(root);
        }
    }

    db_close();
    curl_global_cleanup();
    printf("[CG] %d crypto sources updated\n", total);
    return 0;
}
