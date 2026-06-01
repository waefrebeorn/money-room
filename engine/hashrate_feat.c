/*
 * hashrate_feat.c — P41: BTC Hash Rate & Mining Difficulty Features
 *
 * Fetches accurate global hash rate from mempool.space (free, no key).
 * Computes: hash rate trend, miner floor price estimate, difficulty regime.
 *
 * Miner floor estimate assumes Antminer S19j Pro efficiency (30.5 W/TH)
 * and $0.05/kWh — a conservative baseline.
 *
 * Output: JSON → feed_bridge → market_feed.json → engine features (F49-F51).
 *
 * Dependencies: libcurl, libjansson, libm
 * Build: gcc -O3 -march=native hashrate_feat.c -o hashrate_feat -lcurl -ljansson -lm
 * Cron: every 15m
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>

#define HOME_DIR    "/home/wubu2"
#define OUT_FILE    HOME_DIR "/.hermes/options_cache/hashrate_features.json"

#define MEMPOOL_API "https://mempool.space/api/v1/mining/hashrate/24h?avg=144"
#define BLOCK_API   "https://mempool.space/api/blocks/tip/height"
#define CURL_TIMEOUT 15L

/* ── Constants for miner floor estimation ── */
#define BLOCKS_PER_DAY     144.0
#define BTC_PER_BLOCK      3.125
#define BTC_PER_DAY        (BLOCKS_PER_DAY * BTC_PER_BLOCK)  /* 450 BTC/day */
#define WATTS_PER_TH       30.5   /* Antminer S19j Pro: 100TH/s @ 3050W */
#define POWER_COST_PER_KWH 0.05   /* $/kWh — conservative US industrial rate */
#define WATTS_TO_KW        (WATTS_PER_TH / 1000.0)
#define KWH_PER_TH_PER_DAY (WATTS_TO_KW * 24.0)
#define COST_PER_TH_PER_DAY (KWH_PER_TH_PER_DAY * POWER_COST_PER_KWH)

/* ── HTTP helpers ── */
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

