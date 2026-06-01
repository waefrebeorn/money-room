/*
 * feed_bridge.c — E16: Real-time market data bridge (C port of room_feed_bridge.py)
 *
 * Reads from historical.db (Kraken BTC OHLCV) + timeline.db (macro data)
 * + news feeds → writes market_feed.json for the C room engine.
 *
 * Dependencies: libjansson, libsqlite3, libcurl
 * Build: gcc -O3 -march=native feed_bridge.c -o feed_bridge -ljansson -lsqlite3 -lcurl -lm
 *
 * Usage: ./feed_bridge
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sqlite3.h>
#include <jansson.h>
#include <curl/curl.h>

/* ─── Paths ─── */
#define HOME_DIR            "/home/wubu2"
#define ROOM_DIR            HOME_DIR "/.hermes/pm_logs/c_room"
#define HISTORICAL_DB       HOME_DIR "/.hermes/pm_logs/historical/historical.db"
#define TIMELINE_DB         HOME_DIR "/.hermes/pm_logs/timeline.db"
#define NEWS_DIR            HOME_DIR "/.hermes/pm_logs/news/runs"
#define ECO_DIR             HOME_DIR "/.hermes/pm_logs/eco"
#define MARKET_FEED         ROOM_DIR "/market_feed.json"
#define FEED_TMP            ROOM_DIR "/feed_tmp.json"
#define OPTIONS_FEAT        HOME_DIR "/.hermes/options_cache/latest_features.json"
#define EARNINGS_FEAT       HOME_DIR "/.hermes/options_cache/earnings_features.json"
#define ONCHAIN_FEAT        HOME_DIR "/.hermes/options_cache/onchain_features.json"
#define STABLECOIN_FEAT     HOME_DIR "/.hermes/options_cache/stablecoin_features.json"
#define FUNDING_FEAT        HOME_DIR "/.hermes/options_cache/funding_features.json"
#define OI_FEAT             HOME_DIR "/.hermes/options_cache/open_interest_features.json"
#define LS_FEAT             HOME_DIR "/.hermes/options_cache/ls_ratio_features.json"
#define LIQ_FEAT            HOME_DIR "/.hermes/options_cache/liquidation_features.json"
#define WHALE_FEAT          HOME_DIR "/.hermes/options_cache/whale_features.json"
#define ETF_FEAT            HOME_DIR "/.hermes/options_cache/etf_flow_features.json"
#define HASHRATE_FEAT       HOME_DIR "/.hermes/options_cache/hashrate_features.json"

/* ─── Constants ─── */
#define BTC_MIN_PRICE       10000.0
#define BTC_MAX_PRICE       500000.0
#define MAX_CANDLE_AGE_SECS 300
#define FEAR_GREED_URL      "https://api.alternative.me/fng/?limit=1"
#define COINBASE_TICKER_URL "https://api.exchange.coinbase.com/products/BTC-USD/ticker"
#define OKX_TICKER_URL      "https://www.okx.com/api/v5/market/ticker?instId=BTC-USDT"
#define EURUSD_URL          "https://api.frankfurter.app/latest?from=USD&to=EUR"
#define YAHOO_CHART_SPY     "https://query1.finance.yahoo.com/v8/finance/chart/SPY?range=1d&interval=1d"
#define YAHOO_CHART_QQQ     "https://query1.finance.yahoo.com/v8/finance/chart/QQQ?range=1d&interval=1d"
#define YAHOO_CHART_DIA     "https://query1.finance.yahoo.com/v8/finance/chart/DIA?range=1d&interval=1d"
#define YAHOO_CHART_GLD     "https://query1.finance.yahoo.com/v8/finance/chart/GLD?range=1d&interval=1d"
#define YAHOO_CHART_USO     "https://query1.finance.yahoo.com/v8/finance/chart/USO?range=1d&interval=1d"
#define YAHOO_CHART_SLV     "https://query1.finance.yahoo.com/v8/finance/chart/SLV?range=1d&interval=1d"
#define YAHOO_CHART_VIX     "https://query1.finance.yahoo.com/v8/finance/chart/%5EVIX?range=5d&interval=1d"
#define BLOCKCHAIN_STATS    "https://blockchain.info/stats?format=json"

/* ─── Exchange ticker struct ─── */
typedef struct {
    double price, bid, ask, volume_24h;
    int    found;
} ExchangeTicker;

/* ─── Candle struct ─── */
typedef struct {
    long ts;
    double open, high, low, close, volume;
    int found;
} Candle;

/* ─── Buffer for libcurl ─── */
struct MemBuf {
    char *data;
    size_t len;
};

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemBuf *buf = (struct MemBuf *)userp;
    char *ptr = realloc(buf->data, buf->len + realsize + 1);
    if (!ptr) return 0;
    buf->data = ptr;
    memcpy(&buf->data[buf->len], contents, realsize);
    buf->len += realsize;
    buf->data[buf->len] = 0;
    return realsize;
}

/* ─── Fetch URL into string ─── */
static char *http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    struct MemBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[feed] HTTP error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ─── Get latest BTC 1-min candle from historical.db ─── */
static Candle get_latest_btc_candle(void) {
    Candle c = {0, 0, 0, 0, 0, 0, 0};
    sqlite3 *db = NULL;
    if (sqlite3_open(HISTORICAL_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "[feed] Cannot open historical.db\n");
        return c;
    }
    const char *sql = "SELECT ts, open, high, low, close, volume "
                      "FROM candles_multi "
                      "WHERE pair='BTC' AND interval=1 AND close > 0 "
                      "ORDER BY ts DESC LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            c.ts      = sqlite3_column_int64(stmt, 0);
            c.open    = sqlite3_column_double(stmt, 1);
            c.high    = sqlite3_column_double(stmt, 2);
            c.low     = sqlite3_column_double(stmt, 3);
            c.close   = sqlite3_column_double(stmt, 4);
            c.volume  = sqlite3_column_double(stmt, 5);
            c.found   = 1;
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return c;
}

/* ─── Get pump_score from latest news run ─── */
static double get_latest_pump_score(void) {
    /* Find latest news_*.json file */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls -t " NEWS_DIR "/news_*.json 2>/dev/null | head -1");
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0.0;
    char path[512];
    if (!fgets(path, sizeof(path), fp)) {
        pclose(fp);
        return 0.0;
    }
    pclose(fp);
    /* Trim newline */
    size_t len = strlen(path);
    while (len > 0 && (path[len-1] == '\n' || path[len-1] == '\r')) path[--len] = 0;

    /* Parse JSON */
    json_t *root = NULL;
    json_error_t err;
    root = json_load_file(path, 0, &err);
    if (!root) {
        fprintf(stderr, "[feed] JSON parse error in %s: %s\n", path, err.text);
        return 0.0;
    }

    json_t *pump_articles = json_object_get(root, "pump_articles");
    json_t *total_articles = json_object_get(root, "total_articles");
    double score = 0.0;
    if (json_is_integer(pump_articles) && json_is_integer(total_articles)) {
        long total = json_integer_value(total_articles);
        if (total > 0) {
            score = (double)json_integer_value(pump_articles) / (double)total;
            score = (score - 0.5) * 2.0; /* normalize to [-1, 1] */
        }
    }
    json_decref(root);
    return score;
}

/* ─── Fetch fear & greed index ─── */
static int get_fear_greed(void) {
    char *body = http_get(FEAR_GREED_URL);
    if (!body) return 50;

    json_t *root = NULL;
    json_error_t err;
    root = json_loads(body, 0, &err);
    free(body);
    if (!root) return 50;

    int fg = 50;
    json_t *data = json_object_get(root, "data");
    if (json_is_array(data)) {
        json_t *first = json_array_get(data, 0);
        if (first) {
            json_t *value = json_object_get(first, "value");
            if (json_is_string(value))
                fg = atoi(json_string_value(value));
        }
    }
    json_decref(root);
    return fg;
}

/* ─── Fetch Coinbase BTC-USD ticker ─── */
static ExchangeTicker get_coinbase_ticker(void) {
    ExchangeTicker t = {0, 0, 0, 0, 0};
    char *body = http_get(COINBASE_TICKER_URL);
    if (!body) return t;

    json_t *root = NULL;
    json_error_t err;
    root = json_loads(body, 0, &err);
    free(body);
    if (!root) return t;

    json_t *price = json_object_get(root, "price");
    json_t *bid   = json_object_get(root, "bid");
    json_t *ask   = json_object_get(root, "ask");
    json_t *vol   = json_object_get(root, "volume");
    if (json_is_string(price)) t.price = atof(json_string_value(price));
    if (json_is_string(bid))   t.bid   = atof(json_string_value(bid));
    if (json_is_string(ask))   t.ask   = atof(json_string_value(ask));
    if (json_is_string(vol))   t.volume_24h = atof(json_string_value(vol));
    t.found = (t.price > 0);
    json_decref(root);

    if (t.found)
        printf("[feed] Coinbase ticker: price=%.2f bid=%.2f ask=%.2f vol=%.0f\n",
               t.price, t.bid, t.ask, t.volume_24h);
    return t;
}

/* ─── Fetch OKX BTC-USDT ticker ─── */
static ExchangeTicker get_okx_ticker(void) {
    ExchangeTicker t = {0, 0, 0, 0, 0};
    char *body = http_get(OKX_TICKER_URL);
    if (!body) return t;

    json_t *root = NULL;
    json_error_t err;
    root = json_loads(body, 0, &err);
    free(body);
    if (!root) return t;

    json_t *data = json_object_get(root, "data");
    if (json_is_array(data) && json_array_size(data) > 0) {
        json_t *first = json_array_get(data, 0);
        json_t *last  = json_object_get(first, "last");
        json_t *bid   = json_object_get(first, "bidPx");
        json_t *ask   = json_object_get(first, "askPx");
        json_t *vol   = json_object_get(first, "vol24h");
        if (json_is_string(last)) t.price = atof(json_string_value(last));
        if (json_is_string(bid))  t.bid   = atof(json_string_value(bid));
        if (json_is_string(ask))  t.ask   = atof(json_string_value(ask));
        if (json_is_string(vol))  t.volume_24h = atof(json_string_value(vol));
        t.found = (t.price > 0);
    }
    json_decref(root);

    if (t.found)
        printf("[feed] OKX ticker: price=%.2f bid=%.2f ask=%.2f vol=%.0f\n",
               t.price, t.bid, t.ask, t.volume_24h);
    return t;
}

