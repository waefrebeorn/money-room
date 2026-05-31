/**
 * exchange_market_collector.c — Unified exchange public market data (T1184-T1215)
 * Fetches order books, candles, trades, tickers from 7 exchanges
 * All public REST APIs, no API keys needed.
 * 
 * Compile: gcc -O3 -Wall -Wextra -o exchange_market_collector exchange_market_collector.c -lcurl -lsqlite3 -lm -ljansson
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <jansson.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

#define DB_PATH "/home/wubu2/money-room/engine/timeline.db"
#define MAX_BUF 524288
#define SAFE_ATOF(jobj, key) ({ json_t *_j = json_object_get(jobj, key); _j ? atof(json_string_value(_j)) : 0; })

typedef struct { char *data; size_t size; } MemBuf;
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *u) {
    MemBuf *b = (MemBuf*)u; size_t t = size*nmemb;
    char *n = realloc(b->data, b->size+t+1); if(!n) return 0;
    b->data=n; memcpy(b->data+b->size,ptr,t); b->size+=t; b->data[b->size]=0; return t;
}

static sqlite3 *open_db(void) {
    sqlite3 *db; int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS exchange_data ("
        "  exchange TEXT, symbol TEXT, data_type TEXT, obs_time INTEGER,"
        "  value1 REAL, value2 REAL, value3 REAL, value4 REAL, value5 REAL,"
        "  json_data TEXT,"
        "  created_at TEXT DEFAULT (datetime('now')),"
        "  PRIMARY KEY (exchange, symbol, data_type, obs_time)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_exdata ON exchange_data(exchange, data_type, obs_time);",
        NULL, NULL, NULL);
    return db;
}

/* HTTP GET helper */
static int http_get(const char *url, MemBuf *buf, int timeout) {
    CURL *curl = curl_easy_init(); if(!curl) return -1;
    buf->size = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MoneyRoom/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    long http = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK && http == 200) ? 0 : -1;
}

static void insert_row(sqlite3 *db, const char *ex, const char *sym, const char *type,
                        time_t t, double v1, double v2, double v3, double v4, double v5,
                        const char *json_str) {
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO exchange_data (exchange, symbol, data_type, obs_time, "
        "value1, value2, value3, value4, value5, json_data) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
        -1, &s, NULL);
    sqlite3_bind_text(s,1,ex,-1,SQLITE_STATIC); sqlite3_bind_text(s,2,sym,-1,SQLITE_STATIC);
    sqlite3_bind_text(s,3,type,-1,SQLITE_STATIC); sqlite3_bind_int64(s,4,(sqlite3_int64)t);
    sqlite3_bind_double(s,5,v1); sqlite3_bind_double(s,6,v2);
    sqlite3_bind_double(s,7,v3); sqlite3_bind_double(s,8,v4); sqlite3_bind_double(s,9,v5);
    if(json_str) sqlite3_bind_text(s,10,json_str,-1,SQLITE_STATIC);
    sqlite3_step(s); sqlite3_reset(s); sqlite3_finalize(s);
}

/* ─── BINANCE ─── */
static int binance_ticker(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data, 0, NULL); free(b.data); if(!j) return -1;
    if(json_object_get(j,"code")) { json_decref(j); return -1; }
    double price = json_number_value(json_object_get(j,"lastPrice"));
    double vol = json_number_value(json_object_get(j,"volume"));
    double chg = json_number_value(json_object_get(j,"priceChangePercent"));
    double high = json_number_value(json_object_get(j,"highPrice"));
    double low = json_number_value(json_object_get(j,"lowPrice"));
    time_t t = time(NULL);
    insert_row(db,"binance","BTCUSDT","ticker",t,price,chg,vol,high,low,NULL);
    printf("  Binance BTC: %.2f (%.2f%%) Vol=%.0f\n", price, chg, vol);
    json_decref(j); return 0;
}

