/*
 * whale_feat.c — P39: Whale Wallet Tracking
 *
 * Tracks large BTC transactions using BlockCypher free API.
 * Computes whale activity, large tx volume, and accumulation signals.
 *
 * Build: gcc -O3 -march=native whale_feat.c -o whale_feat -lcurl -ljansson -lm
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

#define HOME_DIR "/home/wubu2"
#define OUT_FILE HOME_DIR "/.hermes/options_cache/whale_features.json"
#define CURL_TIMEOUT 15L

typedef struct { char *data; size_t len; } HttpBuf;
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    HttpBuf *b = (HttpBuf*)user; size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0; memcpy(p + b->len, ptr, n);
    b->data = p; b->len += n; b->data[b->len] = '\0'; return n;
}

static json_t *http_get(const char *url) {
    CURL *curl = curl_easy_init(); if (!curl) return NULL;
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
    json_error_t err; json_t *root = json_loads(buf.data, 0, &err);
    free(buf.data); return root;
}

static double parse_dbl(const char *s) {
    if (!s) return 0;
    return strtod(s, NULL);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Fetch blockchain stats from blockchain.info */
    json_t *stats = http_get("https://blockchain.info/stats?format=json");
    if (!stats) { fprintf(stderr, "Failed blockchain stats\n"); curl_global_cleanup(); return 1; }

    double total_sent_24h = json_number_value(json_object_get(stats, "total_btc_sent")) / 1e8;
    double n_tx_24h = json_number_value(json_object_get(stats, "n_tx"));
    double difficulty_t = json_number_value(json_object_get(stats, "difficulty")) / 1e12;
    double hash_rate = json_number_value(json_object_get(stats, "hash_rate"));
    double trade_vol = json_number_value(json_object_get(stats, "trade_volume_usd"));
    json_decref(stats);

    /* Fetch recent transactions from BlockCypher (free, no key) */
    json_t *txs = http_get("https://api.blockcypher.com/v1/btc/main/txs?limit=10");
    if (!txs) { fprintf(stderr, "Failed BlockCypher\n"); curl_global_cleanup(); return 1; }

    /* Scan recent transactions for whales */
    size_t n_txs = json_array_size(txs);
    int count_gt_50 = 0, count_gt_100 = 0, count_gt_500 = 0;
    double vol_gt_10 = 0, vol_gt_100 = 0, max_tx_btc = 0;

    for (size_t i = 0; i < n_txs; i++) {
        json_t *tx = json_array_get(txs, i);
        json_t *outputs = json_object_get(tx, "outputs");
        if (!json_is_array(outputs)) continue;
        size_t n_out = json_array_size(outputs);
        for (size_t j = 0; j < n_out; j++) {
            json_t *out = json_array_get(outputs, j);
            double val = parse_dbl(json_string_value(json_object_get(out, "value")));
            double btc = val / 1e8;
            if (btc > max_tx_btc) max_tx_btc = btc;
            if (btc > 10) vol_gt_10 += btc;
            if (btc > 100) { vol_gt_100 += btc; count_gt_100++; }
            if (btc > 50) count_gt_50++;
            if (btc > 500) count_gt_500++;
        }
    }
    json_decref(txs);

    /* Fetch mempool unconfirmed count */
    json_t *mp = http_get("https://api.blockcypher.com/v1/btc/main");
    int unconfirmed = 0;
    if (mp) {
        unconfirmed = json_integer_value(json_object_get(mp, "unconfirmed_count"));
        json_decref(mp);
    }

    /* Compute whale signals */
    /* F43: Large tx count — how many >100 BTC recent (0-10 scale) */
    double large_tx_ratio = n_txs > 0 ? (double)count_gt_100 / n_txs : 0;

    /* F44: Whale activity score — composite of large tx volume + mempool */
    double avg_tx_size_btc = n_tx_24h > 0 ? total_sent_24h / n_tx_24h : 0.01;
    double whale_vol_norm = vol_gt_100 / 1000.0; /* normalize to 0-1 (cap at 1000 BTC) */
    if (whale_vol_norm > 1.0) whale_vol_norm = 1.0;
    double mempool_pressure = unconfirmed / 50000.0; /* typical 10-50K */
    if (mempool_pressure > 1.0) mempool_pressure = 1.0;
    double whale_activity = (whale_vol_norm * 0.6 + mempool_pressure * 0.4);

    /* F45: Accumulation signal — avg tx size deviation from baseline */
    double baseline_avg = 0.5; /* typical avg tx ~0.5 BTC */
    double acc_signal = (avg_tx_size_btc - baseline_avg) / 5.0; /* -1 to ~+1 */
    if (acc_signal < -1) acc_signal = -1;
    if (acc_signal > 1) acc_signal = 1;
    double acc_signal_norm = (acc_signal + 1.0) / 2.0; /* -1..1 -> 0..1 */

    /* Hash rate signal (miner activity proxy) */
    double hash_rate_eh = hash_rate / 1e15;
    double hash_signal = hash_rate_eh > 500 ? 1.0 : hash_rate_eh / 500.0; /* normalize to ~500EH baseline */

    json_t *root = json_object();
    json_object_set_new(root, "large_tx_count_100btc", json_integer(count_gt_100));
    json_object_set_new(root, "large_tx_count_500btc", json_integer(count_gt_500));
    json_object_set_new(root, "max_tx_btc", json_real(max_tx_btc));
    json_object_set_new(root, "whale_vol_100btc", json_real(vol_gt_100));
    json_object_set_new(root, "large_tx_ratio", json_real(large_tx_ratio));
    json_object_set_new(root, "whale_activity", json_real(whale_activity));
    json_object_set_new(root, "mempool_unconfirmed", json_integer(unconfirmed));
    json_object_set_new(root, "mempool_pressure", json_real(mempool_pressure));
    json_object_set_new(root, "avg_tx_size_btc", json_real(avg_tx_size_btc));
    json_object_set_new(root, "total_sent_24h_btc", json_real(total_sent_24h));
    json_object_set_new(root, "acc_signal_norm", json_real(acc_signal_norm));
    json_object_set_new(root, "hash_rate_eh", json_real(hash_rate_eh));
    json_object_set_new(root, "hash_signal", json_real(hash_signal));
    json_object_set_new(root, "difficulty_t", json_real(difficulty_t));
    json_object_set_new(root, "source", json_string("blockcypher+blockchain_info"));

    FILE *f = fopen(OUT_FILE, "w");
    if (f) { json_dumpf(root, f, JSON_INDENT(2) | JSON_SORT_KEYS); fclose(f); }
    json_decref(root);

    printf("Wrote %s\n", OUT_FILE);
    printf("  >100BTC txs: %d | Max tx: %.2f BTC | Whale vol: %.2f BTC\n",
           count_gt_100, max_tx_btc, vol_gt_100);
    printf("  Whale activity: %.3f | Mempool: %d | Avg tx: %.4f BTC\n",
           whale_activity, unconfirmed, avg_tx_size_btc);
    printf("  Hash rate: %.1f EH/s | Difficulty: %.1fT | Acc signal: %.3f\n",
           hash_rate_eh, difficulty_t, acc_signal_norm);

    curl_global_cleanup();
    return 0;
}