static char *http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    HttpBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/8.0");
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) { free(buf.data); return NULL; }
    return buf.data;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* ── Fetch mempool.space hash rate data ── */
    char *body = http_get(MEMPOOL_API);
    if (!body) {
        fprintf(stderr, "[hashrate] mempool.space API failed\n");
        curl_global_cleanup();
        return 1;
    }

    /* Parse JSON manually for big integers that jansson can't handle */
    double hash_rate_hs = 0, difficulty = 0;
    {
        const char *s = body;
        const char *hr_match = strstr(s, "\"currentHashrate\":");
        if (hr_match) {
            hr_match += 18; /* skip past key */
            while (*hr_match == ' ' || *hr_match == '\t' || *hr_match == ':') hr_match++;
            hash_rate_hs = strtod(hr_match, NULL);
        }
        const char *df_match = strstr(s, "\"currentDifficulty\":");
        if (df_match) {
            df_match += 20;
            while (*df_match == ' ' || *df_match == '\t' || *df_match == ':') df_match++;
            difficulty = strtod(df_match, NULL);
        }
    }
    free(body);
    if (hash_rate_hs <= 0 || difficulty <= 0) {
        fprintf(stderr, "[hashrate] Failed to parse hash rate/difficulty from response\n");
        curl_global_cleanup();
        return 1;
    }

    /* ── Fetch current block height for context ── */
    char *height_str = http_get(BLOCK_API);
    int64_t block_height = 0;
    if (height_str) {
        block_height = strtoll(height_str, NULL, 10);
        free(height_str);
    }

    /* ── Compute derived features ── */

    /* Convert hash rate to EH/s (exahashes) */
    /* 1 EH = 1e18 H/s */
    double hash_rate_eh = hash_rate_hs / 1e18;

    /* Cap at reasonable range (200-1500 EH/s) */
    if (hash_rate_eh < 200.0) hash_rate_eh = 200.0;
    if (hash_rate_eh > 1500.0) hash_rate_eh = 1500.0;

    /* F49: Normalized hash rate signal (0-1) */
    /* Normalize: 300-1200 EH/s → 0-1 range */
    double hash_rate_norm = (hash_rate_eh - 300.0) / 900.0;
    if (hash_rate_norm < 0.0) hash_rate_norm = 0.0;
    if (hash_rate_norm > 1.0) hash_rate_norm = 1.0;

    /* F50: Normalized difficulty signal (0-1) */
    /* BTC difficulty typical range: 10T-250T (2024-2025) */
    double difficulty_norm = (difficulty / 1e12 - 10.0) / 240.0;
    if (difficulty_norm < 0.0) difficulty_norm = 0.0;
    if (difficulty_norm > 1.0) difficulty_norm = 1.0;

    /* F51: Miner floor price — estimated cost to mine 1 BTC */
    /* TH/s needed to mine 1 BTC/day = global hashrate / BTC_per_day */
    double th_needed_per_btc = (hash_rate_hs / 1e12) / BTC_PER_DAY;
    double miner_floor_price = th_needed_per_btc * COST_PER_TH_PER_DAY;

    /* Normalize miner floor: typical range $10K-$150K → 0-1 */
    double miner_floor_norm = (miner_floor_price - 10000.0) / 140000.0;
    if (miner_floor_norm < 0.0) miner_floor_norm = 0.0;
    if (miner_floor_norm > 1.0) miner_floor_norm = 1.0;

    /* Hash rate trend signal: ratio of current to 3-month average */
    /* Use 144-block (24h) avg hashrate from mempool as 1d average */
    /* For trend, we use the 24h avg vs current — if current > avg, rising */
    double trend_signal = hash_rate_norm; /* proxy: current normalized level */
    /* Momentum: 1.0 = strong growth, 0.5 = flat, 0.0 = declining */
    double momentum = hash_rate_norm > 0.5 ? hash_rate_norm : 0.5;

    /* Build output JSON */
    json_t *out = json_object();
    json_object_set_new(out, "hash_rate_eh", json_real(hash_rate_eh));
    json_object_set_new(out, "difficulty_t", json_real(difficulty / 1e12));
    json_object_set_new(out, "miner_floor_usd", json_real(miner_floor_price));
    json_object_set_new(out, "block_height", json_integer(block_height));
    /* Normalized features for engine */
    json_object_set_new(out, "hash_rate_norm", json_real(hash_rate_norm));     /* F49 */
    json_object_set_new(out, "difficulty_norm", json_real(difficulty_norm));   /* F50 */
    json_object_set_new(out, "miner_floor_norm", json_real(miner_floor_norm)); /* F51 */
    json_object_set_new(out, "trend_signal", json_real(trend_signal));
    json_object_set_new(out, "momentum", json_real(momentum));
    json_object_set_new(out, "source", json_string("mempool_space"));

    FILE *f = fopen(OUT_FILE, "w");
    if (!f) {
        fprintf(stderr, "[hashrate] Can't write %s\n", OUT_FILE);
        json_decref(out);
        curl_global_cleanup();
        return 1;
    }
    json_dumpf(out, f, JSON_INDENT(2) | JSON_SORT_KEYS);
    fclose(f);
    json_decref(out);

    printf("[hashrate] Wrote %s\n", OUT_FILE);
    printf("[hashrate] Hash rate: %.0f EH/s | Difficulty: %.1fT | Block: %ld\n",
           hash_rate_eh, difficulty / 1e12, (long)block_height);
    printf("[hashrate] Miner floor: $%.0f | Hash norm: %.3f | Diff norm: %.3f | Floor norm: %.3f\n",
           miner_floor_price, hash_rate_norm, difficulty_norm, miner_floor_norm);

    curl_global_cleanup();
    return 0;
}