static int binance_orderbook(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=20", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data, 0, NULL); free(b.data); if(!j) return -1;
    if(json_object_get(j,"code")) { json_decref(j); return -1; }
    json_t *bids = json_object_get(j,"bids"), *asks = json_object_get(j,"asks");
    if(!json_is_array(bids)||!json_is_array(asks)||json_array_size(bids)==0) { json_decref(j); return -1; }
    double best_bid = atof(json_string_value(json_array_get(json_array_get(bids,0),0)));
    double best_ask = atof(json_string_value(json_array_get(json_array_get(asks,0),0)));
    double bid_vol = 0, ask_vol = 0, bid_sum=0, ask_sum=0;
    size_t nb = json_array_size(bids), na = json_array_size(asks);
    for(size_t i=0;i<nb&&i<20;i++) {
        double v = atof(json_string_value(json_array_get(json_array_get(bids,i),1)));
        bid_vol += v; bid_sum += v * atof(json_string_value(json_array_get(json_array_get(bids,i),0)));
    }
    for(size_t i=0;i<na&&i<20;i++) {
        double v = atof(json_string_value(json_array_get(json_array_get(asks,i),1)));
        ask_vol += v; ask_sum += v * atof(json_string_value(json_array_get(json_array_get(asks,i),0)));
    }
    time_t t = time(NULL);
    double imbalance = (bid_vol - ask_vol) / (bid_vol + ask_vol + 1);
    double spread_bps = (best_ask - best_bid) / best_bid * 10000;
    insert_row(db,"binance","BTCUSDT","orderbook",t,best_bid,best_ask,spread_bps,imbalance,bid_vol+ask_vol,NULL);
    printf("  Binance OB: bid=%.2f ask=%.2f spread=%.1fbps imbalance=%.3f\n",
           best_bid,best_ask,spread_bps,imbalance);
    json_decref(j); return 0;
}

/* ─── COINBASE ─── */
static int coinbase_ticker(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://api.exchange.coinbase.com/products/BTC-USD/ticker", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data, 0, NULL); free(b.data); if(!j) return -1;
    json_t *jp = json_object_get(j,"price");
    json_t *jb = json_object_get(j,"bid");
    json_t *ja = json_object_get(j,"ask");
    json_t *jv = json_object_get(j,"volume");
    double price = jp ? atof(json_string_value(jp)) : 0;
    double bid = jb ? atof(json_string_value(jb)) : 0;
    double ask = ja ? atof(json_string_value(ja)) : 0;
    double vol = jv ? atof(json_string_value(jv)) : 0;
    time_t t = time(NULL);
    insert_row(db,"coinbase","BTC-USD","ticker",t,price,0,vol,bid,ask,NULL);
    printf("  Coinbase BTC: %.2f bid=%.2f ask=%.2f Vol=%.0f\n", price, bid, ask, vol);
    json_decref(j); return 0;
}

static int coinbase_orderbook(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://api.exchange.coinbase.com/products/BTC-USD/book?level=2", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data, 0, NULL); free(b.data); if(!j) return -1;
    json_t *bids = json_object_get(j,"bids"), *asks = json_object_get(j,"asks");
    if(!json_is_array(bids)||!json_is_array(asks)) { json_decref(j); return -1; }
    double best_bid = atof(json_string_value(json_array_get(json_array_get(bids,0),0)));
    double best_ask = atof(json_string_value(json_array_get(json_array_get(asks,0),0)));
    double bid_vol=0, ask_vol=0;
    size_t nb = json_array_size(bids), na = json_array_size(asks);
    for(size_t i=0;i<nb&&i<20;i++) bid_vol += atof(json_string_value(json_array_get(json_array_get(bids,i),1)));
    for(size_t i=0;i<na&&i<20;i++) ask_vol += atof(json_string_value(json_array_get(json_array_get(asks,i),1)));
    time_t t = time(NULL);
    double imbalance = (bid_vol - ask_vol) / (bid_vol + ask_vol + 1);
    double spread_bps = (best_ask - best_bid) / best_bid * 10000;
    insert_row(db,"coinbase","BTC-USD","orderbook",t,best_bid,best_ask,spread_bps,imbalance,bid_vol+ask_vol,NULL);
    printf("  Coinbase OB: bid=%.2f ask=%.2f spread=%.1fbps\n", best_bid, best_ask, spread_bps);
    json_decref(j); return 0;
}

/* ─── KRAKEN ─── */
static int kraken_ticker(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://api.kraken.com/0/public/Ticker?pair=XBTUSD", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    json_t *res = json_object_get(j,"result");
    if(!res) { json_decref(j); return -1; }
    /* Kraken uses XXBTZUSD as key */
    const char *keys[] = {"XXBTZUSD","XBTUSDT","XBTEUR",NULL};
    json_t *pair = NULL;
    for(int i=0;keys[i];i++) { pair = json_object_get(res,keys[i]); if(pair) break; }
    /* Try first key */
    void *iter = json_object_iter(res);
    if(!pair && iter) pair = json_object_iter_value(iter);
    if(!pair) { json_decref(j); return -1; }
    json_t *c = json_object_get(pair,"c");
    json_t *v = json_object_get(pair,"v");
    json_t *p = json_object_get(pair,"p");
    double price = c&&json_is_array(c)?atof(json_string_value(json_array_get(c,0))):0;
    double vol = v&&json_is_array(v)?atof(json_string_value(json_array_get(v,1))):0;
    double vwap = p&&json_is_array(p)?atof(json_string_value(json_array_get(p,1))):0;
    time_t t = time(NULL);
    insert_row(db,"kraken","XBTUSD","ticker",t,price,0,vol,vwap,0,NULL);
    printf("  Kraken BTC: %.2f Vol=%.2f VWAP=%.2f\n", price, vol, vwap);
    json_decref(j); return 0;
}

