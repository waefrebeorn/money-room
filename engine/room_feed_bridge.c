/**
 * room_feed_bridge.c — C Room Feed Bridge
 * Replaces room_feed_bridge.py (249 lines Python, runs every 60s).
 * 
 * Reads:  historical.db (BTC 1-min candle)
 *         timeline.db (SP500, VIX, BTC 30d stats, CoinGecko context)
 *         news/ (pump_score)
 * Fetches: alternative.me fear & greed API
 * Writes:  market_feed.json + heartbeat
 * 
 * Dependencies: libsqlite3, libcurl, libjansson
 * Compile: gcc -O3 -o room_feed_bridge room_feed_bridge.c -lsqlite3 -lcurl -ljansson -lm
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include "exchange_api.h"

// ── Paths ──
#define HISTORICAL_DB  "/home/wubu2/.hermes/pm_logs/historical/historical.db"
#define TIMELINE_DB    "/home/wubu2/.hermes/pm_logs/timeline.db"
#define C_ROOM_DIR     "/home/wubu2/.hermes/pm_logs/c_room"
#define ECO_DIR        "/home/wubu2/.hermes/pm_logs/eco"
#define NEWS_DIR       "/home/wubu2/.hermes/pm_logs/news"
#define HB_DIR         "/home/wubu2/.hermes/infra/heartbeats"

// ── HTTP Response Buffer ──
typedef struct {
    char *data;
    size_t len;
} http_buf_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    http_buf_t *buf = (http_buf_t*)userdata;
    char *newp = realloc(buf->data, buf->len + total + 1);
    if (!newp) return 0;
    buf->data = newp;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *http_get(const char *url, long timeout_sec) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    http_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "room-feed-bridge/1.0");
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[bridge] HTTP error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

// ── Read latest BTC candle from historical.db ──
static json_t *get_latest_btc_candle(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(HISTORICAL_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "[bridge] Can't open historical.db\n");
        return NULL;
    }
    
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT ts, open, high, low, close, volume "
                       "FROM candles_multi WHERE pair='BTC' AND interval=1 "
                       "ORDER BY ts DESC LIMIT 1";
    
    json_t *candle = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        candle = json_object();
        json_object_set_new(candle, "ts", json_integer(sqlite3_column_int64(stmt, 0)));
        json_object_set_new(candle, "open", json_real(sqlite3_column_double(stmt, 1)));
        json_object_set_new(candle, "high", json_real(sqlite3_column_double(stmt, 2)));
        json_object_set_new(candle, "low", json_real(sqlite3_column_double(stmt, 3)));
        json_object_set_new(candle, "close", json_real(sqlite3_column_double(stmt, 4)));
        json_object_set_new(candle, "volume", json_real(sqlite3_column_double(stmt, 5)));
    }
    
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return candle;
}

// ── Read pump_score from latest news run ──
static double get_latest_pump_score(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/runs", NEWS_DIR);
    
    // Find latest news_*.json file
    // Use shell: ls -t news_runs/*.json | head -1
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "ls -t %s/news_*.json 2>/dev/null | head -1", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0.0;
    
    char fpath[512];
    if (!fgets(fpath, sizeof(fpath), fp)) { pclose(fp); return 0.0; }
    pclose(fp);
    
    // Trim newline
    size_t len = strlen(fpath);
    while (len > 0 && (fpath[len-1] == '\n' || fpath[len-1] == '\r')) fpath[--len] = '\0';
    if (len == 0) return 0.0;
    
    // Read and parse JSON
    json_error_t err;
    json_t *root = json_load_file(fpath, 0, &err);
    if (!root) return 0.0;
    
    json_t *j_total = json_object_get(root, "total_articles");
    json_t *j_pump = json_object_get(root, "pump_articles");
    json_t *j_fear = json_object_get(root, "fear_articles");
    
    int total = j_total ? (int)json_integer_value(j_total) : 1;
    int pump = j_pump ? (int)json_integer_value(j_pump) : 0;
    int fear = j_fear ? (int)json_integer_value(j_fear) : 0;
    
    json_decref(root);
    
    if (total == 0) return 0.0;
    double score = (double)(pump - fear) / (double)total * 3.0;
    if (score > 1.0) score = 1.0;
    if (score < -1.0) score = -1.0;
    return score;
}

// ── Get fear & greed index from alternative.me API ──
static double get_fear_greed(void) {
    char *resp = http_get("https://api.alternative.me/fng/?limit=1", 5);
    if (!resp) return 50.0;
    
    json_error_t err;
    json_t *root = json_loads(resp, 0, &err);
    free(resp);
    if (!root) return 50.0;
    
    double fng = 50.0;
    json_t *jdata = json_object_get(root, "data");
    if (jdata && json_array_size(jdata) > 0) {
        json_t *first = json_array_get(jdata, 0);
        json_t *jval = json_object_get(first, "value");
        if (jval) fng = (double)json_integer_value(jval);
    }
    json_decref(root);
    return fng;
}

// ── Query timeline.db ──
typedef struct {
    double sp500;
    double vix;
    double btc_30d_vol;
    double btc_30d_mean;
    double btc_30d_high;
    double btc_30d_low;
    double crypto_market_cap;
    double btc_dominance;
    int    active_cryptos;
    int    has_data;
} TimelineContext;

static TimelineContext get_timeline_context(time_t now) {
    TimelineContext tc = {0, 18.0, 0, 0, 0, 0, 0, 62.0, 0, 0};
    
    sqlite3 *db = NULL;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) return tc;
    
    sqlite3_stmt *stmt = NULL;
    
    // SP500
    if (sqlite3_prepare_v2(db, 
            "SELECT data FROM timeline WHERE source='fred_sp500' "
            "AND json_extract(data, '$.value') IS NOT NULL "
            "ORDER BY ts DESC LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *d = (const char*)sqlite3_column_text(stmt, 0);
            if (d) {
                json_error_t err;
                json_t *j = json_loads(d, 0, &err);
                if (j) {
                    json_t *v = json_object_get(j, "value");
                    if (v) tc.sp500 = json_number_value(v);
                    json_decref(j);
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    
    // VIX
    if (sqlite3_prepare_v2(db,
            "SELECT data FROM timeline WHERE source='fred_vix' "
            "AND json_extract(data, '$.value') IS NOT NULL "
            "ORDER BY ts DESC LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *d = (const char*)sqlite3_column_text(stmt, 0);
            if (d) {
                json_error_t err;
                json_t *j = json_loads(d, 0, &err);
                if (j) {
                    json_t *v = json_object_get(j, "value");
                    if (v) tc.vix = json_number_value(v);
                    json_decref(j);
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    
    // BTC 30-day stats
    time_t month_ago = now - 86400 * 30;
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT json_extract(data, '$.close') as c FROM timeline "
        "WHERE source='bitstamp_1min' AND json_extract(data, '$.pair')='BTC' "
        "AND ts > %ld AND json_extract(data, '$.close') IS NOT NULL "
        "ORDER BY ts DESC LIMIT 43200", (long)month_ago);
    
    double *prices = NULL;
    int nprices = 0, cap = 1024;
    prices = malloc(cap * sizeof(double));
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            double c = sqlite3_column_double(stmt, 0);
            if (nprices >= cap) {
                cap *= 2;
                prices = realloc(prices, cap * sizeof(double));
            }
            prices[nprices++] = c;
        }
        sqlite3_finalize(stmt);
    }
    
    if (nprices > 20) {
        double sum = 0, sum_sq = 0;
        tc.btc_30d_high = prices[0];
        tc.btc_30d_low = prices[0];
        for (int i = 0; i < nprices; i++) {
            sum += prices[i];
            sum_sq += prices[i] * prices[i];
            if (prices[i] > tc.btc_30d_high) tc.btc_30d_high = prices[i];
            if (prices[i] < tc.btc_30d_low) tc.btc_30d_low = prices[i];
        }
        tc.btc_30d_mean = sum / nprices;
        double variance = sum_sq / nprices - tc.btc_30d_mean * tc.btc_30d_mean;
        tc.btc_30d_vol = (variance > 0) ? (sqrt(variance) / tc.btc_30d_mean * 100.0) : 0;
    }
    free(prices);
    
    // CoinGecko global data
    if (sqlite3_prepare_v2(db,
            "SELECT data FROM timeline WHERE source='coingecko_global' "
            "ORDER BY collected_at DESC LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *d = (const char*)sqlite3_column_text(stmt, 0);
            if (d) {
                json_error_t err;
                json_t *j = json_loads(d, 0, &err);
                if (j) {
                    json_t *mcap = json_object_get(j, "total_market_cap_usd");
                    if (mcap) tc.crypto_market_cap = json_number_value(mcap);
                    json_t *dom = json_object_get(j, "btc_dominance");
                    if (dom) tc.btc_dominance = json_number_value(dom);
                    json_t *act = json_object_get(j, "active_cryptocurrencies");
                    if (act) tc.active_cryptos = (int)json_integer_value(act);
                    json_decref(j);
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    
    sqlite3_close(db);
    tc.has_data = (nprices > 0) ? 1 : 0;
    return tc;
}

// ── Anti-consensus signal from worst/best 20% ecosystem agents ──
typedef struct {
    double anti_consensus;
    double anti_signal;
    double worst_wr;
    double best_wr;
} AntiConsensus;

static AntiConsensus get_anti_consensus(void) {
    AntiConsensus ac = {0.5, 0.0, 0.5, 0.5};
    
    char path[512];
    snprintf(path, sizeof(path), "%s/portfolios.json", ECO_DIR);
    
    json_error_t err;
    json_t *pfs = json_load_file(path, 0, &err);
    if (!pfs || !json_is_object(pfs)) {
        if (pfs) json_decref(pfs);
        return ac;
    }
    
    const char *key;
    json_t *val;
    int n = json_object_size(pfs);
    if (n < 100) { json_decref(pfs); return ac; }
    
    // Build arrays for sorting by PnL
    typedef struct { const char *key; double pnl; int wins; int total; } Agent;
    Agent *agents = malloc(n * sizeof(Agent));
    int idx = 0;
    
    json_object_foreach(pfs, key, val) {
        json_t *j_pnl = json_object_get(val, "total_pnl");
        json_t *j_wins = json_object_get(val, "wins");
        json_t *j_total = json_object_get(val, "total_trades");
        agents[idx].key = strdup(key);
        agents[idx].pnl = j_pnl ? json_number_value(j_pnl) : 0;
        agents[idx].wins = j_wins ? (int)json_integer_value(j_wins) : 0;
        agents[idx].total = j_total ? (int)json_integer_value(j_total) : 0;
        idx++;
    }
    
    // Sort by PnL (simple insertion sort — n is small)
    for (int i = 1; i < n; i++) {
        Agent tmp = agents[i];
        int j = i - 1;
        while (j >= 0 && agents[j].pnl > tmp.pnl) {
            agents[j+1] = agents[j];
            j--;
        }
        agents[j+1] = tmp;
    }
    
    // Worst 20%, best 20%
    int n_tail = n / 5;
    if (n_tail < 1) n_tail = 1;
    
    double worst_wins = 0, worst_total = 0;
    double best_wins = 0, best_total = 0;
    
    for (int i = 0; i < n_tail; i++) {
        worst_wins += agents[i].wins;
        worst_total += agents[i].total;
    }
    for (int i = n - n_tail; i < n; i++) {
        best_wins += agents[i].wins;
        best_total += agents[i].total;
    }
    
    ac.worst_wr = worst_total > 0 ? worst_wins / worst_total : 0.5;
    ac.best_wr = best_total > 0 ? best_wins / best_total : 0.5;
    ac.anti_consensus = 1.0 - ac.worst_wr;
    ac.anti_signal = (ac.anti_consensus - 0.5) * 2.0;
    
    for (int i = 0; i < n; i++) free((void*)agents[i].key);
    free(agents);
    json_decref(pfs);
    return ac;
}

// ── PID Controller (replaces MarketDynamicsEngine for enrichment) ──
typedef struct {
    double p, i, d;
    double integral;
    double prev_error;
    int initialized;
} PIDController;

static void pid_init(PIDController *pid, double kp, double ki, double kd) {
    pid->p = kp; pid->i = ki; pid->d = kd;
    pid->integral = 0;
    pid->prev_error = 0;
    pid->initialized = 0;
}

static double pid_update(PIDController *pid, double setpoint, double measured, double dt) {
    double error = setpoint - measured;
    pid->integral += error * dt;
    if (pid->integral > 10.0) pid->integral = 10.0;
    if (pid->integral < -10.0) pid->integral = -10.0;
    
    double derivative;
    if (pid->initialized) {
        derivative = (error - pid->prev_error) / dt;
    } else {
        derivative = 0;
        pid->initialized = 1;
    }
    
    pid->prev_error = error;
    return pid->p * error + pid->i * pid->integral + pid->d * derivative;
}

// ── Simple nested prediction: EWMA regime + momentum ──
static double nested_prediction(double close, double *ewma_fast, double *ewma_slow) {
    if (*ewma_fast == 0) {
        *ewma_fast = close;
        *ewma_slow = close;
        return 0.5;
    }
    *ewma_fast = 0.3 * close + 0.7 * (*ewma_fast);
    *ewma_slow = 0.05 * close + 0.95 * (*ewma_slow);
    
    double diff = (*ewma_fast - *ewma_slow) / *ewma_slow;
    double pred = 1.0 / (1.0 + exp(-diff * 5.0));  // Sigmoid of normalized diff
    if (pred < 0.01) pred = 0.01;
    if (pred > 0.99) pred = 0.99;
    return pred;
}

// ── Main ──
int main(int argc, char **argv) {
    time_t now = time(NULL);
    time_t window_ts = now - (now % 60);  // Round down to minute
    
    // ── Build feed ──
    json_t *feed = json_object();
    
    // BTC candle
    json_t *candle = get_latest_btc_candle();
    if (candle) {
        json_t *jts = json_object_get(candle, "ts");
        int64_t candle_ts = jts ? json_integer_value(jts) : 0;
        // Validate candle freshness — if stale (>5 min old), use current window_ts
        if (candle_ts > 0 && llabs(candle_ts - window_ts) < 300) {
            json_object_set(feed, "window_ts", jts);
        } else {
            json_object_set_new(feed, "window_ts", json_integer(window_ts));
        }
        // Check if candle has real price data (not zero from stale pipeline)
        json_t *jclose = json_object_get(candle, "close");
        double close_val = jclose ? json_number_value(jclose) : 0;
        if (close_val > 0) {
            json_object_set(feed, "asset", json_string("BTC"));
            json_object_set(feed, "open", json_object_get(candle, "open"));
            json_object_set(feed, "high", json_object_get(candle, "high"));
            json_object_set(feed, "low", json_object_get(candle, "low"));
            json_object_set(feed, "close", jclose);
            json_object_set(feed, "volume", json_object_get(candle, "volume"));
        }
        json_decref(candle);
    }
    // If candle had zero data (stale pipeline), use live exchange price
    // Check if close is still unset or zero
    json_t *jclose = json_object_get(feed, "close");
    if (!jclose || json_number_value(jclose) <= 0) {
        json_object_set_new(feed, "asset", json_string("BTC"));
        if (!jclose) json_object_set_new(feed, "window_ts", json_integer(window_ts));
        // Use Kraken price if available, else Coinbase
        double fallback_price = 0;
        ExchangeTicker kraken = fetch_kraken_ticker("XXBTZUSD", 8);
        if (kraken.has_data) fallback_price = kraken.price;
        if (fallback_price <= 0) {
            ExchangeTicker coinbase = fetch_coinbase_ticker("BTC-USD", 8);
            if (coinbase.has_data) fallback_price = coinbase.price;
        }
        if (fallback_price > 0) {
            json_object_set_new(feed, "open", json_real(fallback_price * 0.999));
            json_object_set_new(feed, "high", json_real(fallback_price * 1.001));
            json_object_set_new(feed, "low", json_real(fallback_price * 0.999));
            json_object_set_new(feed, "close", json_real(fallback_price));
            json_object_set_new(feed, "volume", json_real(100));
            printf("[bridge] using live price: close=%.2f\n", fallback_price);
        } else {
            json_object_set_new(feed, "open", json_real(0));
            json_object_set_new(feed, "high", json_real(0));
            json_object_set_new(feed, "low", json_real(0));
            json_object_set_new(feed, "close", json_real(0));
            json_object_set_new(feed, "volume", json_real(0));
        }
    }
    
    // Fear & Greed
    double fng = get_fear_greed();
    json_object_set_new(feed, "fear_greed", json_real(fng));
    
    // Pump score
    double pump = get_latest_pump_score();
    json_object_set_new(feed, "pump_score", json_real(pump));
    
    // Timeline context
    TimelineContext tc = get_timeline_context(now);
    if (tc.sp500 > 0) json_object_set_new(feed, "sp500", json_real(tc.sp500));
    json_object_set_new(feed, "vix", json_real(tc.vix > 0 ? tc.vix : 18.0));
    if (tc.btc_30d_vol > 0) json_object_set_new(feed, "btc_30d_volatility", json_real(tc.btc_30d_vol));
    if (tc.btc_30d_mean > 0) json_object_set_new(feed, "btc_30d_mean", json_real(tc.btc_30d_mean));
    if (tc.btc_30d_high > 0) json_object_set_new(feed, "btc_30d_high", json_real(tc.btc_30d_high));
    if (tc.btc_30d_low > 0) json_object_set_new(feed, "btc_30d_low", json_real(tc.btc_30d_low));
    if (tc.crypto_market_cap > 0) json_object_set_new(feed, "crypto_market_cap", json_real(tc.crypto_market_cap));
    json_object_set_new(feed, "btc_dominance", json_real(tc.btc_dominance > 0 ? tc.btc_dominance : 62.0));
    json_object_set_new(feed, "active_cryptocurrencies", json_integer(tc.active_cryptos));
    
    // ── T11: Exchange API integration — live price from Kraken + Coinbase ──
    // Binance geo-blocked from US — falls through gracefully
    ExchangeConfig exch_cfg;
    exchange_config_init(&exch_cfg);
    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/exchange_config.json", C_ROOM_DIR);
    exchange_config_load(&exch_cfg, cfg_path);
    
    // Kraken BTC/USD (public ticker — no key needed)
    ExchangeTicker kraken = fetch_kraken_ticker("XXBTZUSD", 8);
    if (kraken.has_data) {
        json_object_set_new(feed, "kraken_price", json_real(kraken.price));
        json_object_set_new(feed, "kraken_bid", json_real(kraken.bid));
        json_object_set_new(feed, "kraken_ask", json_real(kraken.ask));
        json_object_set_new(feed, "kraken_volume_24h", json_real(kraken.volume_24h));
        json_object_set_new(feed, "kraken_high_24h", json_real(kraken.high_24h));
        json_object_set_new(feed, "kraken_low_24h", json_real(kraken.low_24h));
        json_object_set_new(feed, "kraken_change_24h", json_real(kraken.change_24h));
    }
    
    // Coinbase BTC-USD (public ticker — no key needed)
    ExchangeTicker coinbase = fetch_coinbase_ticker("BTC-USD", 8);
    if (coinbase.has_data) {
        json_object_set_new(feed, "coinbase_price", json_real(coinbase.price));
        json_object_set_new(feed, "coinbase_bid", json_real(coinbase.bid));
        json_object_set_new(feed, "coinbase_ask", json_real(coinbase.ask));
        json_object_set_new(feed, "coinbase_volume_24h", json_real(coinbase.volume_24h));
    }
    
    // Cross-exchange spread (arbitrage signal)
    if (kraken.has_data && coinbase.has_data) {
        double spread = exchange_spread_pct(kraken.price, coinbase.price);
        json_object_set_new(feed, "kraken_coinbase_spread_pct", json_real(spread));
        printf("[bridge] exchange spread: Kraken~Coinbase=%.4f%%\n", spread);
    }
    
    // ── T12: WebSocket feed — real-time ticker from ws_feed_bridge.py ──
    char ws_path[512];
    snprintf(ws_path, sizeof(ws_path), "%s/ws_feed.json", C_ROOM_DIR);
    json_t *ws_root = json_load_file(ws_path, 0, NULL);
    if (ws_root) {
        json_t *j_ws_ts = json_object_get(ws_root, "ts");
        double ws_ts = j_ws_ts ? json_number_value(j_ws_ts) : 0;
        double ws_age = now - ws_ts;
        
        // Only use WS data if < 30s old (fresh)
        if (ws_ts > 0 && ws_age < 30.0) {
            json_object_set_new(feed, "ws_age_sec", json_real(ws_age));
            
            // Kraken WS
            json_t *j_kraken = json_object_get(ws_root, "kraken");
            if (j_kraken) {
                json_t *v;
                v = json_object_get(j_kraken, "price");
                if (v) json_object_set_new(feed, "ws_kraken_price", json_real(json_number_value(v)));
                v = json_object_get(j_kraken, "bid");
                if (v) json_object_set_new(feed, "ws_kraken_bid", json_real(json_number_value(v)));
                v = json_object_get(j_kraken, "ask");
                if (v) json_object_set_new(feed, "ws_kraken_ask", json_real(json_number_value(v)));
                v = json_object_get(j_kraken, "volume");
                if (v) json_object_set_new(feed, "ws_kraken_vol", json_real(json_number_value(v)));
            }
            
            // Coinbase WS
            json_t *j_coinbase = json_object_get(ws_root, "coinbase");
            if (j_coinbase) {
                json_t *v;
                v = json_object_get(j_coinbase, "price");
                if (v) json_object_set_new(feed, "ws_coinbase_price", json_real(json_number_value(v)));
                v = json_object_get(j_coinbase, "bid");
                if (v) json_object_set_new(feed, "ws_coinbase_bid", json_real(json_number_value(v)));
                v = json_object_get(j_coinbase, "ask");
                if (v) json_object_set_new(feed, "ws_coinbase_ask", json_real(json_number_value(v)));
                v = json_object_get(j_coinbase, "volume");
                if (v) json_object_set_new(feed, "ws_coinbase_vol", json_real(json_number_value(v)));
            }
            
            printf("[bridge] WS feed: age=%.1fs (kraken+coinbase live)\n", ws_age);
        } else {
            json_object_set_new(feed, "ws_age_sec", json_real(ws_age));
            printf("[bridge] WS feed stale (age=%.1fs)\n", ws_age);
        }
        json_decref(ws_root);
    } else {
        printf("[bridge] WS feed not available (ws_feed.json missing)\n");
    }
    
    // PID signals + market dynamics (inlined C — no Python dependency)
    double close_val = 0;
    json_t *j_close = json_object_get(feed, "close");
    if (j_close) close_val = json_number_value(j_close);
    
    // Load previous feed for PID state persistence
    char prev_path[512], tmp_path[512], dst_path[512];
    snprintf(prev_path, sizeof(prev_path), "%s/feed_prev.json", C_ROOM_DIR);
    snprintf(tmp_path, sizeof(tmp_path), "%s/feed_tmp.json", C_ROOM_DIR);
    snprintf(dst_path, sizeof(dst_path), "%s/market_feed.json", C_ROOM_DIR);
    
    static PIDController pid_btc = {0.1, 0.001, 0.5, 0, 0, 0};
    static double ewma_fast = 0, ewma_slow = 0;
    static double prev_close = 0;
    
    // Load previous state
    json_t *prev_root = json_load_file(prev_path, 0, NULL);
    if (prev_root) {
        json_t *j = json_object_get(prev_root, "pid_integral");
        if (j) pid_btc.integral = json_number_value(j);
        j = json_object_get(prev_root, "pid_prev_error");
        if (j) pid_btc.prev_error = json_number_value(j);
        pid_btc.initialized = 1;
        j = json_object_get(prev_root, "ewma_fast");
        if (j) ewma_fast = json_number_value(j);
        j = json_object_get(prev_root, "ewma_slow");
        if (j) ewma_slow = json_number_value(j);
        j = json_object_get(prev_root, "close");
        if (j) prev_close = json_number_value(j);
        json_decref(prev_root);
    }
    
    // PID signal for BTC
    double pid_signal = pid_update(&pid_btc, close_val, close_val * 1.001, 60.0);
    
    // Nested prediction
    double npred = nested_prediction(close_val, &ewma_fast, &ewma_slow);
    
    // Regime detection (based on price vs moving average)
    int regime = 1;  // 0=bear, 1=side, 2=bull
    double regime_diff = close_val > 0 && ewma_slow > 0 ?
        (close_val - ewma_slow) / ewma_slow : 0;
    if (regime_diff > 0.02) regime = 2;
    else if (regime_diff < -0.02) regime = 0;
    
    // Build enrichment JSON
    json_t *pid_signals = json_object();
    json_t *btc_pid = json_object();
    json_object_set_new(btc_pid, "p", json_real(pid_btc.p * pid_btc.prev_error));
    json_object_set_new(btc_pid, "i", json_real(pid_btc.i * pid_btc.integral));
    json_object_set_new(btc_pid, "d", json_real(pid_btc.d * (pid_btc.prev_error > 0 ? pid_signal * 0.1 : 0)));
    json_object_set_new(pid_signals, "BTC", btc_pid);
    json_object_set_new(feed, "pid_signals", pid_signals);
    
    json_t *sentiment = json_object();
    json_object_set_new(sentiment, "momentum_phase", json_integer(regime));
    json_object_set_new(sentiment, "fear_greed_signal", json_real((fng - 50.0) / 50.0));
    json_object_set_new(feed, "sentiment_indexes", sentiment);
    
    json_object_set_new(feed, "market_regime", json_integer(regime));
    json_object_set_new(feed, "market_volatility", json_real(tc.btc_30d_vol > 0 ? tc.btc_30d_vol : 3.0));
    json_object_set_new(feed, "nested_prediction", json_real(npred));
    
    // Strategy weights (simplified: equal weight with regime bias)
    json_t *weights = json_object();
    double w_bull = (regime == 2) ? 0.35 : (regime == 0) ? 0.15 : 0.20;
    double w_side = (regime == 1) ? 0.35 : 0.20;
    double w_breakout = 0.15;
    double w_reversal = 0.15;
    double w_momentum = 1.0 - w_bull - w_side - w_breakout - w_reversal;
    json_object_set_new(weights, "bull", json_real(w_bull));
    json_object_set_new(weights, "sideways", json_real(w_side));
    json_object_set_new(weights, "breakout", json_real(w_breakout));
    json_object_set_new(weights, "reversal", json_real(w_reversal));
    json_object_set_new(weights, "momentum", json_real(w_momentum));
    json_object_set_new(feed, "strategy_weights", weights);
    
    // ─── Q-Controller Reward (T3) ───
    if (prev_close > 0 && close_val > 0) {
        double move_pct = (close_val - prev_close) / prev_close;
        double reward = fabs(move_pct) * (move_pct < 0 ? -1.0 : 1.0);
        if (reward > 1.0) reward = 1.0;
        if (reward < -1.0) reward = -1.0;
        reward *= 10.0;
        json_object_set_new(feed, "q_reward", json_real(reward));
        json_object_set_new(feed, "q_action", json_integer(regime == 0 ? 0 : (regime == 2 ? 1 : 2)));
    }
    
    // Anti-consensus signal
    AntiConsensus ac = get_anti_consensus();
    json_object_set_new(feed, "anti_consensus", json_real(ac.anti_consensus));
    json_object_set_new(feed, "anti_signal", json_real(ac.anti_signal));
    json_object_set_new(feed, "worst_wr", json_real(ac.worst_wr));
    json_object_set_new(feed, "best_wr", json_real(ac.best_wr));
    
    // ── Write atomically ──
    FILE *fout = fopen(tmp_path, "w");
    if (fout) {
        json_dumpf(feed, fout, JSON_REAL_PRECISION(8));
        fclose(fout);
    }
    rename(tmp_path, dst_path);
    
    // ── Save PID state for next tick ──
    json_t *prev = json_object();
    json_object_set_new(prev, "pid_integral", json_real(pid_btc.integral));
    json_object_set_new(prev, "pid_prev_error", json_real(pid_btc.prev_error));
    json_object_set_new(prev, "ewma_fast", json_real(ewma_fast));
    json_object_set_new(prev, "ewma_slow", json_real(ewma_slow));
    json_object_set_new(prev, "close", json_real(close_val));
    FILE *fp_prev = fopen(prev_path, "w");
    if (fp_prev) {
        json_dumpf(prev, fp_prev, JSON_REAL_PRECISION(8));
        fclose(fp_prev);
    }
    json_decref(prev);
    
    // ── Heartbeat ──
    char hb_path[512];
    snprintf(hb_path, sizeof(hb_path), "%s/room-feed.heartbeat", HB_DIR);
    FILE *hb = fopen(hb_path, "w");
    if (hb) {
        fprintf(hb, "%ld", (long)now);
        fclose(hb);
    }
    
    // ── Log ──
    printf("[bridge] wrote feed: BTC ts=%ld close=%.2f pump=%.3f\n",
           (long)window_ts, close_val, pump);
    
    json_decref(feed);
    return 0;
}