/* ─── Fetch EUR/USD forex rate ─── */
static double get_eurusd(void) {
    char *body = http_get(EURUSD_URL);
    if (!body) return 0.0;

    json_t *root = NULL;
    json_error_t err;
    root = json_loads(body, 0, &err);
    free(body);
    if (!root) return 0.0;

    double rate = 0.0;
    json_t *rates = json_object_get(root, "rates");
    if (json_is_object(rates)) {
        json_t *eur = json_object_get(rates, "EUR");
        if (json_is_real(eur)) rate = json_real_value(eur);
    }
    json_decref(root);

    if (rate > 0)
        printf("[feed] EUR/USD: %.5f\n", rate);
    return rate;
}

/* ─── Fetch index ETF price from Yahoo Finance ─── */
static double get_yahoo_index(const char *url, const char *name) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0.0;

    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/json");
    struct MemBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    double price = 0.0;
    if (res == CURLE_OK && buf.data) {
        json_t *root = NULL;
        json_error_t err;
        root = json_loads(buf.data, 0, &err);
        if (root) {
            json_t *chart = json_object_get(root, "chart");
            json_t *result = chart ? json_object_get(chart, "result") : NULL;
            if (json_is_array(result) && json_array_size(result) > 0) {
                json_t *first = json_array_get(result, 0);
                json_t *meta = json_object_get(first, "meta");
                if (meta) {
                    json_t *mp = json_object_get(meta, "regularMarketPrice");
                    if (json_is_real(mp)) price = json_real_value(mp);
                }
            }
            json_decref(root);
        }
    }
    free(buf.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (price > 0) printf("[feed] %s: %.2f\n", name, price);
    return price;
}

/* ─── Fetch VIX with 5-day percentile ─── */
typedef struct { double price; double min_5d; double max_5d; } VixData;
static VixData get_vix_data(void) {
    VixData v = {0, 0, 0};
    CURL *curl = curl_easy_init();
    if (!curl) return v;
    struct MemBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, YAHOO_CHART_VIX);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK && buf.data) {
        json_t *root = NULL;
        json_error_t err;
        root = json_loads(buf.data, 0, &err);
        if (root) {
            json_t *chart = json_object_get(root, "chart");
            json_t *result = chart ? json_object_get(chart, "result") : NULL;
            if (json_is_array(result) && json_array_size(result) > 0) {
                json_t *first = json_array_get(result, 0);
                json_t *meta = json_object_get(first, "meta");
                if (meta) {
                    json_t *mp = json_object_get(meta, "regularMarketPrice");
                    if (json_is_real(mp)) v.price = json_real_value(mp);
                }
                /* Get 5-day high/low from indicators */
                json_t *indicators = json_object_get(first, "indicators");
                json_t *quotes = indicators ? json_object_get(indicators, "quote") : NULL;
                if (json_is_array(quotes) && json_array_size(quotes) > 0) {
                    json_t *q = json_array_get(quotes, 0);
                    json_t *high = json_object_get(q, "high");
                    json_t *low  = json_object_get(q, "low");
                    if (json_is_array(high) && json_is_array(low)) {
                        size_t n = json_array_size(high);
                        for (size_t i = 0; i < n; i++) {
                            double h = json_is_real(json_array_get(high, i)) ? json_real_value(json_array_get(high, i)) : 0.0;
                            double l = json_is_real(json_array_get(low, i))  ? json_real_value(json_array_get(low, i))  : 0.0;
                            if (h > v.max_5d) v.max_5d = h;
                            if (v.min_5d == 0 || (l > 0 && l < v.min_5d)) v.min_5d = l;
                        }
                    }
                }
            }
            json_decref(root);
        }
    }
    free(buf.data);
    curl_easy_cleanup(curl);
    if (v.price > 0) printf("[feed] VIX: %.2f (5d range: %.2f-%.2f)\n", v.price, v.min_5d, v.max_5d);
    return v;
}

/* ─── On-chain BTC data from blockchain.info ─── */
typedef struct {
    double hash_rate_ph_s;   // Petahash/s
    double difficulty_t;     // Trillions
    double n_tx;             // 24h tx count
    double total_sent_btc;   // 24h BTC sent
    double trade_vol_usd;    // 24h trade volume
    int    found;
} OnChainData;
static OnChainData get_onchain_data(void) {
    OnChainData oc = {0, 0, 0, 0, 0, 0};
    char *body = http_get(BLOCKCHAIN_STATS);
    if (!body) return oc;
    json_t *root = NULL;
    json_error_t err;
    root = json_loads(body, 0, &err);
    free(body);
    if (!root) return oc;
    json_t *hr = json_object_get(root, "hash_rate");
    json_t *df = json_object_get(root, "difficulty");
    json_t *tx = json_object_get(root, "n_tx");
    json_t *tb = json_object_get(root, "total_btc_sent");
    json_t *tv = json_object_get(root, "trade_volume_usd");
    if (json_is_real(hr)) oc.hash_rate_ph_s  = json_real_value(hr) / 1e15;
    if (json_is_real(df)) oc.difficulty_t    = json_real_value(df) / 1e12;
    else if (json_is_integer(df)) oc.difficulty_t = (double)json_integer_value(df) / 1e12;
    if (json_is_integer(tx)) oc.n_tx         = (double)json_integer_value(tx);
    if (json_is_integer(tb)) oc.total_sent_btc = (double)json_integer_value(tb);
    if (json_is_real(tv)) oc.trade_vol_usd   = json_real_value(tv);
    oc.found = (json_is_real(hr) || json_is_integer(hr));
    json_decref(root);
    if (oc.found)
        printf("[feed] On-chain: hash=%.2fPH/s diff=%.0fT tx=%.0f vol=$%.0f\n",
               oc.hash_rate_ph_s, oc.difficulty_t, oc.n_tx, oc.trade_vol_usd);
    return oc;
}

/* ─── Query timeline.db for a metric ─── */
static double get_timeline_metric(const char *source, const char *json_path, double def) {
    sqlite3 *db = NULL;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) return def;

    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT json_extract(data, '%s') as val FROM timeline "
        "WHERE source='%s' AND json_extract(data, '%s') IS NOT NULL "
        "ORDER BY ts DESC LIMIT 1",
        json_path, source, json_path);

    double val = def;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            val = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return val;
}

/* ─── Get BTC 30-day stats from timeline.db ─── */
static int get_btc_30d_stats(double *vol, double *mean_price, double *high, double *low) {
    sqlite3 *db = NULL;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) return -1;

    long month_ago = (long)time(NULL) - 86400 * 30;
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT json_extract(data, '$.close') as c FROM timeline "
        "WHERE source='bitstamp_1min' AND json_extract(data, '$.pair')='BTC' "
        "AND ts > %ld AND json_extract(data, '$.close') IS NOT NULL "
        "ORDER BY ts DESC LIMIT 43200",
        month_ago);

    sqlite3_stmt *stmt = NULL;
    double *prices = NULL;
    int n = 0, cap = 43200;
    prices = malloc(cap * sizeof(double));
    if (!prices) { sqlite3_close(db); return -1; }

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && n < cap) {
            prices[n++] = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);

    if (n < 20) { free(prices); return -1; }

    double sum = 0, sum_sq = 0;
    *high = *low = prices[0];
    for (int i = 0; i < n; i++) {
        sum += prices[i];
        sum_sq += prices[i] * prices[i];
        if (prices[i] > *high) *high = prices[i];
        if (prices[i] < *low)  *low  = prices[i];
    }
    *mean_price = sum / n;
    *vol = sqrt(sum_sq / n - (*mean_price) * (*mean_price));
    free(prices);
    return 0;
}

/* ─── P45: Puell multiple — daily miner rev / 60d MA of daily rev ─── */
/* Daily miner revenue ≈ (block_reward + fees) × blocks × price ≈ 450 × BTC_price */
static double get_puell_multiple(double current_price) {
    sqlite3 *db = NULL;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) return -1;

    /* Get daily average close prices for last 60 days */
    /* Group 1-min data by day, compute daily avg close */
    long two_month_ago = (long)time(NULL) - 86400 * 62;
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT CAST(ts / 86400 AS INTEGER) * 86400 AS day, "
        "AVG(json_extract(data, '$.close')) as avg_close "
        "FROM timeline "
        "WHERE source='bitstamp_1min' AND json_extract(data, '$.pair')='BTC' "
        "AND ts > %ld AND json_extract(data, '$.close') IS NOT NULL "
        "GROUP BY day ORDER BY day ASC",
        two_month_ago);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    #define REVENUE_BLOCKS 86400L
    double daily_revs[90];
    int n_days = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n_days < 90) {
        double avg_price = sqlite3_column_double(stmt, 1);
        /* Daily miner revenue in USD: 450 BTC/day × avg_price */
        daily_revs[n_days++] = 450.0 * avg_price;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (n_days < 20) return -1;  /* Need at least 20 days */

    /* Current daily revenue */
    double current_rev = 450.0 * current_price;

    /* Compute 60-day MA of daily revenue (or use all available) */
    int ma_window = n_days < 60 ? n_days - 1 : 60;
    double sum = 0;
    for (int i = n_days - ma_window; i < n_days; i++)
        sum += daily_revs[i];
    double ma = sum / ma_window;

    if (ma <= 0) return -1;

    return current_rev / ma;
}