/* ─── OKX ─── */
static int okx_ticker(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://www.okx.com/api/v5/market/ticker?instId=BTC-USDT", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    json_t *data = json_object_get(j,"data");
    if(!json_is_array(data)||json_array_size(data)==0) { json_decref(j); return -1; }
    json_t *t = json_array_get(data,0);
    json_t *jlast = json_object_get(t,"last");
    json_t *jvol = json_object_get(t,"volCcy24h");
    json_t *jopen = json_object_get(t,"open24h");
    json_t *jbid = json_object_get(t,"bidPx");
    json_t *jask = json_object_get(t,"askPx");
    double price = jlast ? atof(json_string_value(jlast)) : 0;
    double vol = jvol ? atof(json_string_value(jvol)) : 0;
    double open24 = jopen ? atof(json_string_value(jopen)) : 0;
    double chg = open24 > 0 ? (price - open24) / open24 * 100 : 0;
    double bid = jbid ? atof(json_string_value(jbid)) : 0;
    double ask = jask ? atof(json_string_value(jask)) : 0;
    time_t now = time(NULL);
    insert_row(db,"okx","BTC-USDT","ticker",now,price,chg,vol,bid,ask,NULL);
    printf("  OKX BTC: %.2f chg=%.2f%% bid=%.2f ask=%.2f\n", price, chg, bid, ask);
    json_decref(j); return 0;
}

static int okx_orderbook(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://www.okx.com/api/v5/market/books?instId=BTC-USDT&sz=20", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    json_t *data = json_object_get(j,"data");
    if(!json_is_array(data)||json_array_size(data)==0) { json_decref(j); return -1; }
    json_t *book = json_array_get(data,0);
    json_t *bids = json_object_get(book,"bids"), *asks = json_object_get(book,"asks");
    if(!json_is_array(bids)||!json_is_array(asks)) { json_decref(j); return -1; }
    double best_bid = atof(json_string_value(json_array_get(json_array_get(bids,0),0)));
    double best_ask = atof(json_string_value(json_array_get(json_array_get(asks,0),0)));
    double bid_vol=0, ask_vol=0;
    size_t nb = json_array_size(bids), na = json_array_size(asks);
    for(size_t i=0;i<nb&&i<20;i++) bid_vol += atof(json_string_value(json_array_get(json_array_get(bids,i),1)));
    for(size_t i=0;i<na&&i<20;i++) ask_vol += atof(json_string_value(json_array_get(json_array_get(asks,i),1)));
    time_t now = time(NULL);
    double spread_bps = (best_ask - best_bid) / best_bid * 10000;
    double imbalance = (bid_vol - ask_vol) / (bid_vol + ask_vol + 1);
    insert_row(db,"okx","BTC-USDT","orderbook",now,best_bid,best_ask,spread_bps,imbalance,bid_vol+ask_vol,NULL);
    printf("  OKX OB: bid=%.2f ask=%.2f spread=%.1fbps\n", best_bid, best_ask, spread_bps);
    json_decref(j); return 0;
}

static int okx_funding(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://www.okx.com/api/v5/public/funding-rate?instId=BTC-USD-SWAP", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    json_t *data = json_object_get(j,"data");
    if(!json_is_array(data)||json_array_size(data)==0) { json_decref(j); return -1; }
    json_t *fr = json_array_get(data,0);
    double rate = atof(json_string_value(json_object_get(fr,"fundingRate")));
    double next = atof(json_string_value(json_object_get(fr,"nextFundingRate")));
    time_t now = time(NULL);
    insert_row(db,"okx","BTC-USD-SWAP","funding",now,rate,next,0,0,0,NULL);
    printf("  OKX Funding: %.6f next=%.6f\n", rate, next);
    json_decref(j); return 0;
}

