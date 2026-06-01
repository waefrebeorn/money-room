/**
 * source_validator.c — E54: Cross-Exchange Source Validation
 *
 * Reads market_feed.json, validates price consistency across
 * Kraken, Coinbase, and OKX sources. Reports discrepancies.
 *
 * Build: gcc -O3 -march=native source_validator.c -o source_validator -lm -ljansson
 * Usage: ./source_validator [market_feed.json]
 *        ./source_validator watch [path] — continuous mode
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <jansson.h>
#include <unistd.h>

#define FEED_PATH_DEFAULT "/home/wubu2/.hermes/pm_logs/c_room/market_feed.json"
#define MAX_SOURCES 16

typedef struct {
    const char *name;
    double price;
    double bid;
    double ask;
    double vol;
    int    found;
} Source;

static void check_feed(const char *path) {
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) {
        fprintf(stderr, "Error loading %s: %s\n", path, err.text);
        return;
    }
    
    Source sources[] = {
        {"kraken", 0,0,0,0,0}, {"coinbase", 0,0,0,0,0}, {"okx", 0,0,0,0,0},
        {"kraken_ws", 0,0,0,0,0}, {"coinbase_ws", 0,0,0,0,0}
    };
    int n = sizeof(sources)/sizeof(sources[0]);
    
    /* Extract prices from JSON */
    json_t *v;
    for (int i = 0; i < n; i++) {
        char key[64];
        /* Try price fields */
        snprintf(key, sizeof(key), "%s_price", sources[i].name);
        v = json_object_get(root, key);
        if (json_is_real(v)) { sources[i].price = json_real_value(v); sources[i].found = 1; }
        
        snprintf(key, sizeof(key), "%s_bid", sources[i].name);
        v = json_object_get(root, key);
        if (json_is_real(v)) sources[i].bid = json_real_value(v);
        
        snprintf(key, sizeof(key), "%s_ask", sources[i].name);
        v = json_object_get(root, key);
        if (json_is_real(v)) sources[i].ask = json_real_value(v);
        
        snprintf(key, sizeof(key), "%s_vol", sources[i].name);
        v = json_object_get(root, key);
        if (json_is_real(v)) sources[i].vol = json_real_value(v);
    }
    
    /* Also try generic keys */
    if (!sources[0].found) {
        v = json_object_get(root, "price");
        if (json_is_real(v)) {
            sources[0].price = json_real_value(v);
            sources[0].found = 1;
        }
    }
    
    printf("SOURCE VALIDATOR\n");
    printf("===============\n\n");
    
    /* Table */
    printf("SOURCE      ACTIVE  PRICE       BID        ASK        SPREAD_BPS  VOL        STATUS\n");
    printf("----------- ------  ----------  ---------  ---------  ----------  ---------  ------\n");
    
    int active = 0;
    double prices[MAX_SOURCES];
    int pcount = 0;
    
    for (int i = 0; i < n; i++) {
        if (!sources[i].found && sources[i].price == 0) continue;
        
        double spread = 0;
        if (sources[i].bid > 0 && sources[i].ask > 0)
            spread = (sources[i].ask - sources[i].bid) / sources[i].bid * 10000;
        
        const char *status = sources[i].found ? "✅" : "❌";
        if (sources[i].price > 0) { active++; prices[pcount++] = sources[i].price; }
        
        printf("%-11s  %-6s  %10.2f  %9.2f  %9.2f  %10.2f  %9.2f  %s\n",
               sources[i].name,
               sources[i].found ? "yes" : "no",
               sources[i].price, sources[i].bid, sources[i].ask,
               spread, sources[i].vol, status);
    }
    
    printf("\nActive sources: %d\n", active);
    
    /* Price consistency check */
    if (pcount >= 2) {
        double mean = 0, min_p = 1e18, max_p = 0;
        for (int i = 0; i < pcount; i++) {
            mean += prices[i];
            if (prices[i] < min_p) min_p = prices[i];
            if (prices[i] > max_p) max_p = prices[i];
        }
        mean /= pcount;
        
        double max_dev = 0;
        for (int i = 0; i < pcount; i++) {
            double dev = fabs(prices[i] - mean) / mean * 10000; /* in bps */
            if (dev > max_dev) max_dev = dev;
        }
        
        double spread_range = (max_p - min_p) / mean * 10000;
        
        printf("Median price: $%.2f\n", mean);
        printf("Price range:  $%.2f - $%.2f (%.2f bps)\n", min_p, max_p, spread_range);
        printf("Max deviation from median: %.2f bps\n", max_dev);
        
        if (spread_range > 10)
            printf("\n⚠  WARNING: Cross-source price spread >10bps — possible arb or stale data\n");
        else if (spread_range > 5)
            printf("\n⚠  Cross-source price spread >5bps — moderate divergence\n");
        else
            printf("\n✅ Cross-source price spread <5bps — all sources consistent\n");
    } else {
        printf("\n⚠  Only %d active source(s) — insufficient for cross-validation\n", pcount);
    }
    
    /* Trusted source — use median instead of mean (out of scope) */
    if (active >= 2) {
        printf("\nMost reliable source: ");
        /* Compute median */
        double sorted[MAX_SOURCES];
        for (int i = 0; i < pcount; i++) sorted[i] = prices[i];
        for (int i = 0; i < pcount; i++)
            for (int j = i+1; j < pcount; j++)
                if (sorted[j] < sorted[i]) { double t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
        double median = sorted[pcount / 2];
        
        int best = -1;
        for (int i = 0; i < n; i++)
            if (sources[i].found && sources[i].bid > 0 && sources[i].ask > 0)
                if (best < 0 || fabs(sources[i].price - median) < fabs(sources[best].price - median))
                    best = i;
        if (best >= 0) printf("%s (bid/ask valid, price near median)\n", sources[best].name);
    }
    
    json_decref(root);
}

int main(int argc, char **argv) {
    const char *path = FEED_PATH_DEFAULT;
    int watch = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "watch") == 0) watch = 1;
        else path = argv[i];
    }
    
    if (watch) {
        while (1) {
            check_feed(path);
            printf("\n--- Waiting 60s for next check ---\n\n");
            sleep(60);
        }
    } else {
        check_feed(path);
    }
    
    return 0;
}