/* ─── P48: Pi Cycle proxy — short MA / long MA ratio ─── */
static double get_pi_cycle_ratio(double current_price) {
    /* 
     * Pi Cycle Top = when 111d MA price > 350d MA × 2
     * With only ~85d data, use 30d / 60d MA ratio as proxy
     * Rising ratio = short-term momentum outpacing long-term (bullish)
     * Falling ratio = momentum fading (bearish signal if crosses below 1)
     */
    sqlite3 *db = NULL;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) return -1;

    long two_month_ago = (long)time(NULL) - 86400 * 62;
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT json_extract(data, '$.close') as c FROM timeline "
        "WHERE source='bitstamp_1min' AND json_extract(data, '$.pair')='BTC' "
        "AND ts > %ld AND json_extract(data, '$.close') IS NOT NULL "
        "ORDER BY ts DESC LIMIT 86400",
        two_month_ago);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    double closes[90000];
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < 90000)
        closes[n++] = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (n < 1000) return -1;

    /* Use 30-min candles: sample every 30 minutes */
    int sample = 30;
    int sampled_count = n / sample;
    if (sampled_count < 30) return -1;

    /* Short MA: last 30 sampled points (≈ 15 hours with 30-min samples) */
    /* Long MA: last 60 sampled points (≈ 30 hours) */
    /* This is a proxy for Pi Cycle concept but at much shorter timeframes */
    int short_w = 30 < sampled_count ? 30 : sampled_count / 2;
    int long_w  = 60 < sampled_count ? 60 : sampled_count;
    double short_sum = 0, long_sum = 0;
    for (int i = 0; i < long_w; i++) {
        long_sum += closes[i * sample];
        if (i < short_w) short_sum += closes[i * sample];
    }
    double short_ma = short_sum / short_w;
    double long_ma  = long_sum / long_w;
    if (long_ma <= 0) return -1;

    return short_ma / long_ma;
}

/* ─── P49: Mayer multiple — price / 60d MA price ─── */
static double get_mayer_multiple(double current_price) {
    sqlite3 *db = NULL;
    if (sqlite3_open(TIMELINE_DB, &db) != SQLITE_OK) return -1;

    long two_month_ago = (long)time(NULL) - 86400 * 62;
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT CAST(ts / 86400 AS INTEGER) * 86400 AS day, "
        "AVG(json_extract(data, '$.close')) as avg_close "
        "FROM timeline "
        "WHERE source='bitstamp_1min' AND json_extract(data, '$.pair')='BTC' "
        "AND ts > %ld AND json_extract(data, '$.close') IS NOT NULL "
        "GROUP BY day ORDER BY day ASC",
        two_month_ago);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    double daily_closes[90];
    int n_days = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n_days < 90)
        daily_closes[n_days++] = sqlite3_column_double(stmt, 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (n_days < 20) return -1;

    /* Compute 60d SMA of daily closes */
    int ma_window = n_days < 60 ? n_days - 1 : 60;
    double sum = 0;
    for (int i = n_days - ma_window; i < n_days; i++)
        sum += daily_closes[i];
    double ma = sum / ma_window;

    if (ma <= 0) return -1;

    /* Mayer multiple = current price / 60d MA */
    /* Traditional Mayer uses 200d MA — this is a proxy with available data */
    return current_price / ma;
}

// ── TemporalFeatures struct ──
/* ─── Temporal features from historical candles (T29) ─── */
#define MAX_LAGS 15
typedef struct {
    double price_lag_1min, price_lag_5min, price_lag_15min;
    double mom_1min, mom_5min, mom_15min;
    double roll_vol_5min, roll_vol_15min;
    double ma_fast, ma_slow, ma_cross;
    double high_lag_15, low_lag_15;
    int found;
} TemporalFeatures;
static TemporalFeatures get_temporal_features(void) {
    TemporalFeatures tf = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    sqlite3 *db = NULL;
    if (sqlite3_open(HISTORICAL_DB, &db) != SQLITE_OK) return tf;

    double closes[MAX_LAGS], highs[MAX_LAGS], lows[MAX_LAGS];
    int n = 0;
    const char *sql = "SELECT close, high, low FROM candles_multi "
                      "WHERE pair='BTC' AND interval=1 AND close > 0 "
                      "ORDER BY ts DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, MAX_LAGS);
        while (sqlite3_step(stmt) == SQLITE_ROW && n < MAX_LAGS) {
            closes[n] = sqlite3_column_double(stmt, 0);
            highs[n]  = sqlite3_column_double(stmt, 1);
            lows[n]   = sqlite3_column_double(stmt, 2);
            n++;
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);

    if (n < 3) return tf;  // Need at least 3 candles for useful features

    // Price lags
    double current = closes[0];
    if (n > 1)  tf.price_lag_1min  = closes[1];
    if (n > 5)  tf.price_lag_5min  = closes[5];
    if (n > 14) tf.price_lag_15min = closes[14];

    // Momentum (rate of change over N periods)
    tf.mom_1min  = (n > 1  && closes[1]  > 0) ? (current - closes[1])  / closes[1]  * 100.0 : 0;
    tf.mom_5min  = (n > 5  && closes[5]  > 0) ? (current - closes[5])  / closes[5]  * 100.0 : 0;
    tf.mom_15min = (n > 14 && closes[14] > 0) ? (current - closes[14]) / closes[14] * 100.0 : 0;

    // Rolling volatility (std dev of returns over N periods)
    if (n > 5) {
        double sum = 0, sum_sq = 0;
        for (int i = 1; i < 6 && i < n; i++) {
            double ret = (closes[i-1] - closes[i]) / closes[i];
            sum += ret; sum_sq += ret * ret;
        }
        double mean = sum / (n > 5 ? 5 : 1);
        tf.roll_vol_5min = sqrt(sum_sq / (n > 5 ? 5 : 1) - mean * mean) * 100.0;
    }
    if (n > 14) {
        double sum = 0, sum_sq = 0;
        for (int i = 1; i < 15 && i < n; i++) {
            double ret = (closes[i-1] - closes[i]) / closes[i];
            sum += ret; sum_sq += ret * ret;
        }
        double mean = sum / (n > 14 ? 14 : 1);
        tf.roll_vol_15min = sqrt(sum_sq / (n > 14 ? 14 : 1) - mean * mean) * 100.0;
    }

    // Moving averages: fast=3, slow=8
    int fast_n = 3 < n ? 3 : n;
    int slow_n = 8 < n ? 8 : n;
    double fast_sum = 0, slow_sum = 0;
    for (int i = 0; i < fast_n; i++) fast_sum += closes[i];
    for (int i = 0; i < slow_n; i++) slow_sum += closes[i];
    tf.ma_fast = fast_sum / fast_n;
    tf.ma_slow = slow_sum / slow_n;
    tf.ma_cross = (tf.ma_slow > 0) ? (tf.ma_fast / tf.ma_slow - 1.0) * 100.0 : 0;

    // High/low range lag
    double max_h = highs[0], min_l = lows[0];
    for (int i = 1; i < n; i++) {
        if (highs[i] > max_h) max_h = highs[i];
        if (lows[i] < min_l)  min_l = lows[i];
    }
    tf.high_lag_15 = max_h;
    tf.low_lag_15  = min_l;
    tf.found = 1;

    printf("[feed] Temporal: mom_1=%.2f%% mom_5=%.2f%% mom_15=%.2f%% vol_5=%.4f%% cross=%.2f%%\n",
           tf.mom_1min, tf.mom_5min, tf.mom_15min, tf.roll_vol_5min, tf.ma_cross);
    return tf;
}

