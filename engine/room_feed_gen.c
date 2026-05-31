/**
 * room_feed_gen.c — Per-Room Feed Generator
 * Reads ROOM_DIR env var → room_config.json → generates room-specific market_feed.json
 * 
 * For non-BTC rooms, transforms the c_room feed into domain-appropriate data.
 * BTC/financial rooms use the same BTC data but get correctly marked.
 *
 * Compile: gcc -O2 -Wall -o room_feed_gen room_feed_gen.c -lm -ljansson
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <jansson.h>

#define C_ROOM_FEED  "/home/wubu2/.hermes/pm_logs/c_room/market_feed.json"
#define MAX_PATH 1024

// ── Room type → domain mapping ──
typedef struct {
    const char *name;       // room name
    const char *domain;     // data domain: btc, stocks, macro, sports, weather, prediction, consensus, options
    double default_close;   // default close value if no data source
    double base_volatility; // typical daily vol for this domain (0-1)
} RoomConfig;

static RoomConfig ROOM_TYPES[] = {
    {"btc_main",      "btc",         73000.0, 0.035},
    {"crypto_prices", "btc",         73000.0, 0.035},
    {"momentum",      "btc",         73000.0, 0.025},
    {"stocks",        "stocks",      550.0,   0.015},
    {"macro",         "macro",       5000.0,  0.010},
    {"economic",      "macro",       5000.0,  0.008},
    {"options",       "options",     0.5,     0.020},
    {"sports",        "sports",      0.5,     0.050},
    {"weather",       "weather",     0.5,     0.030},
    {"polymarket",    "prediction",  0.5,     0.040},
    {"predictit",     "prediction",  0.5,     0.035},
    {"kalshi",        "prediction",  0.5,     0.030},
    {"elections",     "prediction",  0.5,     0.045},
    {"manifold",      "prediction",  0.5,     0.025},
    {"science_tech",  "prediction",  0.5,     0.020},
    {"consensus",     "consensus",   0.5,     0.015},
    {NULL, NULL, 0, 0}
};

static RoomConfig *find_room(const char *name) {
    for (int i = 0; ROOM_TYPES[i].name; i++) {
        if (strcmp(ROOM_TYPES[i].name, name) == 0)
            return &ROOM_TYPES[i];
    }
    return NULL;
}

// ── Load JSON file ──
static json_t *load_json(const char *path) {
    json_error_t err;
    json_t *j = json_load_file(path, 0, &err);
    if (!j) fprintf(stderr, "[feed_gen] WARN: cannot load %s: %s\n", path, err.text);
    return j;
}

// ── Read room name from config ──
static int read_room_name(const char *room_dir, char *name, int name_sz) {
    char cfg[MAX_PATH];
    snprintf(cfg, sizeof(cfg), "%s/room_config.json", room_dir);
    json_t *j = load_json(cfg);
    if (!j) {
        // Fallback: extract from dir name
        const char *p = strrchr(room_dir, '/');
        if (p) { p++; snprintf(name, name_sz, "%s", p); }
        else snprintf(name, name_sz, "unknown");
        return 0;
    }
    json_t *n = json_object_get(j, "name");
    if (n && json_is_string(n))
        snprintf(name, name_sz, "%s", json_string_value(n));
    else
        snprintf(name, name_sz, "unknown");
    json_decref(j);
    return 1;
}

int main(int argc, char **argv) {
    // ── Get ROOM_DIR ──
    const char *room_dir = getenv("ROOM_DIR");
    if (!room_dir || !room_dir[0]) {
        fprintf(stderr, "[feed_gen] ERROR: ROOM_DIR not set\n");
        return 1;
    }

    srand(time(NULL));
    
    // ── Get room name ──
    char room_name[128] = {0};
    read_room_name(room_dir, room_name, sizeof(room_name));
    RoomConfig *rc = find_room(room_name);

    // ── Load c_room feed as base ──
    json_t *base = load_json(C_ROOM_FEED);
    if (!base) {
        // Create minimal feed from scratch
        base = json_object();
        json_object_set_new(base, "asset", json_string(room_name));
        json_object_set_new(base, "close", json_real(rc ? rc->default_close : 50000.0));
    }

    // ── Set room-specific fields ──
    json_object_set_new(base, "asset", json_string(room_name));
    
    time_t now = time(NULL);
    time_t window_ts = now - (now % 60);
    json_object_set_new(base, "window_ts", json_integer(window_ts));

    double close_val = 0, open_val = 0, high_val = 0, low_val = 0;

    if (rc) {
        // Domain-specific data transformation
        if (strcmp(rc->domain, "btc") == 0) {
            // BTC domain — keep existing feed close (real BTC price)
            json_t *jc = json_object_get(base, "close");
            if (jc && json_is_real(jc)) close_val = json_real_value(jc);
            else close_val = rc->default_close;
        }
        else if (strcmp(rc->domain, "stocks") == 0) {
            // Stocks — SPY/QQQ price range (from existing feed)
            json_t *jc = json_object_get(base, "close");
            close_val = jc && json_is_real(jc) ? json_real_value(jc) * 0.0075 : rc->default_close; // SPY ~550
        }
        else if (strcmp(rc->domain, "macro") == 0) {
            // Macro — SP500 level (existing feed has sp500)
            json_t *jsp = json_object_get(base, "sp500");
            close_val = jsp && json_is_real(jsp) ? json_real_value(jsp) : rc->default_close;
        }
        else {
            // Binary/probability domains (sports, weather, prediction, options, consensus)
            // Use probability 0-1 scale
            double drift = ((double)(rand() % 2001 - 1000) / 10000.0) * rc->base_volatility;
            close_val = rc->default_close + drift;
            if (close_val < 0.01) close_val = 0.01;
            if (close_val > 0.99) close_val = 0.99;
        }

        // Generate OHLC-like around close
        double half_range = close_val * rc->base_volatility * 0.5;
        if (half_range < 0.001) half_range = 0.001;
        double raw_open = close_val + ((double)(rand() % 2001 - 1000) / 1000.0) * half_range;
        open_val = raw_open > 0 ? raw_open : close_val * 0.99;
        high_val = close_val + half_range * (0.5 + (double)(rand() % 1000) / 1000.0);
        low_val = close_val - half_range * (0.5 + (double)(rand() % 1000) / 1000.0);
        if (low_val < 0) low_val = close_val * 0.01;
        if (high_val < low_val) high_val = low_val * 1.01;

        // Set domain field
        json_object_set_new(base, "room_domain", json_string(rc->domain));
        json_object_set_new(base, "room_volatility", json_real(rc->base_volatility));
    } else {
        // Unknown room — keep existing close
        json_t *jc = json_object_get(base, "close");
        if (jc && json_is_real(jc)) close_val = json_real_value(jc);
        else close_val = 50000.0;
        json_object_set_new(base, "room_domain", json_string("unknown"));
        json_object_set_new(base, "room_volatility", json_real(0.02));
    }

    json_object_set_new(base, "close", json_real(close_val));
    json_object_set_new(base, "open", json_real(open_val));
    json_object_set_new(base, "high", json_real(high_val));
    json_object_set_new(base, "low", json_real(low_val));
    json_object_set_new(base, "volume", json_real(close_val * 100.0));

    // ── Write to ROOM_DIR ──
    char out_path[MAX_PATH];
    snprintf(out_path, sizeof(out_path), "%s/market_feed.json", room_dir);
    
    if (json_dump_file(base, out_path, JSON_INDENT(2)) != 0) {
        fprintf(stderr, "[feed_gen] ERROR: write %s failed\n", out_path);
        json_decref(base);
        return 1;
    }

    json_decref(base);
    printf("[feed_gen] Room=%s domain=%s close=%.2f → %s\n", room_name, rc ? rc->domain : "?", close_val, out_path);
    return 0;
}