static int okx_oi(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://www.okx.com/api/v5/public/open-interest?instId=BTC-USDT-SWAP", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    json_t *data = json_object_get(j,"data");
    if(!json_is_array(data)||json_array_size(data)==0) { json_decref(j); return -1; }
    json_t *oi = json_array_get(data,0);
    double oi_val = atof(json_string_value(json_object_get(oi,"oi")));
    double oi_ccy = atof(json_string_value(json_object_get(oi,"oiCcy")));
    time_t now = time(NULL);
    insert_row(db,"okx","BTC-USDT-SWAP","open_interest",now,oi_val,oi_ccy,0,0,0,NULL);
    printf("  OKX OI: %.2f (%.2f USD)\n", oi_val, oi_ccy);
    json_decref(j); return 0;
}

static int okx_ls_ratio(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://www.okx.com/api/v5/rubik/stat/contracts/long-short-account-ratio?instId=BTC-USDT&period=1h", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    json_t *data = json_object_get(j,"data");
    if(!json_is_array(data)||json_array_size(data)==0) { json_decref(j); return -1; }
    json_t *ls = json_array_get(data,0);
    double ratio = atof(json_string_value(json_object_get(ls,"longShortRatio")));
    time_t now = time(NULL);
    insert_row(db,"okx","BTC-USDT-SWAP","ls_ratio",now,ratio,0,0,0,0,NULL);
    printf("  OKX L/S: %.3f\n", ratio);
    json_decref(j); return 0;
}

/* ─── BITFINEX ─── */
static int bitfinex_ticker(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://api-pub.bitfinex.com/v2/ticker/tBTCUSD", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    if(!json_is_array(j)||json_array_size(j)<3) { json_decref(j); return -1; }
    double bid = json_number_value(json_array_get(j,0));
    double ask = json_number_value(json_array_get(j,2));
    double price = json_number_value(json_array_get(j,6));
    double vol = fabs(json_number_value(json_array_get(j,7))); /* may be negative per Bitfinex */
    time_t now = time(NULL);
    insert_row(db,"bitfinex","BTCUSD","ticker",now,price,0,vol,bid,ask,NULL);
    printf("  Bitfinex BTC: %.2f bid=%.2f ask=%.2f\n", price, bid, ask);
    json_decref(j); return 0;
}

static int bitfinex_orderbook(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://api-pub.bitfinex.com/v2/book/tBTCUSD/P0", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    if(!json_is_array(j)) { json_decref(j); return -1; }
    double best_bid=0, best_ask=0, bid_vol=0, ask_vol=0;
    size_t n = json_array_size(j);
    for(size_t i=0;i<n;i++) {
        json_t *row = json_array_get(j,i);
        double price = json_number_value(json_array_get(row,0));
        double qty = json_number_value(json_array_get(row,2));
        if(qty > 0) { /* bid */
            if(best_bid==0||price>best_bid) best_bid=price;
            bid_vol += qty;
        } else { /* ask */
            if(best_ask==0||(price<best_ask||best_ask==0)) best_ask=price;
            ask_vol += fabs(qty);
        }
    }
    time_t now = time(NULL);
    double spread_bps = best_ask>0&&best_bid>0?(best_ask-best_bid)/best_bid*10000:0;
    double imbalance = (bid_vol-ask_vol)/(bid_vol+ask_vol+1);
    insert_row(db,"bitfinex","BTCUSD","orderbook",now,best_bid,best_ask,spread_bps,imbalance,bid_vol+ask_vol,NULL);
    printf("  Bitfinex OB: bid=%.2f ask=%.2f spread=%.1fbps\n", best_bid, best_ask, spread_bps);
    json_decref(j); return 0;
}

/* ─── GATE.IO ─── */
static int gate_ticker(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://api.gateio.ws/api/v4/spot/tickers?currency_pair=BTC_USDT", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    if(!json_is_array(j)||json_array_size(j)==0) { json_decref(j); return -1; }
    json_t *t = json_array_get(j,0);
    json_t *jlast = json_object_get(t,"last");
    json_t *jvol = json_object_get(t,"quote_volume");
    json_t *jchg = json_object_get(t,"change_percentage");
    double price = jlast ? atof(json_string_value(jlast)) : 0;
    double vol = jvol ? atof(json_string_value(jvol)) : 0;
    double chg = jchg ? atof(json_string_value(jchg)) : 0;
    time_t now = time(NULL);
    insert_row(db,"gateio","BTC_USDT","ticker",now,price,chg,vol,0,0,NULL);
    printf("  Gate.io BTC: %.2f chg=%.2f%% Vol=%.0f\n", price, chg, vol);
    json_decref(j); return 0;
}