/* ─── Write market_feed.json ─── */
static int write_feed(void) {
    Candle candle = get_latest_btc_candle();
    double pump = get_latest_pump_score();
    int fg = get_fear_greed();

    /* Fetch live exchange tickers */
    ExchangeTicker cb = get_coinbase_ticker();
    ExchangeTicker ok = get_okx_ticker();
    double eurusd = get_eurusd();
    double spy = get_yahoo_index(YAHOO_CHART_SPY, "SPY");
    double qqq = get_yahoo_index(YAHOO_CHART_QQQ, "QQQ");
    double dia = get_yahoo_index(YAHOO_CHART_DIA, "DIA");
    double gld = get_yahoo_index(YAHOO_CHART_GLD, "GLD");
    double uso = get_yahoo_index(YAHOO_CHART_USO, "USO");
    double slv = get_yahoo_index(YAHOO_CHART_SLV, "SLV");
    VixData vd = get_vix_data();
    OnChainData oc = get_onchain_data();
    TemporalFeatures tf = get_temporal_features();

    /* Build feed object */
    json_t *feed = json_object();

    time_t now = time(NULL);
    long window_ts = (long)now - ((long)now % 60);

    if (!candle.found || candle.close <= 0.0) {
        /* Fallback — use exchange ticker price when candle is missing or stale */
        double ticker_price = 0;
        if (cb.found && cb.price > 0) ticker_price = cb.price;
        else if (ok.found && ok.price > 0) ticker_price = ok.price;
        else ticker_price = 73000.0;
        json_object_set_new(feed, "asset", json_string("BTC"));
        json_object_set_new(feed, "window_ts", json_integer(window_ts));
        json_object_set_new(feed, "open", json_real(ticker_price));
        json_object_set_new(feed, "high", json_real(ticker_price));
        json_object_set_new(feed, "low", json_real(ticker_price));
        json_object_set_new(feed, "close", json_real(ticker_price));
        json_object_set_new(feed, "volume", json_real(cb.found ? cb.volume_24h : 0));
        json_object_set_new(feed, "fear_greed", json_real(50.0));
        json_object_set_new(feed, "pump_score", json_real(pump));
        json_object_set_new(feed, "btc_dominance", json_real(62.0));
        json_object_set_new(feed, "vix", json_real(18.0));
    } else {
        json_object_set_new(feed, "asset", json_string("BTC"));
        json_object_set_new(feed, "window_ts", json_integer(candle.ts));
        json_object_set_new(feed, "open", json_real(candle.open));
        json_object_set_new(feed, "high", json_real(candle.high));
        json_object_set_new(feed, "low", json_real(candle.low));
        json_object_set_new(feed, "close", json_real(candle.close));
        json_object_set_new(feed, "volume", json_real(candle.volume));
        json_object_set_new(feed, "fear_greed", json_real(fg));
        json_object_set_new(feed, "pump_score", json_real(pump));
        json_object_set_new(feed, "btc_dominance", json_real(62.0));
        json_object_set_new(feed, "vix", json_real(18.0));

        /* ─── Data Quality (E16) ─── */
        int score = 100;
        if (candle.volume <= 0) score -= 25;
        if (candle.open == candle.high && candle.high == candle.low && candle.low == candle.close && candle.close > 0)
            score -= 10;
        if (candle.close < BTC_MIN_PRICE || candle.close > BTC_MAX_PRICE) score -= 25;
        if ((now - candle.ts) > MAX_CANDLE_AGE_SECS) score -= 25;
        json_object_set_new(feed, "data_quality_score", json_integer(score > 0 ? score : 0));
    }

    /* ─── Timeline enrichment ─── */
    double sp500 = get_timeline_metric("fred_sp500", "$.value", 0);
    if (sp500 > 0) json_object_set_new(feed, "sp500", json_real(sp500));

    double vix = get_timeline_metric("fred_vix", "$.value", 18.0);
    json_object_set_new(feed, "vix", json_real(vix));

    double btc_vol, btc_mean, btc_high, btc_low;
    if (get_btc_30d_stats(&btc_vol, &btc_mean, &btc_high, &btc_low) == 0) {
        json_object_set_new(feed, "btc_30d_volatility", json_real(btc_vol));
        json_object_set_new(feed, "btc_30d_mean", json_real(btc_mean));
        json_object_set_new(feed, "btc_30d_high", json_real(btc_high));
        json_object_set_new(feed, "btc_30d_low", json_real(btc_low));
    }

    /* ─── Coingecko market context ─── */
    double cg_mcap = get_timeline_metric("coingecko_global", "$.total_market_cap_usd", 0);
    if (cg_mcap > 0) json_object_set_new(feed, "crypto_market_cap", json_real(cg_mcap));
    double btc_dom = get_timeline_metric("coingecko_global", "$.btc_dominance", 0);
    if (btc_dom > 0) json_object_set_new(feed, "btc_dominance", json_real(btc_dom));
    double active_crypto = get_timeline_metric("coingecko_global", "$.active_cryptocurrencies", 0);
    if (active_crypto > 0) json_object_set_new(feed, "active_cryptocurrencies", json_real(active_crypto));

    /* ─── Live exchange tickers (T21) ─── */
    if (cb.found) {
        json_object_set_new(feed, "cb_price", json_real(cb.price));
        json_object_set_new(feed, "cb_bid", json_real(cb.bid));
        json_object_set_new(feed, "cb_ask", json_real(cb.ask));
        json_object_set_new(feed, "cb_volume_24h", json_real(cb.volume_24h));
    }
    if (ok.found) {
        json_object_set_new(feed, "ok_price", json_real(ok.price));
        json_object_set_new(feed, "ok_bid", json_real(ok.bid));
        json_object_set_new(feed, "ok_ask", json_real(ok.ask));
        json_object_set_new(feed, "ok_volume_24h", json_real(ok.volume_24h));
    }
    if (eurusd > 0) {
        json_object_set_new(feed, "eurusd", json_real(eurusd));
    }
    /* ─── Index futures (T23) ─── */
    if (spy > 0) json_object_set_new(feed, "spy_price", json_real(spy));
    if (qqq > 0) json_object_set_new(feed, "qqq_price", json_real(qqq));
    if (dia > 0) json_object_set_new(feed, "dia_price", json_real(dia));
    /* ─── Commodities (T24) ─── */
    if (gld > 0) json_object_set_new(feed, "gld_price", json_real(gld));
    if (uso > 0) json_object_set_new(feed, "uso_price", json_real(uso));
    if (slv > 0) json_object_set_new(feed, "slv_price", json_real(slv));
    /* ─── Options data (T25) ─── */
    if (vd.price > 0) {
        json_object_set_new(feed, "vix", json_real(vd.price));
        double range = vd.max_5d - vd.min_5d;
        double vix_pctile = (range > 0) ? ((vd.price - vd.min_5d) / range) : 0.5;
        json_object_set_new(feed, "vix_5d_min", json_real(vd.min_5d));
        json_object_set_new(feed, "vix_5d_max", json_real(vd.max_5d));
        json_object_set_new(feed, "vix_5d_pctile", json_real(vix_pctile));
        /* Options risk signal: low VIX <15 (complacent), high >25 (fear) */
        const char *vix_zone = (vd.price < 15) ? "low_vol" : (vd.price > 25) ? "high_vol" : "normal";
        json_object_set_new(feed, "vix_zone", json_string(vix_zone));
    }
    /* ─── On-chain BTC data (T26) ─── */
    if (oc.found) {
        json_object_set_new(feed, "btc_hash_rate_ph_s", json_real(oc.hash_rate_ph_s));
        json_object_set_new(feed, "btc_difficulty_t", json_real(oc.difficulty_t));
        json_object_set_new(feed, "btc_24h_tx", json_real(oc.n_tx));
        json_object_set_new(feed, "btc_24h_sent_btc", json_real(oc.total_sent_btc));
        json_object_set_new(feed, "btc_trade_vol_usd", json_real(oc.trade_vol_usd));
    }
    /* ─── Temporal features (T29) ─── */
    if (tf.found) {
        json_object_set_new(feed, "price_lag_1min", json_real(tf.price_lag_1min));
        json_object_set_new(feed, "price_lag_5min", json_real(tf.price_lag_5min));
        json_object_set_new(feed, "price_lag_15min", json_real(tf.price_lag_15min));
        json_object_set_new(feed, "mom_1min_pct", json_real(tf.mom_1min));
        json_object_set_new(feed, "mom_5min_pct", json_real(tf.mom_5min));
        json_object_set_new(feed, "mom_15min_pct", json_real(tf.mom_15min));
        json_object_set_new(feed, "roll_vol_5min_pct", json_real(tf.roll_vol_5min));
        json_object_set_new(feed, "roll_vol_15min_pct", json_real(tf.roll_vol_15min));
        json_object_set_new(feed, "ma_fast", json_real(tf.ma_fast));
        json_object_set_new(feed, "ma_slow", json_real(tf.ma_slow));
        json_object_set_new(feed, "ma_cross_pct", json_real(tf.ma_cross));
        json_object_set_new(feed, "high_lag_15", json_real(tf.high_lag_15));
        json_object_set_new(feed, "low_lag_15", json_real(tf.low_lag_15));

        /* T31: Regime detection v2 — volatility + clustering regimes */
        double vol_regime = 0;  // 0=low, 1=normal, 2=high
        if (tf.roll_vol_15min < 0.01) vol_regime = 0;  // <0.01% = low vol
        else if (tf.roll_vol_15min < 0.05) vol_regime = 1;  // 0.01-0.05% = normal
        else vol_regime = 2;  // >0.05% = high vol
        json_object_set_new(feed, "vol_regime", json_real(vol_regime));

        /* Clustering regime: how tightly price clusters around MA */
        double bb_bw = (tf.high_lag_15 - tf.low_lag_15) / tf.ma_slow * 100.0;
        json_object_set_new(feed, "bb_bandwidth_pct", json_real(bb_bw));
        double cluster_regime = 0;  // 0=tight range, 1=normal, 2=wide
        if (bb_bw < 0.05) cluster_regime = 0;
        else if (bb_bw < 0.2) cluster_regime = 1;
        else cluster_regime = 2;
        json_object_set_new(feed, "cluster_regime", json_real(cluster_regime));

        /* Composite regime score (0-100): combining trend, vol, clustering */
        double trend_strength = fabs(tf.mom_15min);
        double composite = (trend_strength * 10) + (vol_regime * 15) + (cluster_regime * 10);
        if (composite > 100) composite = 100;
        json_object_set_new(feed, "composite_regime_score", json_real(composite));

        /* Regime label for room engine */
        const char *regime_label = "ranging";
        if (tf.mom_15min > 0.1 && composite > 20) regime_label = "bull";
        else if (tf.mom_15min < -0.1 && composite > 20) regime_label = "bear";
        else if (vol_regime >= 2) regime_label = "volatile";
        json_object_set_new(feed, "regime_label", json_string(regime_label));

        printf("[feed] Regime: vol=%.0f cluster=%.0f composite=%.0f label=%s\n",
               vol_regime, cluster_regime, composite, regime_label);

        /* T32: Anomaly detection — flash crash, price gaps, momentum extreme */
        double anomaly_score = 0;
        int flash_crash = 0, price_gap = 0, mom_extreme = 0;
        if (tf.mom_1min < -0.5) { flash_crash = 1; anomaly_score += 30; }
        if (tf.mom_1min > 0.5)  { anomaly_score += 20; }
        if (fabs(tf.mom_1min) > fabs(tf.mom_15min) * 3 && fabs(tf.mom_1min) > 0.1)
            { price_gap = 1; anomaly_score += 25; }
        if (fabs(tf.mom_15min) > 1.0) { mom_extreme = 1; anomaly_score += 25; }
        if (anomaly_score > 50) anomaly_score = 50;
        json_object_set_new(feed, "anomaly_score", json_real(anomaly_score));
        json_object_set_new(feed, "flash_crash_flag", json_integer(flash_crash));
        json_object_set_new(feed, "price_gap_flag", json_integer(price_gap));
        json_object_set_new(feed, "mom_extreme_flag", json_integer(mom_extreme));
        if (anomaly_score > 0)
            printf("[feed] Anomaly: score=%.0f crash=%d gap=%d extreme=%d\n",
                   anomaly_score, flash_crash, price_gap, mom_extreme);
    }
    /* ─── Cross-exchange arbitrage signals ─── */
    if (cb.found && ok.found) {
        double min_price = cb.price < ok.price ? cb.price : ok.price;
        double max_price = cb.price > ok.price ? cb.price : ok.price;
        double spread_bps = (max_price / min_price - 1.0) * 10000.0;
        double arb_signal = (cb.price - ok.price) / ((cb.price + ok.price) / 2.0) * 10000.0;
        json_object_set_new(feed, "cb_ok_spread_bps", json_real(spread_bps));
        json_object_set_new(feed, "cb_ok_arb_bps", json_real(arb_signal));
        char *arb_dir = (arb_signal > 0) ? "sell_cb_buy_ok" : (arb_signal < 0) ? "sell_ok_buy_cb" : "no_arb";
        json_object_set_new(feed, "cb_ok_arb_direction", json_string(arb_dir));
        json_object_set_new(feed, "cb_ok_arb_pct", json_real(arb_signal / 100.0));
        printf("[feed] Cross-exchange: cb=%.2f ok=%.2f spread=%.2fbps arb=%s\n",
               cb.price, ok.price, spread_bps, arb_dir);
    }
    /* Also compare with Kraken candle close if available */
    if (candle.found && cb.found) {
        double kr_spread_bps = (candle.close / cb.price - 1.0) * 10000.0;
        if (kr_spread_bps < 0) kr_spread_bps = -kr_spread_bps;
        json_object_set_new(feed, "kraken_cb_spread_bps", json_real(kr_spread_bps));
    }

    /* ─── P31: Options-implied features (from options_feat binary) ─── */
    {
        FILE *f = fopen(OPTIONS_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *of = json_loadf(f, 0, &err);
            fclose(f);
            if (of) {
                double val;

                val = json_number_value(json_object_get(of, "iv_skew"));
                if (val != 0) json_object_set_new(feed, "iv_skew", json_real(val));

                val = json_number_value(json_object_get(of, "atm_impl_move"));
                if (val > 0) json_object_set_new(feed, "atm_impl_move", json_real(val));

                val = json_number_value(json_object_get(of, "iv_term_slope"));
                if (val != 0) json_object_set_new(feed, "iv_term_slope", json_real(val));

                val = json_number_value(json_object_get(of, "near_iv"));
                if (val > 0) json_object_set_new(feed, "near_iv", json_real(val));

                val = json_number_value(json_object_get(of, "next_iv"));
                if (val > 0) json_object_set_new(feed, "next_iv", json_real(val));

                val = json_number_value(json_object_get(of, "pcr_vol"));
                if (val > 0) json_object_set_new(feed, "pcr_vol", json_real(val));

                json_decref(of);
                printf("[feed] Options features: iv_skew=%.4f atm_move=%.4f term=%.4f\n",
                       json_number_value(json_object_get(feed, "iv_skew")),
                       json_number_value(json_object_get(feed, "atm_impl_move")),
                       json_number_value(json_object_get(feed, "iv_term_slope")));
            }
        }
    }

    /* ─── P32: Earnings calendar features ─── */
    {
        FILE *f = fopen(EARNINGS_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *ef = json_loadf(f, 0, &err);
            fclose(f);
            if (ef) {
                double val;

                val = json_number_value(json_object_get(ef, "days_to_next_earnings"));
                if (val >= 0) json_object_set_new(feed, "days_to_next_earnings", json_real(val));

                val = json_number_value(json_object_get(ef, "earn_density"));
                json_object_set_new(feed, "earn_density", json_real(val));

                val = json_number_value(json_object_get(ef, "earn_activity"));
                json_object_set_new(feed, "earn_activity", json_real(val));

                const char *tkr = json_string_value(json_object_get(ef, "next_earnings_date"));
                if (tkr) json_object_set_new(feed, "next_earnings_date", json_string(tkr));

                json_decref(ef);
                printf("[feed] Earnings: next=%.0fd density=%.2f activity=%.2f\n",
                       json_number_value(json_object_get(feed, "days_to_next_earnings")),
                       json_number_value(json_object_get(feed, "earn_density")),
                       json_number_value(json_object_get(feed, "earn_activity")));
            }
        }
    }

    /* ─── P33: On-chain BTC features (CoinGecko) ─── */
    {
        FILE *f = fopen(ONCHAIN_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *of = json_loadf(f, 0, &err);
            fclose(f);
            if (of) {
                json_object_set_new(feed, "btc_market_cap", 
                    json_real(json_number_value(json_object_get(of, "btc_market_cap"))));
                json_object_set_new(feed, "btc_dominance_pct", 
                    json_real(json_number_value(json_object_get(of, "btc_dominance_pct"))));
                json_object_set_new(feed, "btc_dominance_signal", 
                    json_real(json_number_value(json_object_get(of, "btc_dominance_signal"))));
                json_object_set_new(feed, "btc_mcap_to_ath", 
                    json_real(json_number_value(json_object_get(of, "btc_mcap_to_ath"))));
                json_object_set_new(feed, "btc_vol_signal", 
                    json_real(json_number_value(json_object_get(of, "btc_vol_signal"))));
                json_object_set_new(feed, "btc_price_7d_chg", 
                    json_real(json_number_value(json_object_get(of, "btc_price_7d_chg"))));
                /* P42: Stock-to-flow model — read circ supply, compute S2F ratio */
                double circ_supply = json_number_value(json_object_get(of, "btc_circulating_supply"));
                if (circ_supply > 0) {
                    json_object_set_new(feed, "btc_circulating_supply", json_real(circ_supply));
                    /* BTC annual production: 3.125 BTC/block × 144 blocks/day × 365 days */
                    double annual_prod = 3.125 * 144.0 * 365.0;
                    double s2f = circ_supply / annual_prod;
                    /* Normalize: S2F 0-200 → 0-1 (BTC ~120, gold ~60) */
                    double s2f_norm = s2f / 200.0;
                    if (s2f_norm > 1.0) s2f_norm = 1.0;
                    if (s2f_norm < 0) s2f_norm = 0;
                    json_object_set_new(feed, "btc_s2f_ratio", json_real(s2f));
                    json_object_set_new(feed, "btc_s2f_norm", json_real(s2f_norm));
                }
                json_decref(of);
                printf("[feed] On-chain: dom=%.1f%% mcap/ath=%.3f vol_sig=%.2f\n",
                       json_number_value(json_object_get(feed, "btc_dominance_pct")),
                       json_number_value(json_object_get(feed, "btc_mcap_to_ath")),
                       json_number_value(json_object_get(feed, "btc_vol_signal")));
            }
        }
    }

    /* ─── P34: Stablecoin flow features ─── */
    {
        FILE *f = fopen(STABLECOIN_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *sf = json_loadf(f, 0, &err);
            fclose(f);
            if (sf) {
                double smb = json_number_value(json_object_get(sf, "stable_total_mcap_b"));
                double usdt_dom = json_number_value(json_object_get(sf, "usdt_dominance_pct"));
                double vol_ratio = json_number_value(json_object_get(sf, "stable_vol_ratio"));
                json_object_set_new(feed, "stable_total_mcap_b", json_real(smb));
                json_object_set_new(feed, "usdt_dominance_pct", json_real(usdt_dom));
                json_object_set_new(feed, "stable_vol_ratio", json_real(vol_ratio));
                json_decref(sf);
                printf("[feed] Stablecoin: $%.0fB mcap | USDT dom %.1f%% | vol %.2f\n",
                       smb, usdt_dom, vol_ratio);
            }
        }
    }

    /* P35: Perpetual funding rate features */
    {
        FILE *f = fopen(FUNDING_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *ff = json_loadf(f, 0, &err);
            fclose(f);
            if (ff) {
                json_object_set_new(feed, "funding_rate_norm",
                    json_real(json_number_value(json_object_get(ff, "funding_rate_norm"))));
                json_object_set_new(feed, "funding_signal",
                    json_real(json_number_value(json_object_get(ff, "funding_signal"))));
                json_decref(ff);
                printf("[feed] Funding: norm=%.3f signal=%.3f\n",
                       json_number_value(json_object_get(feed, "funding_rate_norm")),
                       json_number_value(json_object_get(feed, "funding_signal")));
            }
        }
    }

    /* P36: Open interest features */
    {
        FILE *f = fopen(OI_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *of = json_loadf(f, 0, &err);
            fclose(f);
            if (of) {
                json_object_set_new(feed, "btc_oi_signal",
                    json_real(json_number_value(json_object_get(of, "btc_oi_signal"))));
                json_object_set_new(feed, "spy_oi_signal",
                    json_real(json_number_value(json_object_get(of, "spy_oi_signal"))));
                json_decref(of);
                printf("[feed] OI: btc=%.3f spy=%.3f\n",
                       json_number_value(json_object_get(feed, "btc_oi_signal")),
                       json_number_value(json_object_get(feed, "spy_oi_signal")));
            }
        }
    }

    /* P37: L/S ratio features */
    {
        FILE *f = fopen(LS_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *lf = json_loadf(f, 0, &err);
            fclose(f);
            if (lf) {
                json_object_set_new(feed, "ls_ratio_norm",
                    json_real(json_number_value(json_object_get(lf, "ls_ratio_norm"))));
                json_object_set_new(feed, "buy_pct_norm",
                    json_real(json_number_value(json_object_get(lf, "buy_pct_norm"))));
                json_object_set_new(feed, "ls_signal_norm",
                    json_real(json_number_value(json_object_get(lf, "ls_signal_norm"))));
                json_decref(lf);
                printf("[feed] L/S: ratio_norm=%.3f buy_pct=%.3f signal=%.3f\n",
                       json_number_value(json_object_get(feed, "ls_ratio_norm")),
                       json_number_value(json_object_get(feed, "buy_pct_norm")),
                       json_number_value(json_object_get(feed, "ls_signal_norm")));
            }
        }
    }

    /* P38: Liquidation features */
    {
        FILE *f = fopen(LIQ_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *lf = json_loadf(f, 0, &err);
            fclose(f);
            if (lf) {
                json_object_set_new(feed, "liq_ls_ratio_norm",
                    json_real(json_number_value(json_object_get(lf, "liq_ls_ratio_norm"))));
                json_object_set_new(feed, "liq_intensity",
                    json_real(json_number_value(json_object_get(lf, "liq_intensity"))));
                json_object_set_new(feed, "long_dominance",
                    json_real(json_number_value(json_object_get(lf, "long_dominance"))));
                json_decref(lf);
                printf("[feed] Liq: ratio_norm=%.3f intensity=%.3f long_dom=%.3f\n",
                       json_number_value(json_object_get(feed, "liq_ls_ratio_norm")),
                       json_number_value(json_object_get(feed, "liq_intensity")),
                       json_number_value(json_object_get(feed, "long_dominance")));
            }
        }
    }

    /* P39: Whale tracking features */
    {
        FILE *f = fopen(WHALE_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *wf = json_loadf(f, 0, &err);
            fclose(f);
            if (wf) {
                json_object_set_new(feed, "large_tx_ratio",
                    json_real(json_number_value(json_object_get(wf, "large_tx_ratio"))));
                json_object_set_new(feed, "whale_activity",
                    json_real(json_number_value(json_object_get(wf, "whale_activity"))));
                json_object_set_new(feed, "acc_signal_norm",
                    json_real(json_number_value(json_object_get(wf, "acc_signal_norm"))));
                json_decref(wf);
                printf("[feed] Whale: tx_ratio=%.3f activity=%.3f acc=%.3f\n",
                       json_number_value(json_object_get(feed, "large_tx_ratio")),
                       json_number_value(json_object_get(feed, "whale_activity")),
                       json_number_value(json_object_get(feed, "acc_signal_norm")));
            }
        }
    }

    /* P40: ETF flow features */
    {
        FILE *f = fopen(ETF_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *ef = json_loadf(f, 0, &err);
            fclose(f);
            if (ef) {
                json_object_set_new(feed, "etf_flow_norm",
                    json_real(json_number_value(json_object_get(ef, "etf_flow_norm"))));
                json_object_set_new(feed, "conc_norm",
                    json_real(json_number_value(json_object_get(ef, "conc_norm"))));
                json_object_set_new(feed, "avg_flow_norm",
                    json_real(json_number_value(json_object_get(ef, "avg_flow_norm"))));
                json_decref(ef);
                printf("[feed] ETF: flow_norm=%.3f conc=%.3f avg=%.3f\n",
                       json_number_value(json_object_get(feed, "etf_flow_norm")),
                       json_number_value(json_object_get(feed, "conc_norm")),
                       json_number_value(json_object_get(feed, "avg_flow_norm")));
            }
        }
    }

    /* P41: Hash rate & mining features */
    {
        FILE *f = fopen(HASHRATE_FEAT, "r");
        if (f) {
            json_error_t err;
            json_t *hf = json_loadf(f, 0, &err);
            fclose(f);
            if (hf) {
                json_object_set_new(feed, "hash_rate_norm",
                    json_real(json_number_value(json_object_get(hf, "hash_rate_norm"))));
                json_object_set_new(feed, "difficulty_norm",
                    json_real(json_number_value(json_object_get(hf, "difficulty_norm"))));
                json_object_set_new(feed, "miner_floor_norm",
                    json_real(json_number_value(json_object_get(hf, "miner_floor_norm"))));
                json_decref(hf);
                printf("[feed] Hashrate: hash_norm=%.3f diff_norm=%.3f floor_norm=%.3f\n",
                       json_number_value(json_object_get(feed, "hash_rate_norm")),
                       json_number_value(json_object_get(feed, "difficulty_norm")),
                       json_number_value(json_object_get(feed, "miner_floor_norm")));
            }
        }
    }

    /* P44: MVRV ratio — estimate from BTC price / miner floor price */
    {
        double close_price = candle.found ? candle.close : 0;
        /* Get miner_floor_usd — read from the hashrate features we may have loaded */
        double floor_usd = json_number_value(json_object_get(feed, "miner_floor_norm"));
        if (floor_usd > 0) {
            /* Denormalize: miner_floor_norm = (floor_usd - 10000) / 140000 */
            double raw_floor = floor_usd * 140000.0 + 10000.0;
            if (close_price > 0 && raw_floor > 0) {
                double mvrv = close_price / raw_floor;
                /* Normalize MVRV: typical range 0.5-7.0, map to 0-1 */
                double mvrv_norm = (mvrv - 0.5) / 6.5;
                if (mvrv_norm < 0) mvrv_norm = 0;
                if (mvrv_norm > 1) mvrv_norm = 1;
                json_object_set_new(feed, "btc_mvrv_est", json_real(mvrv));
                json_object_set_new(feed, "btc_mvrv_norm", json_real(mvrv_norm));
                printf("[feed] MVRV: est=%.2f norm=%.3f (price=%.0f floor=%.0f)\n",
                       mvrv, mvrv_norm, close_price, raw_floor);
            }
        }
    }

    /* P45: Puell multiple */
    {
        double close_price = candle.found ? candle.close : 0;
        if (close_price > 0) {
            double puell = get_puell_multiple(close_price);
            if (puell > 0) {
                /* Normalize Puell: typical range 0.3-6.0, map 0-3 range to 0-1 */
                double puell_norm = puell / 3.0;
                if (puell_norm > 1) puell_norm = 1;
                json_object_set_new(feed, "btc_puell_multiple", json_real(puell));
                json_object_set_new(feed, "btc_puell_norm", json_real(puell_norm));
                printf("[feed] Puell: multiple=%.3f norm=%.3f\n", puell, puell_norm);
            }
        }
    }

    /* P48: Pi Cycle proxy */
    {
        double close_price = candle.found ? candle.close : 0;
        if (close_price > 0) {
            double pi = get_pi_cycle_ratio(close_price);
            if (pi > 0) {
                /* Normalize Pi: typical ratio 0.95-1.05, map 0.9-1.1 to 0-1 */
                double pi_norm = (pi - 0.9) / 0.2;
                if (pi_norm < 0) pi_norm = 0;
                if (pi_norm > 1) pi_norm = 1;
                json_object_set_new(feed, "btc_pi_cycle_ratio", json_real(pi));
                json_object_set_new(feed, "btc_pi_cycle_norm", json_real(pi_norm));
                printf("[feed] PiCycle: ratio=%.4f norm=%.3f\n", pi, pi_norm);
            }
        }
    }

    /* P49: Mayer multiple */
    {
        double close_price = candle.found ? candle.close : 0;
        if (close_price > 0) {
            double mayer = get_mayer_multiple(close_price);
            if (mayer > 0) {
                /* Normalize Mayer: typical 0.5-3.0, map 0-3 range to 0-1 */
                double mayer_norm = mayer / 3.0;
                if (mayer_norm > 1) mayer_norm = 1;
                json_object_set_new(feed, "btc_mayer_multiple", json_real(mayer));
                json_object_set_new(feed, "btc_mayer_norm", json_real(mayer_norm));
                printf("[feed] Mayer: multiple=%.3f norm=%.3f\n", mayer, mayer_norm);
            }
        }
    }

    /* P63: Dark pool / ATS volume features */
    {
        const char *dp_path = HOME_DIR "/.hermes/dark_pool_cache/SPY_darkpool_feat.json";
        FILE *dp_f = fopen(dp_path, "r");
        if (dp_f) {
            fseek(dp_f, 0, SEEK_END);
            long sz = ftell(dp_f);
            rewind(dp_f);
            char *dp_buf = malloc(sz + 1);
            if (dp_buf && fread(dp_buf, 1, sz, dp_f) == (size_t)sz) {
                dp_buf[sz] = '\0';
                json_error_t err;
                json_t *dp_root = json_loads(dp_buf, 0, &err);
                if (dp_root) {
                    json_t *features = json_object_get(dp_root, "features");
                    if (features && json_is_object(features)) {
                        json_t *j_dp_ratio = json_object_get(features, "dark_pool_ratio_norm");
                        json_t *j_dp_wow = json_object_get(features, "wow_signal_norm");
                        double dp_ratio = j_dp_ratio && json_is_real(j_dp_ratio) ? json_real_value(j_dp_ratio) : 0.3;
                        double dp_wow = j_dp_wow && json_is_real(j_dp_wow) ? json_real_value(j_dp_wow) : 0.5;
                        json_object_set_new(feed, "btc_dark_pool_ratio_norm", json_real(dp_ratio));
                        json_object_set_new(feed, "btc_dark_pool_wow_norm", json_real(dp_wow));
                        printf("[feed] DarkPool: ratio=%.4f wow=%.4f\n", dp_ratio, dp_wow);
                    }
                    json_decref(dp_root);
                }
            }
            free(dp_buf);
            fclose(dp_f);
        } else {
            json_object_set_new(feed, "btc_dark_pool_ratio_norm", json_real(0.3));
            json_object_set_new(feed, "btc_dark_pool_wow_norm", json_real(0.5));
        }
    }

    /* P64: Congressional trading features */
    {
        const char *cg_path = HOME_DIR "/.hermes/congress_cache/congress_features.json";
        FILE *cg_f = fopen(cg_path, "r");
        if (cg_f) {
            fseek(cg_f, 0, SEEK_END);
            long sz = ftell(cg_f);
            rewind(cg_f);
            char *cg_buf = malloc(sz + 1);
            if (cg_buf && fread(cg_buf, 1, sz, cg_f) == (size_t)sz) {
                cg_buf[sz] = '\0';
                json_error_t err;
                json_t *cg_root = json_loads(cg_buf, 0, &err);
                if (cg_root) {
                    json_t *features = json_object_get(cg_root, "features");
                    if (features && json_is_object(features)) {
                        json_t *j_buy = json_object_get(features, "buy_signal_norm");
                        json_t *j_div = json_object_get(features, "party_divergence_norm");
                        double cg_buy = j_buy && json_is_real(j_buy) ? json_real_value(j_buy) : 0.5;
                        double cg_div = j_div && json_is_real(j_div) ? json_real_value(j_div) : 0.5;
                        json_object_set_new(feed, "btc_congress_buy_norm", json_real(cg_buy));
                        json_object_set_new(feed, "btc_congress_div_norm", json_real(cg_div));
                        printf("[feed] Congress: buy=%.4f div=%.4f\n", cg_buy, cg_div);
                    }
                    json_decref(cg_root);
                }
            }
            free(cg_buf);
            fclose(cg_f);
        } else {
            json_object_set_new(feed, "btc_congress_buy_norm", json_real(0.5));
            json_object_set_new(feed, "btc_congress_div_norm", json_real(0.5));
        }
    }

    /* P65: Insider trading features (SEC Form 4 filings) */
    {
        const char *ins_path = HOME_DIR "/.hermes/insider_cache/insider_features.json";
        FILE *ins_f = fopen(ins_path, "r");
        if (ins_f) {
            fseek(ins_f, 0, SEEK_END);
            long sz = ftell(ins_f);
            rewind(ins_f);
            char *ins_buf = malloc(sz + 1);
            if (ins_buf && fread(ins_buf, 1, sz, ins_f) == (size_t)sz) {
                ins_buf[sz] = '\0';
                json_error_t err;
                json_t *ins_root = json_loads(ins_buf, 0, &err);
                if (ins_root) {
                    json_t *features = json_object_get(ins_root, "features");
                    if (features && json_is_object(features)) {
                        json_t *j_density = json_object_get(features, "insider_density_norm");
                        json_t *j_trend = json_object_get(features, "insider_trend_norm");
                        double in_density = j_density && json_is_real(j_density) ? json_real_value(j_density) : 0.1;
                        double in_trend = j_trend && json_is_real(j_trend) ? json_real_value(j_trend) : 0.5;
                        json_object_set_new(feed, "btc_insider_density_norm", json_real(in_density));
                        json_object_set_new(feed, "btc_insider_trend_norm", json_real(in_trend));
                        printf("[feed] Insider: density=%.4f trend=%.4f\n", in_density, in_trend);
                    }
                    json_decref(ins_root);
                }
            }
            free(ins_buf);
            fclose(ins_f);
        } else {
            json_object_set_new(feed, "btc_insider_density_norm", json_real(0.1));
            json_object_set_new(feed, "btc_insider_trend_norm", json_real(0.5));
        }
    }

    /* P66: 13F Institutional holdings features (SEC EDGAR) */
    {
        const char *f13_path = HOME_DIR "/.hermes/13f_cache/inst_features.json";
        FILE *f13_f = fopen(f13_path, "r");
        if (f13_f) {
            fseek(f13_f, 0, SEEK_END);
            long sz = ftell(f13_f);
            rewind(f13_f);
            char *f13_buf = malloc(sz + 1);
            if (f13_buf && fread(f13_buf, 1, sz, f13_f) == (size_t)sz) {
                f13_buf[sz] = '\0';
                json_error_t err;
                json_t *f13_root = json_loads(f13_buf, 0, &err);
                if (f13_root) {
                    json_t *j_density = json_object_get(f13_root, "inst_filing_density");
                    json_t *j_trend = json_object_get(f13_root, "inst_filing_trend");
                    double f13_density = j_density && json_is_real(j_density) ? json_real_value(j_density) : 0.3;
                    double f13_trend = j_trend && json_is_real(j_trend) ? json_real_value(j_trend) : 0.5;
                    json_object_set_new(feed, "btc_inst_filing_density_norm", json_real(f13_density));
                    json_object_set_new(feed, "btc_inst_filing_trend_norm", json_real(f13_trend));
                    printf("[feed] 13F: density=%.4f trend=%.4f\n", f13_density, f13_trend);
                }
                json_decref(f13_root);
            }
            free(f13_buf);
            fclose(f13_f);
        } else {
            json_object_set_new(feed, "btc_inst_filing_density_norm", json_real(0.3));
            json_object_set_new(feed, "btc_inst_filing_trend_norm", json_real(0.5));
        }
    }

    /* P67: Short interest features (MarketBeat) */
    {
        const char *short_path = HOME_DIR "/.hermes/short_cache/short_features.json";
        FILE *short_f = fopen(short_path, "r");
        if (short_f) {
            fseek(short_f, 0, SEEK_END);
            long sz = ftell(short_f);
            rewind(short_f);
            char *short_buf = malloc(sz + 1);
            if (short_buf && fread(short_buf, 1, sz, short_f) == (size_t)sz) {
                short_buf[sz] = '\0';
                json_error_t err;
                json_t *short_root = json_loads(short_buf, 0, &err);
                if (short_root) {
                    json_t *j_intensity = json_object_get(short_root, "short_intensity_norm");
                    json_t *j_trend = json_object_get(short_root, "short_trend_norm");
                    double sh_intensity = j_intensity && json_is_real(j_intensity) ? json_real_value(j_intensity) : 0.3;
                    double sh_trend = j_trend && json_is_real(j_trend) ? json_real_value(j_trend) : 0.5;
                    json_object_set_new(feed, "btc_short_intensity_norm", json_real(sh_intensity));
                    json_object_set_new(feed, "btc_short_trend_norm", json_real(sh_trend));
                    printf("[feed] Short: intensity=%.4f trend=%.4f\n", sh_intensity, sh_trend);
                }
                json_decref(short_root);
            }
            free(short_buf);
            fclose(short_f);
        } else {
            json_object_set_new(feed, "btc_short_intensity_norm", json_real(0.3));
            json_object_set_new(feed, "btc_short_trend_norm", json_real(0.5));
        }
    }

    /* P70: Earnings calendar features (yfinance) */
    {
        const char *earn_path = HOME_DIR "/.hermes/earnings_cache/earnings_features.json";
        FILE *earn_f = fopen(earn_path, "r");
        if (earn_f) {
            fseek(earn_f, 0, SEEK_END);
            long sz = ftell(earn_f);
            rewind(earn_f);
            char *earn_buf = malloc(sz + 1);
            if (earn_buf && fread(earn_buf, 1, sz, earn_f) == (size_t)sz) {
                earn_buf[sz] = '\0';
                json_error_t err;
                json_t *earn_root = json_loads(earn_buf, 0, &err);
                if (earn_root) {
                    json_t *j_beat = json_object_get(earn_root, "earnings_beat_rate_norm");
                    json_t *j_dens = json_object_get(earn_root, "earnings_density_norm");
                    double ea_beat = j_beat && json_is_real(j_beat) ? json_real_value(j_beat) : 0.5;
                    double ea_dens = j_dens && json_is_real(j_dens) ? json_real_value(j_dens) : 0.1;
                    json_object_set_new(feed, "btc_earn_beat_rate_norm", json_real(ea_beat));
                    json_object_set_new(feed, "btc_earn_density_norm", json_real(ea_dens));
                    printf("[feed] Earnings: beat_rate=%.4f density=%.4f\n", ea_beat, ea_dens);
                }
                json_decref(earn_root);
            }
            free(earn_buf);
            fclose(earn_f);
        } else {
            json_object_set_new(feed, "btc_earn_beat_rate_norm", json_real(0.5));
            json_object_set_new(feed, "btc_earn_density_norm", json_real(0.1));
        }
    }

    /* P71: ETF holdings & flow features (yfinance) */
    {
        const char *etf_path = HOME_DIR "/.hermes/etf_cache/etf_flow_features.json";
        FILE *etf_f = fopen(etf_path, "r");
        if (etf_f) {
            fseek(etf_f, 0, SEEK_END);
            long sz = ftell(etf_f);
            rewind(etf_f);
            char *etf_buf = malloc(sz + 1);
            if (etf_buf && fread(etf_buf, 1, sz, etf_f) == (size_t)sz) {
                etf_buf[sz] = '\0';
                json_error_t err;
                json_t *etf_root = json_loads(etf_buf, 0, &err);
                if (etf_root) {
                    json_t *j_conc = json_object_get(etf_root, "etf_concentration_norm");
                    json_t *j_brd = json_object_get(etf_root, "sector_breadth_norm");
                    double etf_conc = j_conc && json_is_real(j_conc) ? json_real_value(j_conc) : 0.2;
                    double etf_brd = j_brd && json_is_real(j_brd) ? json_real_value(j_brd) : 0.5;
                    json_object_set_new(feed, "btc_etf_concentration_norm", json_real(etf_conc));
                    json_object_set_new(feed, "btc_sector_breadth_norm", json_real(etf_brd));
                    printf("[feed] ETF: concentration=%.4f breadth=%.4f\n", etf_conc, etf_brd);
                }
                json_decref(etf_root);
            }
            free(etf_buf);
            fclose(etf_f);
        } else {
            json_object_set_new(feed, "btc_etf_concentration_norm", json_real(0.2));
            json_object_set_new(feed, "btc_sector_breadth_norm", json_real(0.5));
        }
    }

    /* P72: Full option chain features (yfinance) */
    {
        const char *opt_path = HOME_DIR "/.hermes/options_cache/options_chain_features.json";
        FILE *opt_f = fopen(opt_path, "r");
        if (opt_f) {
            fseek(opt_f, 0, SEEK_END);
            long sz = ftell(opt_f);
            rewind(opt_f);
            char *opt_buf = malloc(sz + 1);
            if (opt_buf && fread(opt_buf, 1, sz, opt_f) == (size_t)sz) {
                opt_buf[sz] = '\0';
                json_error_t err;
                json_t *opt_root = json_loads(opt_buf, 0, &err);
                if (opt_root) {
                    json_t *j_pcr = json_object_get(opt_root, "options_pcr_norm");
                    json_t *j_mp = json_object_get(opt_root, "options_max_pain_norm");
                    double opt_pcr = j_pcr && json_is_real(j_pcr) ? json_real_value(j_pcr) : 0.5;
                    double opt_mp = j_mp && json_is_real(j_mp) ? json_real_value(j_mp) : 0.5;
                    json_object_set_new(feed, "btc_options_pcr_norm", json_real(opt_pcr));
                    json_object_set_new(feed, "btc_options_max_pain_norm", json_real(opt_mp));
                    printf("[feed] Options: pcr=%.4f max_pain=%.4f\n", opt_pcr, opt_mp);
                }
                json_decref(opt_root);
            }
            free(opt_buf);
            fclose(opt_f);
        } else {
            json_object_set_new(feed, "btc_options_pcr_norm", json_real(0.5));
            json_object_set_new(feed, "btc_options_max_pain_norm", json_real(0.5));
        }
    }

    /* P73: Market seasonality features (pure computation from yfinance history) */
    {
        const char *seas_path = HOME_DIR "/.hermes/seasonality_cache/seasonality_features.json";
        FILE *seas_f = fopen(seas_path, "r");
        if (seas_f) {
            fseek(seas_f, 0, SEEK_END);
            long sz = ftell(seas_f);
            rewind(seas_f);
            char *seas_buf = malloc(sz + 1);
            if (seas_buf && fread(seas_buf, 1, sz, seas_f) == (size_t)sz) {
                seas_buf[sz] = '\0';
                json_error_t err;
                json_t *seas_root = json_loads(seas_buf, 0, &err);
                if (seas_root) {
                    json_t *j_dow = json_object_get(seas_root, "dow_seasonality_norm");
                    json_t *j_moy = json_object_get(seas_root, "moy_seasonality_norm");
                    double seas_dow = j_dow && json_is_real(j_dow) ? json_real_value(j_dow) : 0.5;
                    double seas_moy = j_moy && json_is_real(j_moy) ? json_real_value(j_moy) : 0.5;
                    json_object_set_new(feed, "btc_dow_seasonality_norm", json_real(seas_dow));
                    json_object_set_new(feed, "btc_moy_seasonality_norm", json_real(seas_moy));
                    printf("[feed] Seasonality: DOW=%.4f MOY=%.4f\n", seas_dow, seas_moy);
                }
                json_decref(seas_root);
            }
            free(seas_buf);
            fclose(seas_f);
        } else {
            json_object_set_new(feed, "btc_dow_seasonality_norm", json_real(0.5));
            json_object_set_new(feed, "btc_moy_seasonality_norm", json_real(0.5));
        }
    }

    /* P74: Financial news RSS features (RSS feeds) */
    {
        const char *news_path = HOME_DIR "/.hermes/news_cache/news_features.json";
        FILE *news_f = fopen(news_path, "r");
        if (news_f) {
            fseek(news_f, 0, SEEK_END);
            long sz = ftell(news_f);
            rewind(news_f);
            char *news_buf = malloc(sz + 1);
            if (news_buf && fread(news_buf, 1, sz, news_f) == (size_t)sz) {
                news_buf[sz] = '\0';
                json_error_t err;
                json_t *news_root = json_loads(news_buf, 0, &err);
                if (news_root) {
                    json_t *j_vol = json_object_get(news_root, "news_volume_norm");
                    json_t *j_sent = json_object_get(news_root, "news_sentiment_norm");
                    double n_vol = j_vol && json_is_real(j_vol) ? json_real_value(j_vol) : 0.5;
                    double n_sent = j_sent && json_is_real(j_sent) ? json_real_value(j_sent) : 0.5;
                    json_object_set_new(feed, "btc_news_volume_norm", json_real(n_vol));
                    json_object_set_new(feed, "btc_news_sentiment_norm", json_real(n_sent));
                    printf("[feed] News: volume=%.4f sentiment=%.4f\n", n_vol, n_sent);
                }
                json_decref(news_root);
            }
            free(news_buf);
            fclose(news_f);
        } else {
            json_object_set_new(feed, "btc_news_volume_norm", json_real(0.5));
            json_object_set_new(feed, "btc_news_sentiment_norm", json_real(0.5));
        }
    }

    /* P75: Politician portfolio features */
    {
        const char *pol_path = HOME_DIR "/.hermes/congress_cache/politician_portfolio_features.json";
        FILE *pol_f = fopen(pol_path, "r");
        if (pol_f) {
            fseek(pol_f, 0, SEEK_END);
            long sz = ftell(pol_f);
            rewind(pol_f);
            char *pol_buf = malloc(sz + 1);
            if (pol_buf && fread(pol_buf, 1, sz, pol_f) == (size_t)sz) {
                pol_buf[sz] = '\0';
                json_error_t err;
                json_t *pol_root = json_loads(pol_buf, 0, &err);
                if (pol_root) {
                    json_t *j_conc = json_object_get(pol_root, "pol_portfolio_conc_norm");
                    json_t *j_conv = json_object_get(pol_root, "pol_conviction_norm");
                    double pol_conc = j_conc && json_is_real(j_conc) ? json_real_value(j_conc) : 0.5;
                    double pol_conv = j_conv && json_is_real(j_conv) ? json_real_value(j_conv) : 0.5;
                    json_object_set_new(feed, "btc_pol_portfolio_conc_norm", json_real(pol_conc));
                    json_object_set_new(feed, "btc_pol_conviction_norm", json_real(pol_conv));
                    printf("[feed] Politician: conc=%.4f conviction=%.4f\n", pol_conc, pol_conv);
                }
                json_decref(pol_root);
            }
            free(pol_buf);
            fclose(pol_f);
        } else {
            json_object_set_new(feed, "btc_pol_portfolio_conc_norm", json_real(0.5));
            json_object_set_new(feed, "btc_pol_conviction_norm", json_real(0.5));
        }
    }

    /* T34: Order book depth features */
    {
        const char *ob_path = HOME_DIR "/.hermes/orderbook_cache/orderbook_features.json";
        FILE *ob_f = fopen(ob_path, "r");
        if (ob_f) {
            fseek(ob_f, 0, SEEK_END);
            long sz = ftell(ob_f);
            rewind(ob_f);
            char *ob_buf = malloc(sz + 1);
            if (ob_buf && fread(ob_buf, 1, sz, ob_f) == (size_t)sz) {
                ob_buf[sz] = '\0';
                json_error_t err;
                json_t *ob_root = json_loads(ob_buf, 0, &err);
                if (ob_root) {
                    json_t *j_imb = json_object_get(ob_root, "ob_imbalance_norm");
                    json_t *j_dep = json_object_get(ob_root, "ob_depth_ratio_norm");
                    json_t *j_wal = json_object_get(ob_root, "ob_wall_conc_norm");
                    json_t *j_spr = json_object_get(ob_root, "ob_spread_norm");
                    double ob_imb = j_imb && json_is_real(j_imb) ? json_real_value(j_imb) : 0.5;
                    double ob_dep = j_dep && json_is_real(j_dep) ? json_real_value(j_dep) : 0.5;
                    double ob_wal = j_wal && json_is_real(j_wal) ? json_real_value(j_wal) : 0.0;
                    double ob_spr = j_spr && json_is_real(j_spr) ? json_real_value(j_spr) : 0.0;
                    json_object_set_new(feed, "btc_ob_imbalance_norm", json_real(ob_imb));
                    json_object_set_new(feed, "btc_ob_depth_ratio_norm", json_real(ob_dep));
                    json_object_set_new(feed, "btc_ob_wall_conc_norm", json_real(ob_wal));
                    json_object_set_new(feed, "btc_ob_spread_norm", json_real(ob_spr));
                    printf("[feed] Orderbook: imb=%.4f depth=%.4f wall=%.4f spread=%.4f\n",
                           ob_imb, ob_dep, ob_wal, ob_spr);
                }
                json_decref(ob_root);
            }
            free(ob_buf);
            fclose(ob_f);
        } else {
            json_object_set_new(feed, "btc_ob_imbalance_norm", json_real(0.5));
            json_object_set_new(feed, "btc_ob_depth_ratio_norm", json_real(0.5));
            json_object_set_new(feed, "btc_ob_wall_conc_norm", json_real(0.0));
            json_object_set_new(feed, "btc_ob_spread_norm", json_real(0.0));
        }
    }

    /* Write atomically: tmp -> rename */
    if (json_dump_file(feed, FEED_TMP, JSON_INDENT(2) | JSON_SORT_KEYS) != 0) {
        fprintf(stderr, "[feed] Failed to write " FEED_TMP "\n");
        json_decref(feed);
        return 1;
    }
    rename(FEED_TMP, MARKET_FEED);

    printf("[feed] wrote feed: BTC ts=%ld close=%.2f pump=%.3f score=%d\n",
           candle.found ? candle.ts : window_ts,
           candle.found ? candle.close : 0.0,
           pump,
           candle.found ? 100 : 0);
    fflush(stdout);

    json_decref(feed);
    return 0;
}

/* ─── Main ─── */
int main(int argc, char **argv) {
    /* Write heartbeat */
    char hb_path[256];
    snprintf(hb_path, sizeof(hb_path), HOME_DIR "/.hermes/infra/heartbeats/room-feed.heartbeat");
    FILE *f = fopen(hb_path, "w");
    if (f) {
        fprintf(f, "%ld", (long)time(NULL));
        fclose(f);
    }

    return write_feed();
}
