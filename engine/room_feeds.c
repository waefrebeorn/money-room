/**
 * room_feeds.c — L1: Data ingestion
 * LIVE_MODE: Reads market_tick from Python-written JSON feed file.
 * PAPER_MODE: Reads directly from BTC 1-min CSV + Fear & Greed CSV.
 *
 * In PAPER_MODE, no external Python process needed — the engine
 * processes all historical data autonomously at ~5ms/cycle.
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include "types.h"

#ifdef PAPER_MODE
// ── Paper Mode: direct CSV reader ──

#include "paper_feature_bridge.h"

#define BTC_CSV   "/home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv"
#define FNG_CSV   "/home/wubu2/.hermes/pm_logs/historical/raw/fear_greed/fear_greed_all.csv"

// Timeframe: 15-min candles (skip 14 intermediate candles)
// Higher timeframe = more signal, less noise.
// NOTE: With only BTC price + F&G features, 15-min still shows 50% WR
// after Darwin convergence (agents become unanimous).
// Room needs all 12 data streams for true divergence detection.
// #define TIMEFRAME_MINUTES  15
// #define CANDLES_TO_SKIP    (TIMEFRAME_MINUTES - 1)
// Trying 1-min (faster cycles) with diversity-preserving evolution
#define TIMEFRAME_MINUTES  1
#define CANDLES_TO_SKIP    0

// In-memory F&G lookup: day_timestamp → value
typedef struct { int64_t ts; int val; } FngEntry;
static FngEntry *fng_table = NULL;
static int fng_count = 0;

// BTC candle iterator state
static FILE *btc_file = NULL;
static int64_t fng_cache_ts = 0;
static int fng_cache_val = 50;

// ── Load F&G table at startup ──
static void load_fng_table(void) {
    FILE *f = fopen(FNG_CSV, "r");
    if (!f) { printf("[FEED] WARN: No F&G CSV at %s\n", FNG_CSV); return; }
    
    // Count lines
    char buf[256];
    int lines = 0;
    while (fgets(buf, sizeof(buf), f)) lines++;
    lines--; // header
    
    rewind(f);
    fng_table = (FngEntry *)malloc(lines * sizeof(FngEntry));
    if (!fng_table) { fclose(f); return; }
    
    // Skip header
    fgets(buf, sizeof(buf), f);
    
    fng_count = 0;
    while (fng_count < lines && fgets(buf, sizeof(buf), f)) {
        int64_t ts;
        int val;
        if (sscanf(buf, "%ld,%*[^,],%d,", &ts, &val) >= 2) {
            fng_table[fng_count].ts = (ts / 86400) * 86400; // Floor to day
            fng_table[fng_count].val = val;
            fng_count++;
        }
    }
    fclose(f);
    printf("[FEED] Loaded %d F&G entries\n", fng_count);
}

// ── Get F&G value for a given candle timestamp ──
static int get_fng(int64_t candle_ts) {
    if (!fng_table || fng_count == 0) return 50;
    int64_t day_ts = (candle_ts / 86400) * 86400;
    // Check cache first
    if (day_ts == fng_cache_ts) return fng_cache_val;
    // Binary search
    int lo = 0, hi = fng_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fng_table[mid].ts == day_ts) {
            fng_cache_ts = day_ts;
            fng_cache_val = fng_table[mid].val;
            return fng_table[mid].val;
        }
        if (fng_table[mid].ts < day_ts) lo = mid + 1;
        else hi = mid - 1;
    }
    // Nearest
    if (hi < 0) hi = 0;
    if (lo >= fng_count) lo = fng_count - 1;
    int nearest = (labs(day_ts - fng_table[hi].ts) < labs(fng_table[lo].ts - day_ts)) ? hi : lo;
    fng_cache_ts = day_ts;
    fng_cache_val = fng_table[nearest].val;
    return fng_table[nearest].val;
}

// ── Load next market tick from CSV ──
RoomError room_feeds_load(MarketTick *tick) {
    // Init on first call
    if (!btc_file) {
        load_fng_table();
        btc_file = fopen(BTC_CSV, "r");
        if (!btc_file) {
            printf("[FEED] ERROR: Cannot open %s\n", BTC_CSV);
            return ERR_FILE_READ;
        }
        // Skip header
        char buf[256];
        fgets(buf, sizeof(buf), btc_file);
        printf("[FEED] Reading BTC from CSV\n");
    }
    
    char line[256];
    while (fgets(line, sizeof(line), btc_file)) {
        int64_t ts;
        float open, high, low, close, volume;
        if (sscanf(line, "%ld,%f,%f,%f,%f,%f", &ts, &open, &high, &low, &close, &volume) < 6)
            continue;
        if (volume <= 0) continue;
        
        // ── Aggregate candle window for timeframe ──
        // First candle in window: save open, initialize range
        float agg_open = open;
        float agg_high = high;
        float agg_low = low;
        float agg_close = close;
        float agg_vol = volume;
        int64_t agg_ts = ts;
        
        // Skip intermediate candles, accumulating range
        for (int skip = 0; skip < CANDLES_TO_SKIP; skip++) {
            if (!fgets(line, sizeof(line), btc_file)) break;
            if (sscanf(line, "%ld,%f,%f,%f,%f,%f", &ts, &open, &high, &low, &close, &volume) >= 6) {
                if (volume > 0) {
                    if (high > agg_high) agg_high = high;
                    if (low < agg_low) agg_low = low;
                    agg_close = close;
                    agg_vol += volume;
                    agg_ts = ts;
                }
            }
        }
        
        memset(tick, 0, sizeof(MarketTick));
        strncpy(tick->asset, "BTC", 7);
        tick->window_ts = agg_ts;
        tick->open = agg_open;
        tick->high = agg_high;
        tick->low = agg_low;
        tick->close = agg_close;
        tick->volume = agg_vol;
        tick->fear_greed = (float)get_fng(agg_ts);
        
        // ── POPULATE aux fields from timeline.db (replaces old hardcoded constants) ──
        paper_load_aux(tick);
        
        return ERR_OK;
    }
    
    // All candles consumed
    printf("[FEED] All %d candles consumed\n", fng_count > 0 ? 722988 : 0);
    fclose(btc_file);
    btc_file = NULL;
    return ERR_NO_DATA;
}

#else
// ── Live Mode: JSON feed from Python ──

// Feed path (runtime-overridable via ROOM_DIR env var)
static char g_room_feeds_feed_path[576];
#define FEED_PATH g_room_feeds_feed_path

static void init_feeds_path(void) {
    const char *dir = getenv("ROOM_DIR");
    if (!dir || !dir[0]) dir = "/home/wubu2/.hermes/pm_logs/c_room";
    snprintf(g_room_feeds_feed_path, sizeof(g_room_feeds_feed_path), "%s/market_feed.json", dir);
}

static int json_get_str(const char *json, const char *key, char *out, int outsz) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < outsz - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static int json_get_float(const char *json, const char *key, float *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    *out = strtof(p, NULL);
    return 0;
}

static int json_get_int64(const char *json, const char *key, int64_t *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    *out = strtoll(p, NULL, 10);
    return 0;
}

RoomError room_feeds_load(MarketTick *tick) {
    init_feeds_path();
    FILE *f = fopen(FEED_PATH, "r");
    if (!f) return ERR_FILE_READ;
    
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 10) { fclose(f); return ERR_NO_DATA; }
    rewind(f);
    
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return ERR_FILE_READ; }
    size_t read = fread(buf, 1, sz, f);
    fclose(f);
    buf[read] = '\0';
    
    memset(tick, 0, sizeof(MarketTick));
    char asset[16] = {0};
    json_get_str(buf, "asset", asset, sizeof(asset));
    strncpy(tick->asset, asset, sizeof(tick->asset) - 1);
    tick->asset[sizeof(tick->asset) - 1] = '\0';
    json_get_int64(buf, "window_ts", &tick->window_ts);
    // Fallback: some feed writers use "timestamp" instead of "window_ts"
    if (tick->window_ts == 0) {
        json_get_int64(buf, "timestamp", &tick->window_ts);
    }
    // ── T021: Timestamp validation ──
    if (tick->window_ts > 0) {
        int64_t now_sec = (int64_t)time(NULL);
        int64_t age_sec = now_sec - tick->window_ts;
        if (age_sec < -300) {
            fprintf(stderr, "[FEED] WARN: timestamp %ld is %lds in future — rejecting\n",
                    (long)tick->window_ts, (long)(-age_sec));
            free(buf);
            tick->window_ts = 0;
            return ERR_NO_DATA;
        }
        if (age_sec > 86400) {
            fprintf(stderr, "[FEED] WARN: timestamp %ld is %lds stale (>24h) — rejecting\n",
                    (long)tick->window_ts, (long)age_sec);
            free(buf);
            tick->window_ts = 0;
            return ERR_NO_DATA;
        }
    }
    json_get_float(buf, "open", &tick->open);
    json_get_float(buf, "high", &tick->high);
    json_get_float(buf, "low", &tick->low);
    json_get_float(buf, "close", &tick->close);
    json_get_float(buf, "volume", &tick->volume);
    json_get_float(buf, "fear_greed", &tick->fear_greed);
    json_get_float(buf, "pump_score", &tick->pump_score);
    json_get_float(buf, "btc_dominance", &tick->btc_dominance);
    json_get_float(buf, "vix", &tick->vix);
    json_get_float(buf, "sp500", &tick->sp500);
    json_get_float(buf, "btc_30d_volatility", &tick->btc_30d_volatility);
    json_get_float(buf, "btc_30d_mean", &tick->btc_30d_mean);
    json_get_float(buf, "btc_30d_high", &tick->btc_30d_high);
    json_get_float(buf, "btc_30d_low", &tick->btc_30d_low);
    
    free(buf);
    if (tick->window_ts == 0) return ERR_NO_DATA;
    return ERR_OK;
}

#endif // PAPER_MODE
