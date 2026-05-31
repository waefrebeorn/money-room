/**
 * multi_market_trainer.c — Train agent populations across ALL market types
 * 
 * Self-contained: loads CSV/DB data, trains agents, outputs genomes.
 * 
 * Compile:
 *   gcc -O3 -o multi_market_trainer multi_market_trainer.c -lsqlite3 -ljansson -lm -I.
 * Usage:
 *   ./multi_market_trainer [--epochs N] [--agents N]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <jansson.h>
#include "types.h"

// ════════════════════════════════════════════════════════════
//  SECTION 1: MARKET TYPES & DATA LOADING
// ════════════════════════════════════════════════════════════

const char *MARKET_TYPE_NAMES[] = {
    "CRYPTO", "EQUITY", "FOREX", "COMMODITY", "BOND",
    "VOLATILITY", "PREDICTION", "SPORTS", "WEATHER", "ELECTION"
};

#define STOCKS_DIR   "/home/wubu2/.hermes/pm_logs/historical/raw/stocks"
#define FOREX_DIR    "/home/wubu2/.hermes/pm_logs/historical/raw/forex"
#define HIST_DIR     "/home/wubu2/.hermes/pm_logs/historical"
#define PM_DB        "/home/wubu2/.hermes/pm_logs/historical/polymarket_events.db"
#define BTC_CSV      "/home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv"

// Market data: rows of OHLCV
typedef struct {
    MarketType  type;
    char        name[32];
    char        asset[8];
    int64_t    *timestamps;
    double     *opens, *highs, *lows, *closes;
    double     *volumes;
    double    **feats;         // Full feature matrix [n_rows][N_FEATURES] for binary markets
    int         n_rows;
    int         capacity;
} MarketData;

// Collection of markets
typedef struct {
    MarketData markets[20];
    int        n_markets;
} MarketDataSet;

// Polymarket prediction event
typedef struct {
    int64_t timestamp;
    int     outcome;
    double  true_probability;
    double  volume;
    int     category_id;
    double  features[20];
} PMEvent;

typedef struct {
    PMEvent *events;
    int      n_events;
    int      capacity;
} PMEventSet;

// Helper: parse date string to unix timestamp
static int64_t date_to_ts(const char *date_str) {
    struct tm tm = {0};
    char buf[64];
    strncpy(buf, date_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    // Strip timezone info and time portion
    char *t = strchr(buf, 'T');
    if (t) *t = '\0';
    t = strchr(buf, ' ');
    if (t) *t = '\0';
    if (sscanf(buf, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) >= 3) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
    }
    tm.tm_hour = 12;
    return (int64_t)mktime(&tm);
}

// Grow market data buffer
static int md_grow(MarketData *md, int min_rows) {
    if (min_rows <= md->capacity) return 0;
    int new_cap = min_rows + 2000;
    md->timestamps = realloc(md->timestamps, new_cap * sizeof(int64_t));
    md->opens      = realloc(md->opens,      new_cap * sizeof(double));
    md->highs      = realloc(md->highs,      new_cap * sizeof(double));
    md->lows       = realloc(md->lows,       new_cap * sizeof(double));
    md->closes     = realloc(md->closes,     new_cap * sizeof(double));
    md->volumes    = realloc(md->volumes,    new_cap * sizeof(double));
    md->feats      = realloc(md->feats,      new_cap * sizeof(double *));
    if (!md->timestamps || !md->opens || !md->highs || !md->lows || !md->closes || !md->volumes || !md->feats)
        return -1;
    // Zero new feat pointers
    for (int i = md->capacity; i < new_cap; i++) md->feats[i] = NULL;
    md->capacity = new_cap;
    return 0;
}

static int md_add(MarketData *md, int64_t ts, double o, double h, double l, double c, double v) {
    if (md_grow(md, md->n_rows + 1) != 0) return -1;
    int i = md->n_rows++;
    md->timestamps[i] = ts;
    md->opens[i] = o;
    md->highs[i] = h;
    md->lows[i] = l;
    md->closes[i] = c;
    md->volumes[i] = v;
    md->feats[i] = NULL;  // Not a binary row
    return 0;
}

// Store a row with full N_FEATURES feature vector (binary markets)
static int md_add_full(MarketData *md, int64_t ts, const double *f, double outcome) {
    if (md_grow(md, md->n_rows + 1) != 0) return -1;
    int i = md->n_rows++;
    md->timestamps[i] = ts;
    md->opens[i] = outcome;  // Keep outcome in opens for backward compat (build_features)
    md->highs[i] = 0;
    md->lows[i] = 0;
    md->closes[i] = outcome;
    md->volumes[i] = 0;
    // Allocate and copy full feature vector
    md->feats[i] = malloc(N_FEATURES * sizeof(double));
    if (!md->feats[i]) return -1;
    memcpy(md->feats[i], f, N_FEATURES * sizeof(double));
    return 0;
}

static void md_init(MarketData *md, MarketType type, const char *name, const char *asset) {
    memset(md, 0, sizeof(MarketData));
    md->type = type;
    strncpy(md->name, name, sizeof(md->name) - 1);
    strncpy(md->asset, asset, sizeof(md->asset) - 1);
    md->feats = NULL;
}

// Load Yahoo-finance CSV: Date,Open,High,Low,Close,Volume,...
static int load_yahoo_csv(const char *path, MarketData *md) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[DATA] Cannot open %s\n", path); return -1; }
    char buf[1024];
    int line_no = 0, loaded = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line_no++;
        if (line_no == 1) continue;
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        if (len == 0) continue;
        char date_str[64] = {0};
        double o = 0, h = 0, l = 0, c = 0, v = 0;
        if (sscanf(buf, "%63[^,],%lf,%lf,%lf,%lf,%lf", date_str, &o, &h, &l, &c, &v) >= 6) {
            if (md_add(md, date_to_ts(date_str), o, h, l, c, v) == 0) loaded++;
        }
    }
    fclose(f);
    fprintf(stderr, "[DATA] %s: %d rows\n", path + strlen(path) - 20, loaded);
    return loaded;
}

// Load FRED CSV: observation_date,value
static int load_fred_csv(const char *path, MarketData *md) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[DATA] Cannot open %s\n", path); return -1; }
    char buf[256];
    int line_no = 0, loaded = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line_no++;
        if (line_no == 1) continue;
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        if (len == 0) continue;
        char date_str[32] = {0}; double val = 0;
        if (sscanf(buf, "%31[^,],%lf", date_str, &val) >= 2) {
            if (md_add(md, date_to_ts(date_str), val, val, val, val, 0) == 0) loaded++;
        }
    }
    fclose(f);
    fprintf(stderr, "[DATA] %s: %d rows\n", path + strlen(path) - 20, loaded);
    return loaded;
}

// Load BTC 1-min CSV: ts,open,high,low,close,volume
static int load_btc_1min(MarketData *md) {
    FILE *f = fopen(BTC_CSV, "r");
    if (!f) { fprintf(stderr, "[DATA] Cannot open %s\n", BTC_CSV); return -1; }
    char buf[256]; int loaded = 0;
    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        if (len == 0) continue;
        int64_t ts; double o, h, l, c, v;
        if (sscanf(buf, "%ld,%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c, &v) >= 6) {
            if (md_add(md, ts, o, h, l, c, v) == 0) loaded++;
        }
    }
    fclose(f);
    fprintf(stderr, "[DATA] BTC 1-min: %d rows\n", loaded);
    return loaded;
}

// Load equity indices
static int load_equity(MarketDataSet *ds) {
    struct { const char *name, *asset, *file; } tbl[] = {
        {"SP500","SP500","sp500.csv"}, {"DOW","DOW","DOW_daily.csv"},
        {"NASDAQ","NASDAQ","NASDAQ_daily.csv"}, {"FTSE100","FTSE","FTSE100_daily.csv"},
        {"NIKKEI","N225","NIKKEI_daily.csv"}
    };
    int n = 5, total = 0;
    for (int i = 0; i < n; i++) {
        if (ds->n_markets >= 16) break;
        MarketData *md = &ds->markets[ds->n_markets];
        md_init(md, MARKET_EQUITY, tbl[i].name, tbl[i].asset);
        char path[512];
        if (strcmp(tbl[i].name, "SP500") == 0)
            snprintf(path, sizeof(path), "%s/sp500.csv", HIST_DIR);
        else
            snprintf(path, sizeof(path), "%s/%s", STOCKS_DIR, tbl[i].file);
        int r = (strcmp(tbl[i].name, "SP500") == 0) ? load_fred_csv(path, md) : load_yahoo_csv(path, md);
        if (r > 0) { ds->n_markets++; total++; }
    }
    return total;
}

// Load forex
static int load_forex(MarketDataSet *ds) {
    const char *pairs[] = {"EURUSD", "GBPUSD", "USDJPY"};
    int total = 0;
    for (int i = 0; i < 3; i++) {
        if (ds->n_markets >= 16) break;
        MarketData *md = &ds->markets[ds->n_markets];
        md_init(md, MARKET_FOREX, pairs[i], pairs[i]);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_daily.csv", FOREX_DIR, pairs[i]);
        if (load_yahoo_csv(path, md) > 0) { ds->n_markets++; total++; }
    }
    return total;
}

// Load commodities
static int load_commodities(MarketDataSet *ds) {
    const char *names[] = {"GOLD","SILVER","CRUDE_OIL"};
    const char *assets[] = {"XAU","XAG","WTI"};
    int total = 0;
    for (int i = 0; i < 3; i++) {
        if (ds->n_markets >= 16) break;
        MarketData *md = &ds->markets[ds->n_markets];
        md_init(md, MARKET_COMMODITY, names[i], assets[i]);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_daily.csv", STOCKS_DIR, names[i]);
        if (load_yahoo_csv(path, md) > 0) { ds->n_markets++; total++; }
    }
    return total;
}

// Load bonds
static int load_bonds(MarketDataSet *ds) {
    if (ds->n_markets >= 16) return 0;
    MarketData *md = &ds->markets[ds->n_markets];
    md_init(md, MARKET_BOND, "DGS10", "DGS10");
    char path[512];
    snprintf(path, sizeof(path), "%s/DGS10_daily.csv", STOCKS_DIR);
    if (load_fred_csv(path, md) > 0) { ds->n_markets++; return 1; }
    return 0;
}

// Load VIX
static int load_vix(MarketDataSet *ds) {
    if (ds->n_markets >= 16) return 0;
    MarketData *md = &ds->markets[ds->n_markets];
    md_init(md, MARKET_VOLATILITY, "VIX", "VIX");
    char path[512];
    snprintf(path, sizeof(path), "%s/VIX_daily.csv", STOCKS_DIR);
    if (load_yahoo_csv(path, md) > 0) { ds->n_markets++; return 1; }
    return 0;
}

// Load BTC crypto
static int load_crypto(MarketDataSet *ds) {
    if (ds->n_markets >= 16) return 0;
    MarketData *md = &ds->markets[ds->n_markets];
    md_init(md, MARKET_CRYPTO, "BTC", "BTC");
    if (load_btc_1min(md) > 0) { ds->n_markets++; return 1; }
    return 0;
}

// Load Polymarket events
static PMEventSet *load_polymarket(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(PM_DB, &db) != SQLITE_OK) { fprintf(stderr, "[DATA] Cannot open %s\n", PM_DB); return NULL; }
    int count = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM polymarket_events", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (count == 0) { sqlite3_close(db); return NULL; }
    PMEventSet *set = calloc(1, sizeof(PMEventSet));
    set->events = calloc(count, sizeof(PMEvent));
    set->capacity = count;
    const char *sql = "SELECT timestamp, outcome, true_probability, volume, category_id, "
                       "feat_0,feat_1,feat_2,feat_3,feat_4,feat_5,feat_6,feat_7,"
                       "feat_8,feat_9,feat_10,feat_11,feat_12,feat_13,feat_14,"
                       "feat_15,feat_16,feat_17,feat_18,feat_19 "
                       "FROM polymarket_events WHERE outcome IS NOT NULL";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db); free(set->events); free(set); return NULL;
    }
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < count) {
        set->events[idx].timestamp = sqlite3_column_int64(stmt, 0);
        set->events[idx].outcome = sqlite3_column_int(stmt, 1);
        set->events[idx].true_probability = sqlite3_column_double(stmt, 2);
        set->events[idx].volume = sqlite3_column_double(stmt, 3);
        set->events[idx].category_id = sqlite3_column_int(stmt, 4);
        for (int f = 0; f < 20; f++) set->events[idx].features[f] = sqlite3_column_double(stmt, 5 + f);
        idx++;
    }
    set->n_events = idx;
    sqlite3_finalize(stmt); sqlite3_close(db);
    fprintf(stderr, "[DATA] Polymarket: %d events\n", set->n_events);
    return set;
}

// Free all market data
static void free_md(MarketDataSet *ds) {
    for (int i = 0; i < ds->n_markets; i++) {
        free(ds->markets[i].timestamps);
        free(ds->markets[i].opens);
        free(ds->markets[i].highs);
        free(ds->markets[i].lows);
        free(ds->markets[i].closes);
        free(ds->markets[i].volumes);
        // Free feature rows
        if (ds->markets[i].feats) {
            for (int r = 0; r < ds->markets[i].n_rows; r++)
                free(ds->markets[i].feats[r]);
            free(ds->markets[i].feats);
        }
    }
    ds->n_markets = 0;
}

// ── Binary market definitions ──
#define OUT_DIR     "/home/wubu2/money-room/data"
#define GENOME_OUT  "/home/wubu2/money-room/data/trained_genomes.json"

static double f_idx_ge(double *f, int start, int end) {
    double val = 0;
    int count = 0;
    for (int i = start; i < end && i < N_FEATURES; i++) { val += f[i]; count++; }
    return count > 0 ? val / count : 0.5;
}

// Forward declarations
static int load_binary_markets(MarketDataSet *ds);

// Load ALL available markets (OHLCV + binary)
static int load_all(MarketDataSet *ds) {
    memset(ds, 0, sizeof(MarketDataSet));
    load_crypto(ds);
    load_equity(ds);
    load_forex(ds);
    load_commodities(ds);
    load_bonds(ds);
    load_vix(ds);
    load_binary_markets(ds);  // Load sports, weather, prediction markets
    return ds->n_markets;
}

// ── BINARY MARKET LOADERS (sports, weather, prediction) ──

// Store binary features in OHLCV fields for compatibility:
//   opens[i]..feature[0..4]  highs[i]..feature[5..9]
//   lows[i]..feature[10..14]  closes[i] = outcome (0/1)
//   volumes[i] = 0 (unused)

// Load sports JSON from file
static int load_sports_json(MarketData *md) {
    char path[512];
    snprintf(path, sizeof(path), "%s/multi_market/sports_data.json", OUT_DIR);
    
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root || !json_is_array(root)) {
        fprintf(stderr, "[DATA] Cannot load sports: %s (%s)\n", path, err.text);
        return -1;
    }
    
    int n = (int)json_array_size(root);
    int loaded = 0;
    
    for (int i = 0; i < n; i++) {
        json_t *ev = json_array_get(root, i);
        if (!ev) continue;
        
        json_t *jfeats = json_object_get(ev, "features");
        json_t *joutcome = json_object_get(ev, "outcome");
        json_t *jts = json_object_get(ev, "timestamp");
        json_t *jleague = json_object_get(ev, "league");
        
        if (!jfeats || !joutcome || !jts || !json_is_array(jfeats)) continue;
        
        double outcome = json_number_value(joutcome);
        int64_t ts = json_integer_value(jts);
        const char *league = jleague ? json_string_value(jleague) : "sports";
        
        // Pack features into OHLCV fields
        double f[N_FEATURES];
        for (int f_idx = 0; f_idx < N_FEATURES; f_idx++) {
            if (f_idx < (int)json_array_size(jfeats))
                f[f_idx] = json_number_value(json_array_get(jfeats, f_idx));
            else
                f[f_idx] = 0.5;
        }
        
        // Store full feature vector for training
        if (md_add_full(md, ts, f, outcome) == 0) loaded++;
    }
    
    json_decref(root);
    fprintf(stderr, "[DATA] Sports: %d games\n", loaded);
    return loaded;
}

// Load weather JSON (single combined market, all cities)
static int load_weather_json(MarketData *md) {
    char path[512];
    snprintf(path, sizeof(path), "%s/multi_market/weather_data.json", OUT_DIR);
    
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root || !json_is_array(root)) {
        fprintf(stderr, "[DATA] Cannot load weather: %s (%s)\n", path, err.text);
        return -1;
    }
    
    int n = (int)json_array_size(root);
    int loaded = 0;
    
    for (int i = 0; i < n; i++) {
        json_t *ev = json_array_get(root, i);
        if (!ev) continue;
        
        json_t *jfeats = json_object_get(ev, "features");
        json_t *joutcome = json_object_get(ev, "outcome");
        json_t *jts = json_object_get(ev, "timestamp");
        const char *city = json_string_value(json_object_get(ev, "city"));
        
        if (!jfeats || !joutcome || !jts || !json_is_array(jfeats) || !city) continue;
        
        double outcome = json_number_value(joutcome);
        int64_t ts = json_integer_value(jts);
        
        // Pack features into OHLCV fields
        double f[N_FEATURES] = {0};
        for (int f_idx = 0; f_idx < N_FEATURES && f_idx < (int)json_array_size(jfeats); f_idx++)
            f[f_idx] = json_number_value(json_array_get(jfeats, f_idx));
        
        // Store full feature vector for training
        if (md_add_full(md, ts, f, outcome) == 0) loaded++;
    }
    
    json_decref(root);
    fprintf(stderr, "[DATA] Weather: %d entries\n", loaded);
    return loaded;
}

// Load prediction markets from SQLite
static int load_prediction_market(MarketData *md) {
    sqlite3 *db = NULL;
    if (sqlite3_open(PM_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "[DATA] Cannot open %s\n", PM_DB);
        return -1;
    }
    
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT timestamp, outcome, volume, feat_0,feat_1,feat_2,feat_3,feat_4,"
                       "feat_5,feat_6,feat_7,feat_8,feat_9,feat_10,feat_11,feat_12,"
                       "feat_13,feat_14,feat_15,feat_16,feat_17,feat_18,feat_19 "
                       "FROM polymarket_events WHERE outcome IS NOT NULL";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db); return -1;
    }
    
    int loaded = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t ts = sqlite3_column_int64(stmt, 0);
        double outcome = sqlite3_column_double(stmt, 1);
        double volume = sqlite3_column_double(stmt, 2);
        
        double f0 = sqlite3_column_double(stmt, 3);
        double f1 = sqlite3_column_double(stmt, 4);
        double f2 = sqlite3_column_double(stmt, 5);
        double f3 = sqlite3_column_double(stmt, 6);
        double f4 = sqlite3_column_double(stmt, 7);
        double f5 = sqlite3_column_double(stmt, 8);
        
        // Build full feature array
        double feats[N_FEATURES];
        memset(feats, 0, sizeof(feats));
        feats[0] = f0; feats[1] = f1; feats[2] = f2;
        feats[3] = f3; feats[4] = f4; feats[5] = f5;
        
        if (md_add_full(md, ts, feats, outcome) == 0) loaded++;
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    fprintf(stderr, "[DATA] Polymarket: %d events\n", loaded);
    return loaded;
}

// Load ALL binary markets (sports, weather, prediction)
static int load_binary_markets(MarketDataSet *ds) {
    int total = 0;
    
    // Sports
    if (ds->n_markets < MAX_MARKETS) {
        MarketData *s = &ds->markets[ds->n_markets];
        md_init(s, MARKET_SPORTS, "SPORTS", "GAMES");
        if (load_sports_json(s) > 0) { ds->n_markets++; total++; }
    }
    
    // Weather
    if (ds->n_markets < MAX_MARKETS) {
        MarketData *w = &ds->markets[ds->n_markets];
        md_init(w, MARKET_WEATHER, "WEATHER", "WX");
        if (load_weather_json(w) > 0) { ds->n_markets++; total++; }
    }
    
    // Prediction market
    if (ds->n_markets < MAX_MARKETS) {
        MarketData *pm = &ds->markets[ds->n_markets];
        md_init(pm, MARKET_PREDICTION, "PMARKET", "PM");
        if (load_prediction_market(pm) > 0) { ds->n_markets++; total++; }
    }
    
    return total;
}

// Print summary
static void print_summary(const MarketDataSet *ds) {
    printf("═══ MULTI-MARKET DATA ═══\nMarkets: %d\n", ds->n_markets);
    for (int i = 0; i < ds->n_markets; i++) {
        const MarketData *md = &ds->markets[i];
        printf("  [%s] %-8s %-8s %6d rows", MARKET_TYPE_NAMES[md->type], md->name, md->asset, md->n_rows);
        if (md->n_rows > 1) printf("  %.2f → %.2f", md->opens[0], md->closes[md->n_rows-1]);
        printf("\n");
    }
}

// Build a MarketTick from a MarketData candle
static void to_tick(const MarketData *md, int idx, MarketTick *tick) {
    memset(tick, 0, sizeof(MarketTick));
    strncpy(tick->asset, md->asset, sizeof(tick->asset) - 1);
    tick->market_type = md->type;
    tick->window_ts = md->timestamps[idx];
    tick->open = (float)md->opens[idx];
    tick->high = (float)md->highs[idx];
    tick->low = (float)md->lows[idx];
    tick->close = (float)md->closes[idx];
    tick->volume = (float)md->volumes[idx];
}

// ════════════════════════════════════════════════════════════
//  SECTION 2: TRAINER ENGINE
// ════════════════════════════════════════════════════════════

#define FEAT_SIZE N_FEATURES

// Feature builder from candle data
static void build_features(const MarketData *md, int idx, float *feats) {
    for (int i = 0; i < N_FEATURES; i++) feats[i] = 0.5f;
    if (idx < 10) return;
    
    // F0: Price position in 20-period range
    double hi = -1e9, lo = 1e9;
    int s = idx - 20; if (s < 0) s = 0;
    for (int i = s; i <= idx; i++) {
        if (md->highs[i] > hi) hi = md->highs[i];
        if (md->lows[i] < lo) lo = md->lows[i];
    }
    feats[0] = (hi - lo) > 0 ? (float)((md->closes[idx] - lo) / (hi - lo)) : 0.5f;
    
    // F1-F3: Returns
    if (idx > 0 && md->closes[idx-1] > 0)
        feats[1] = (float)((md->closes[idx] - md->closes[idx-1]) / md->closes[idx-1] * 100.0);
    if (idx >= 5 && md->closes[idx-5] > 0)
        feats[2] = (float)((md->closes[idx] - md->closes[idx-5]) / md->closes[idx-5] * 100.0);
    if (idx >= 20 && md->closes[idx-20] > 0)
        feats[3] = (float)((md->closes[idx] - md->closes[idx-20]) / md->closes[idx-20] * 100.0);
    
    // F4: Volume ratio (20-period avg)
    double vs = 0; int vc = 0;
    int vs2 = idx - 20; if (vs2 < 0) vs2 = 0;
    for (int i = vs2; i < idx; i++) { vs += md->volumes[i]; vc++; }
    double av = vc > 0 ? vs / vc : 0;
    feats[4] = av > 0 ? (float)(md->volumes[idx] / av) : 1.0f;
    if (feats[4] > 5.0f) feats[4] = 5.0f; feats[4] /= 5.0f;
    
    // F5: Volatility (10-period ATR / price)
    double atr = 0; int ac = 0;
    for (int i = idx - 10; i < idx; i++) {
        if (i > 0) { atr += fmax(md->highs[i] - md->lows[i], fmax(fabs(md->highs[i] - md->closes[i-1]), fabs(md->lows[i] - md->closes[i-1]))); ac++; }
    }
    atr = ac > 0 ? atr / ac : 0;
    feats[5] = md->closes[idx] > 0 ? (float)(atr / md->closes[idx] * 100.0) : 0;
    if (feats[5] > 10.0f) feats[5] = 10.0f; feats[5] /= 10.0f;
    
    // F6: RSI-like (14 period)
    if (idx >= 14) {
        double gains = 0, losses = 0;
        for (int i = idx - 13; i <= idx; i++) {
            double chg = md->closes[i] - md->closes[i-1];
            if (chg > 0) gains += chg; else losses -= chg;
        }
        double ag = gains / 14.0, al = losses / 14.0;
        feats[6] = al > 0 ? (float)(100.0 - 100.0 / (1.0 + ag / al)) / 100.0f : 1.0f;
    }
    
    // F7: EMA crossover
    if (idx >= 8) {
        double ef = md->closes[idx-7], es = md->closes[idx-7];
        for (int i = idx - 7; i <= idx; i++) {
            ef = 0.3 * md->closes[i] + 0.7 * ef;
            es = 0.1 * md->closes[i] + 0.9 * es;
        }
        double sp = es > 0 ? (ef - es) / es : 0;
        feats[7] = (float)(sp * 100.0);
        if (feats[7] > 1.0f) feats[7] = 1.0f;
        if (feats[7] < -1.0f) feats[7] = -1.0f;
        feats[7] = (feats[7] + 1.0f) / 2.0f;
    }
    
    // F8: Bollinger %B
    if (idx >= 20) {
        double sum = 0, sq = 0;
        for (int i = idx - 19; i <= idx; i++) { sum += md->closes[i]; sq += md->closes[i] * md->closes[i]; }
        double mn = sum / 20.0, sd = sqrt(fmax(0, sq / 20.0 - mn * mn));
        if (sd > 0) { feats[8] = (float)((md->closes[idx] - (mn - 2.0 * sd)) / (4.0 * sd)); if (feats[8] < 0) feats[8] = 0; if (feats[8] > 1) feats[8] = 1; }
    }
    
    // F9: Normalized volatility
    feats[9] = feats[5];  // Reuse ATR-based vol
    
    // F10-F16: Market type indicator (one-hot)
    feats[10] = md->type == MARKET_CRYPTO ? 1.0f : 0.0f;
    feats[11] = md->type == MARKET_EQUITY ? 1.0f : 0.0f;
    feats[12] = md->type == MARKET_FOREX ? 1.0f : 0.0f;
    feats[13] = md->type == MARKET_COMMODITY ? 1.0f : 0.0f;
    feats[14] = md->type == MARKET_BOND ? 1.0f : 0.0f;
    feats[15] = md->type == MARKET_VOLATILITY ? 1.0f : 0.0f;
    feats[16] = md->type == MARKET_PREDICTION ? 1.0f : 0.0f;
    
    // F17: Normalized fear-greed
    feats[17] = feats[1] > 0 ? 0.5f + feats[1] * 0.05f : 0.5f + feats[1] * 0.05f;
    if (feats[17] < 0) feats[17] = 0; if (feats[17] > 1) feats[17] = 1;
}

// Initialize one agent
static void init_agent(AgentState *a) {
    a->alive = true; a->capital = 50.0f; a->starting_capital = 50.0f;
    a->trades = 0; a->wins = 0; a->losses = 0; a->total_pnl = 0.0f;
    a->max_drawdown = 0.0f; a->peak_capital = 50.0f;
    a->consecutive_losses = 0; a->win_rate_ema = 0.5f; a->last_trade_window = -1;
    memset(a->hidden, 0, sizeof(a->hidden));
    a->genome.position_size = 0.01f + (float)rand() / RAND_MAX * 0.49f;
    a->genome.conviction_threshold = 0.05f + (float)rand() / RAND_MAX * 0.40f;
    a->genome.risk_tolerance = (float)rand() / RAND_MAX;
    a->genome.lie_sensitivity = 0.10f + (float)rand() / RAND_MAX * 0.88f;
    a->genome.herd_antipathy = (float)rand() / RAND_MAX;
    a->genome.stop_loss_pct = 0.01f + (float)rand() / RAND_MAX * 0.24f;
    a->genome.take_profit_pct = 0.01f + (float)rand() / RAND_MAX * 0.59f;
    a->genome.min_edge_pct = 0.5f + (float)rand() / RAND_MAX * 49.5f;
    a->genome.time_horizon = 0.1f + (float)rand() / RAND_MAX * 9.9f;
    a->genome.mean_reversion_bias = -1.0f + (float)rand() / RAND_MAX * 2.0f;
    a->genome.bias = ((float)rand() / RAND_MAX - 0.5f) * 0.3f;
    a->genome.learning_rate = 0.005f + (float)rand() / RAND_MAX * 0.015f;
    for (int w = 0; w < N_FEATURES; w++)
        a->genome.feat_weight[w] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f;
    for (int r = 0; r < N_REGS; r++) {
        for (int w = 0; w < N_FEATURES; w++)
            a->genome.regime_weight[r][w] = a->genome.feat_weight[w] + ((float)rand() / RAND_MAX - 0.5f) * 0.5f;
        a->genome.regime_bias[r] = a->genome.bias + ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
    }
}

// Agent vote
static int agent_vote(const AgentState *a, const float *feats, bool *dir, float *conv) {
    double score = a->genome.bias;
    for (int w = 0; w < N_FEATURES; w++) score += (feats[w] - 0.5f) * a->genome.feat_weight[w];
    double sig = 1.0 / (1.0 + exp(-score));
    *conv = (float)fabs(sig - 0.5) * 2.0f;
    if (*conv < a->genome.conviction_threshold) return 0;
    *dir = sig > 0.5;
    return 1;
}

// Per-market population
typedef struct {
    MarketType   market_type;
    char         name[32];
    AgentState  *agents;
    int          n_agents;
    int          n_cycles;
    int          n_resolved;
    float        best_wr;
    float        avg_wr;
    float        best_pnl;
    int          generation;
} MarketPop;

// Initialize population
static void init_pop(MarketPop *p, MarketType type, const char *name, int n) {
    memset(p, 0, sizeof(MarketPop));
    p->market_type = type; strncpy(p->name, name, sizeof(p->name) - 1);
    p->n_agents = n; p->agents = malloc(n * sizeof(AgentState));
    for (int i = 0; i < n; i++) init_agent(&p->agents[i]);
}

// Train one population on one market
static int train_market(MarketPop *pop, const MarketData *md) {
    if (!pop || !md || md->n_rows < 50) return 0;
    int n = pop->n_agents;
    int max_idx = md->n_rows - 1;
    int train_end = max_idx * 70 / 100;
    int start = train_end > 200 ? rand() % (train_end - 100) : 20;
    int cycles = train_end - start; if (cycles < 50) cycles = 50; if (cycles > 200) cycles = 200;
    
    // Detect binary outcome market
    int is_binary = (md->type == MARKET_SPORTS || md->type == MARKET_WEATHER || 
                     md->type == MARKET_PREDICTION || md->type == MARKET_ELECTION);
    
    int *tcount = calloc(n, sizeof(int));
    int *wcount = calloc(n, sizeof(int));
    
    // Reset agent stats for this epoch (tcount/wcount track per-epoch)
    for (int i = 0; i < n; i++) {
        if (pop->agents[i].alive) {
            pop->agents[i].trades = 0;
            pop->agents[i].wins = 0;
            pop->agents[i].losses = 0;
            pop->agents[i].total_pnl = 0.0f;
        }
    }
    
    for (int c = 0; c < cycles; c++) {
        int idx = start + c;
        if (idx >= max_idx) break;
        
        // Build features differently for binary vs OHLCV markets
        float feats[N_FEATURES];
        if (is_binary) {
            // Binary market: prefer full feature matrix, fallback to OHLCV compression
            if (md->feats && md->feats[idx]) {
                for (int i = 0; i < N_FEATURES; i++)
                    feats[i] = (float)md->feats[idx][i];
            } else {
                for (int i = 0; i < N_FEATURES; i++) feats[i] = 0.5f;
                feats[0] = (float)md->opens[idx];
                feats[1] = (float)md->highs[idx];
                feats[2] = (float)md->lows[idx];
                feats[17] = (float)(md->closes[idx] >= 1 ? 0.8f : 0.2f);
            }
        } else {
            build_features(md, idx, feats);
        }
        
        for (int i = 0; i < n; i++) {
            if (!pop->agents[i].alive) continue;
            bool dir; float conv;
            if (!agent_vote(&pop->agents[i], feats, &dir, &conv)) continue;
            
            // Resolve direction vs outcome
            bool won;
            if (is_binary) {
                // Binary: closes[idx] = 0 or 1 = actual outcome
                bool actual = md->closes[idx] >= 0.5;
                won = (dir == actual);
            } else {
                // OHLCV: up if close >= open
                bool was_up = md->closes[idx] >= md->opens[idx];
                won = (dir == was_up);
            }
            
            float pos = pop->agents[i].genome.position_size * pop->agents[i].capital;
            float pnl = won ? pos * 0.01f : -pos * 0.01f;
            
            tcount[i]++; if (won) wcount[i]++;
            pop->agents[i].capital += pnl;
            pop->agents[i].trades++;
            if (won) pop->agents[i].wins++; else pop->agents[i].losses++;
            pop->agents[i].total_pnl += pnl;
            if (pop->agents[i].capital < 1.0f) pop->agents[i].alive = false;
        }
        pop->n_cycles++;
    }
    
    int total_t = 0, total_w = 0;
    float wr_sum = 0; int alive = 0;
    float best_pnl = -1e9f;
    for (int i = 0; i < n; i++) {
        total_t += tcount[i]; total_w += wcount[i];
        if (pop->agents[i].total_pnl > best_pnl) best_pnl = pop->agents[i].total_pnl;
        if (tcount[i] > 5) { wr_sum += (float)wcount[i] / tcount[i]; alive++; }
    }
    pop->n_resolved += total_t;
    pop->best_pnl = best_pnl;
    pop->best_wr = total_t > 0 ? (float)total_w / total_t : 0.5f;
    pop->avg_wr = alive > 0 ? wr_sum / alive : 0.5f;
    
    free(tcount); free(wcount);
    return total_t;
}

// Darwin evolution
static void evolve(MarketPop *pop) {
    int n = pop->n_agents;
    typedef struct { int idx; float fit; } Fit;
    Fit *fits = malloc(n * sizeof(Fit));
    for (int i = 0; i < n; i++) {
        float wr = pop->agents[i].trades > 5 ? (float)pop->agents[i].wins / pop->agents[i].trades : 0.5f;
        fits[i].idx = i; fits[i].fit = wr * sqrtf((float)(pop->agents[i].trades + 1));
    }
    // Bubble sort descending
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (fits[j].fit > fits[i].fit) { Fit t = fits[i]; fits[i] = fits[j]; fits[j] = t; }
    
    int elite = n * 10 / 100;
    int mutate = n * 30 / 100;
    AgentState *new_a = malloc(n * sizeof(AgentState));
    // Copy elite preserving their stats for fitness tracking
    for (int ei = 0; ei < elite; ei++) {
        int src_idx = fits[ei].idx;
        memcpy(&new_a[ei], &pop->agents[src_idx], sizeof(AgentState));
    }
    int ni = elite;
    
    srand(time(NULL) ^ (int)(pop->generation * 12345));
    for (int i = 0; i < mutate && ni < n; i++) {
        int p = rand() % elite;
        memcpy(&new_a[ni], &new_a[p], sizeof(AgentState));
        int nm = 3 + rand() % 5;
        for (int m = 0; m < nm; m++) {
            int param = rand() % (N_FEATURES + 4);
            float mu = ((float)rand() / RAND_MAX - 0.5f) * 0.3f;
            if (param < N_FEATURES) {
                new_a[ni].genome.feat_weight[param] += mu;
                if (new_a[ni].genome.feat_weight[param] > 5.0f) new_a[ni].genome.feat_weight[param] = 5.0f;
                if (new_a[ni].genome.feat_weight[param] < -5.0f) new_a[ni].genome.feat_weight[param] = -5.0f;
            } else if (param < N_FEATURES + 2) {
                new_a[ni].genome.position_size += mu * 0.1f;
                if (new_a[ni].genome.position_size < 0.01f) new_a[ni].genome.position_size = 0.01f;
                if (new_a[ni].genome.position_size > 0.50f) new_a[ni].genome.position_size = 0.50f;
            } else {
                new_a[ni].genome.bias += mu * 0.1f;
            }
        }
        new_a[ni].capital = 50.0f; new_a[ni].alive = true;
        // Reset trade stats for clean tracking
        new_a[ni].trades = 0; new_a[ni].wins = 0; new_a[ni].losses = 0;
        new_a[ni].total_pnl = 0.0f;
        ni++;
    }
    while (ni < n) { init_agent(&new_a[ni]); ni++; }
    memcpy(pop->agents, new_a, n * sizeof(AgentState));
    free(new_a); free(fits);
    pop->generation++;
}

// Train all markets
static int train_all(MarketDataSet *ds, int n_agents, int epochs) {
    if (ds->n_markets == 0) return -1;
    MarketPop *pops = malloc(ds->n_markets * sizeof(MarketPop));
    for (int m = 0; m < ds->n_markets; m++)
        init_pop(&pops[m], ds->markets[m].type, ds->markets[m].name, n_agents);
    
    printf("\n═══ MULTI-MARKET TRAINING ═══\n");
    printf("Markets: %d  Agents/market: %d  Epochs: %d\n\n", ds->n_markets, n_agents, epochs);
    
    for (int e = 0; e < epochs; e++) {
        printf("─── Epoch %d/%d ───\n", e + 1, epochs);
        int total = 0;
        for (int m = 0; m < ds->n_markets; m++) {
            int t = train_market(&pops[m], &ds->markets[m]);
            total += t;
            printf("  [%s] %-8s %5d trades  WR=%5.1f%%  best=%5.1f%%  gen=%d\n",
                   MARKET_TYPE_NAMES[pops[m].market_type], pops[m].name,
                   pops[m].n_resolved, pops[m].avg_wr * 100, pops[m].best_wr * 100,
                   pops[m].generation);
        }
        for (int m = 0; m < ds->n_markets; m++) evolve(&pops[m]);
        printf("  Total: %d trades\n\n", total);
    }
    
    // Output
    printf("═══ RESULTS ═══\n\n");
    mkdir(OUT_DIR, 0755);
    
    // Write JSON
    FILE *out = fopen(GENOME_OUT, "w");
    if (out) {
        fprintf(out, "{\n  \"trainer\": \"multi_market\",\n  \"epochs\": %d,\n  \"markets\": %d,\n", epochs, ds->n_markets);
        fprintf(out, "  \"agents_per_market\": %d,\n  \"best_agents\": [\n", n_agents);
        for (int m = 0; m < ds->n_markets; m++) {
            int bi = 0; float bf = -1;
            for (int i = 0; i < pops[m].n_agents; i++) {
                float fit = pops[m].agents[i].trades > 5 ? (float)pops[m].agents[i].wins / pops[m].agents[i].trades : 0.5f;
                fit *= sqrtf((float)(pops[m].agents[i].trades + 1));
                if (fit > bf) { bf = fit; bi = i; }
            }
            fprintf(out, "    {\"market\":\"%s\",\"type\":\"%s\",\"wr\":%.3f,\"trades\":%d,\"wins\":%d,\"pnl\":%.2f,\"pos\":%.4f,\"bias\":%.4f}", 
                    pops[m].name, MARKET_TYPE_NAMES[pops[m].market_type],
                    pops[m].agents[bi].trades > 0 ? (float)pops[m].agents[bi].wins / pops[m].agents[bi].trades : 0,
                    pops[m].agents[bi].trades, pops[m].agents[bi].wins,
                    pops[m].agents[bi].total_pnl,
                    pops[m].agents[bi].genome.position_size,
                    pops[m].agents[bi].genome.bias);
            fprintf(out, "%s\n", (m < ds->n_markets - 1) ? "," : "");
            
            printf("  [%s] %-8s Best: WR=%.1f%% (%d/%d) PnL=$%.2f\n",
                   MARKET_TYPE_NAMES[pops[m].market_type], pops[m].name,
                   pops[m].agents[bi].trades > 0 ? (float)pops[m].agents[bi].wins / pops[m].agents[bi].trades * 100 : 0,
                   pops[m].agents[bi].wins, pops[m].agents[bi].trades,
                   pops[m].agents[bi].total_pnl);
        }
        fprintf(out, "  ],\n  \"summary\": {\"markets_trained\":%d,\"generations\":%d}\n}\n", ds->n_markets, pops[0].generation);
        fclose(out);
        printf("\n[TRAIN] Genomes: %s\n", GENOME_OUT);
    }
    
    // Write binary genomes per market
    char dpath[512]; snprintf(dpath, sizeof(dpath), "%s/multi_market", OUT_DIR); mkdir(dpath, 0755);
    for (int m = 0; m < ds->n_markets; m++) {
        int bi = 0; float bf = -1;
        for (int i = 0; i < pops[m].n_agents; i++) {
            float fit = pops[m].agents[i].trades > 5 ? (float)pops[m].agents[i].wins / pops[m].agents[i].trades : 0.5f;
            fit *= sqrtf((float)(pops[m].agents[i].trades + 1));
            if (fit > bf) { bf = fit; bi = i; }
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/multi_market/%s.bin", OUT_DIR, pops[m].name);
        FILE *bfp = fopen(path, "wb");
        if (bfp) { 
            fwrite(&pops[m].agents[bi].genome, sizeof(Genome), 1, bfp);
            int mtype = (int)pops[m].market_type;
            fwrite(&mtype, sizeof(int), 1, bfp);
            fclose(bfp); 
        }
    }
    printf("[TRAIN] Binary genomes: %s/multi_market/*.bin\n", OUT_DIR);
    
    for (int m = 0; m < ds->n_markets; m++) free(pops[m].agents);
    free(pops);
    return ds->n_markets;
}

// ════════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════════
int main(int argc, char **argv) {
    int n_agents = 500, epochs = 3;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--agents") == 0 && i+1 < argc) n_agents = atoi(argv[++i]);
        else if (strcmp(argv[i], "--epochs") == 0 && i+1 < argc) epochs = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--epochs N] [--agents N]\n", argv[0]);
            printf("  --agents N   Agents per market (default: 500)\n");
            printf("  --epochs N   Training epochs (default: 3)\n");
            return 0;
        }
    }
    
    srand(time(NULL));
    printf("[TRAIN] Multi-Market Trainer v2\n[%s] Agents: %d  Epochs: %d\n", __DATE__, n_agents, epochs);
    
    MarketDataSet ds;
    int n = load_all(&ds);
    if (n == 0) { fprintf(stderr, "[TRAIN] No markets loaded!\n"); return 1; }
    print_summary(&ds);
    
    int r = train_all(&ds, n_agents, epochs);
    free_md(&ds);
    printf("[TRAIN] Done. %d markets trained.\n", r);
    return r > 0 ? 0 : 1;
}