/* ─── KUCOIN ─── */
static int kucoin_ticker(sqlite3 *db) {
    MemBuf b = {0}; int r = http_get("https://api.kucoin.com/api/v1/market/orderbook/level1?symbol=BTC-USDT", &b, 10);
    if(r) { free(b.data); return -1; }
    json_t *j = json_loads(b.data,0,NULL); free(b.data); if(!j) return -1;
    json_t *data = json_object_get(j,"data");
    if(!data) { json_decref(j); return -1; }
    double price = atof(json_string_value(json_object_get(data,"price")));
    double bid = atof(json_string_value(json_object_get(data,"bestBid")));
    double ask = atof(json_string_value(json_object_get(data,"bestAsk")));
    double vol = atof(json_string_value(json_object_get(data,"size")));
    time_t now = time(NULL);
    // Get 24h stats
    MemBuf b2 = {0}; 
    r = http_get("https://api.kucoin.com/api/v1/market/stats?symbol=BTC-USDT", &b2, 10);
    double chg=0;
    if(r==0) {
        json_t *j2 = json_loads(b2.data,0,NULL);
        if(j2) {
            json_t *d2 = json_object_get(j2,"data");
            if(d2) chg = atof(json_string_value(json_object_get(d2,"changeRate")))*100;
            json_decref(j2);
        }
    }
    free(b2.data);
    insert_row(db,"kucoin","BTC-USDT","ticker",now,price,chg,vol,bid,ask,NULL);
    printf("  KuCoin BTC: %.2f chg=%.2f%% bid=%.2f\n", price, chg, bid);
    json_decref(j); return 0;
}

int main(int argc, char **argv) {
    int opt_all = 1;
    
    if(argc > 1) {
        if(strcmp(argv[1],"stats")==0) {
            sqlite3 *db = open_db(); if(!db) return 1;
            sqlite3_stmt *s;
            sqlite3_prepare_v2(db,
                "SELECT exchange, data_type, COUNT(*) FROM exchange_data GROUP BY exchange, data_type ORDER BY exchange",
                -1, &s, NULL);
            printf("\n%-12s %-15s %8s\n", "EXCHANGE", "TYPE", "ROWS");
            printf("------------ --------------- --------\n");
            while(sqlite3_step(s)==SQLITE_ROW)
                printf("%-12s %-15s %8d\n", sqlite3_column_text(s,0), sqlite3_column_text(s,1), sqlite3_column_int(s,2));
            sqlite3_finalize(s);
            sqlite3_close(db);
            return 0;
        }
        opt_all = 0; /* specific exchange requested */
    }
    
    sqlite3 *db = open_db(); if(!db) return 1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    time_t start = time(NULL);
    int ok=0, fail=0;
    
    if(opt_all || (argc>1 && strcmp(argv[1],"binance")==0)) {
        sleep(1);
        if(binance_ticker(db)==0) ok++; else fail++;
        sleep(1);
        if(binance_orderbook(db)==0) ok++; else fail++;
    }
    if(opt_all || (argc>1 && strcmp(argv[1],"coinbase")==0)) {
        if(coinbase_ticker(db)==0) ok++; else fail++;
        if(coinbase_orderbook(db)==0) ok++; else fail++;
    }
    if(opt_all || (argc>1 && strcmp(argv[1],"kraken")==0)) {
        if(kraken_ticker(db)==0) ok++; else fail++;
    }
    if(opt_all || (argc>1 && strcmp(argv[1],"okx")==0)) {
        if(okx_ticker(db)==0) ok++; else fail++;
        if(okx_orderbook(db)==0) ok++; else fail++;
        if(okx_funding(db)==0) ok++; else fail++;
        if(okx_oi(db)==0) ok++; else fail++;
        if(okx_ls_ratio(db)==0) ok++; else fail++;
    }
    if(opt_all || (argc>1 && strcmp(argv[1],"bitfinex")==0)) {
        if(bitfinex_ticker(db)==0) ok++; else fail++;
        if(bitfinex_orderbook(db)==0) ok++; else fail++;
    }
    if(opt_all || (argc>1 && strcmp(argv[1],"gate")==0)) {
        if(gate_ticker(db)==0) ok++; else fail++;
    }
    if(opt_all || (argc>1 && strcmp(argv[1],"kucoin")==0)) {
        if(kucoin_ticker(db)==0) ok++; else fail++;
    }
    
    time_t elapsed = time(NULL) - start;
    sqlite3_close(db);
    curl_global_cleanup();
    
    printf("\n=== EXCHANGE COLLECTOR RESULT ===\n");
    printf("OK: %d, FAIL: %d, Time: %lds\n", ok, fail, (long)elapsed);
    return 0;
}
