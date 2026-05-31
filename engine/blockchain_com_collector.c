/**
 * blockchain_com_collector.c — Blockchain.com BTC on-chain data (T964-T978)
 * Fetches: tx count, fees, mempool, miner revenue, UTXOs, supply, non-zero addresses
 * Free API, no key needed. All data via blockchain.info/charts API.
 * 
 * Compile: gcc -O3 -Wall -Wextra -o blockchain_com_collector blockchain_com_collector.c -lcurl -lsqlite3 -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <time.h>
#include <jansson.h>

#define DB_PATH "/home/wubu2/money-room/engine/timeline.db"
#define CACHE_PATH "/home/wubu2/money-room/data/blockchain_cache"
#define API_BASE "https://api.blockchain.info/charts"

typedef struct {
    const char *chart;
    const char *name;
    const char *unit;
    int timespan_days;
} ChartDef;

ChartDef CHARTS[] = {
    {"n-transactions",      "Transaction Count",         "tx/day",      7},
    {"n-transactions-per-block", "Tx Per Block",          "tx/block",    7},
    {"transaction-fees",    "Total Transaction Fees",    "BTC/day",     7},
    {"transaction-fees-usd","Transaction Fees (USD)",    "USD/day",     7},
    {"mempool-size",        "Mempool Size",              "tx pending",  1},
    {"mempool-count",       "Mempool Count",             "tx",          1},
    {"miners-revenue",      "Miner Revenue",             "BTC/day",     30},
    {"utxo-count",          "UTXO Count",                 "utxos",       7},
    {"total-bitcoins",      "Total Supply",              "BTC",         7},
    {"market-price",        "BTC Market Price",           "USD",         1},
    {"n-unique-addresses",  "Unique Addresses",           "addrs/day",   7},
    {"estimated-transaction-volume-usd","Est Volume (USD)","USD/day",     7},
    {"trade-volume",        "Exchange Trade Volume",      "USD/day",     7},
    {"hash-rate",           "Hash Rate",                  "TH/s",        7},
    {"difficulty",          "Difficulty",                 "ratio",       30},
};
const int N_CHARTS = sizeof(CHARTS) / sizeof(CHARTS[0]);

typedef struct { char *data; size_t size; } MemBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    MemBuf *buf = (MemBuf*)userdata;
    size_t total = size * nmemb;
    char *new_data = realloc(buf->data, buf->size + total + 1);
    if (!new_data) return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static sqlite3 *open_db(void) {
    sqlite3 *db;
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) { fprintf(stderr, "DB: %s\n", sqlite3_errmsg(db)); return NULL; }
    
    const char *sql =
        "CREATE TABLE IF NOT EXISTS blockchain_data ("
        "  chart_id TEXT NOT NULL,"
        "  obs_date TEXT NOT NULL,"
        "  value REAL,"
        "  name TEXT,"
        "  updated_at TEXT DEFAULT (datetime('now')),"
        "  PRIMARY KEY (chart_id, obs_date)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_bc_chart ON blockchain_data(chart_id);";
    char *err = NULL;
    sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) { fprintf(stderr, "DB init: %s\n", err); sqlite3_free(err); }
    return db;
}

static int fetch_chart(const ChartDef *cd, MemBuf *buf) {
    char url[512];
    snprintf(url, sizeof(url), "%s/%s?format=json&timespan=%dd&sampled=false",
             API_BASE, cd->chart, cd->timespan_days);
    
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    
    buf->size = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MoneyRoom/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    
    return (res == CURLE_OK && http == 200) ? 0 : -1;
}

static int insert_chart(sqlite3 *db, const ChartDef *cd, MemBuf *buf) {
    json_error_t err;
    json_t *root = json_loads(buf->data, 0, &err);
    if (!root) {
        fprintf(stderr, "  JSON: %s\n", err.text);
        return -1;
    }
    
    json_t *values = json_object_get(root, "values");
    if (!values || !json_is_array(values)) {
        json_decref(root);
        return -1;
    }
    
    size_t n = json_array_size(values);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO blockchain_data (chart_id, obs_date, value, name) VALUES (?1, ?2, ?3, ?4)",
        -1, &stmt, NULL);
    
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    int rows = 0;
    
    for (size_t i = 0; i < n; i++) {
        json_t *val = json_array_get(values, i);
        json_t *x = json_object_get(val, "x");  /* unix timestamp */
        json_t *y = json_object_get(val, "y");  /* value */
        
        if (x && y && json_is_number(x) && json_is_number(y)) {
            time_t ts = (time_t)json_number_value(x);
            struct tm *tm = gmtime(&ts);
            char date_str[32];
            strftime(date_str, sizeof(date_str), "%Y%m%d", tm);
            
            double v = json_number_value(y);
            
            sqlite3_bind_text(stmt, 1, cd->chart, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, date_str, -1, SQLITE_STATIC);
            sqlite3_bind_double(stmt, 3, v);
            sqlite3_bind_text(stmt, 4, cd->name, -1, SQLITE_STATIC);
            
            if (sqlite3_step(stmt) == SQLITE_DONE) rows++;
            sqlite3_reset(stmt);
        }
    }
    
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_finalize(stmt);
    json_decref(root);
    
    printf("  %-25s %d rows\n", cd->chart, rows);
    return rows;
}

static void print_stats(sqlite3 *db) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT chart_id, COUNT(*), ROUND(AVG(value), 2), ROUND(MAX(value), 2), "
        "ROUND(MIN(value), 2), name "
        "FROM blockchain_data GROUP BY chart_id ORDER BY chart_id",
        -1, &stmt, NULL);
    
    printf("\n%-30s %8s %12s %12s %12s  %s\n", "CHART", "ROWS", "AVG", "MAX", "MIN", "NAME");
    printf("------------------------------ -------- ------------ ------------ ------------ "
           "--------------------------------\n");
    int total = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%-30s %8d %11.2f %11.2f %11.2f  %s\n",
               sqlite3_column_text(stmt, 0),
               sqlite3_column_int(stmt, 1),
               sqlite3_column_double(stmt, 2),
               sqlite3_column_double(stmt, 3),
               sqlite3_column_double(stmt, 4),
               sqlite3_column_text(stmt, 5));
        total += sqlite3_column_int(stmt, 1);
    }
    printf("------------------------------ -------- ------------ ------------ ------------ "
           "--------------------------------\n");
    printf("%-30s %8d\n", "TOTAL", total);
    sqlite3_finalize(stmt);
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "stats") == 0 || strcmp(argv[1], "status") == 0)) {
        sqlite3 *db = open_db();
        if (db) { print_stats(db); sqlite3_close(db); }
        return 0;
    }
    
    if (argc == 1 || (argc > 1 && strcmp(argv[1], "fetch") == 0)) {
        sqlite3 *db = open_db();
        if (!db) return 1;
        
        curl_global_init(CURL_GLOBAL_DEFAULT);
        MemBuf buf = {NULL, 0};
        
        int ok = 0, fail = 0;
        for (int i = 0; i < N_CHARTS; i++) {
            printf("[%d/%d] %s\n", i+1, N_CHARTS, CHARTS[i].name);
            if (buf.data) { free(buf.data); buf.data = NULL; buf.size = 0; }
            
            if (fetch_chart(&CHARTS[i], &buf) == 0) {
                if (insert_chart(db, &CHARTS[i], &buf) > 0) ok++;
                else fail++;
            } else {
                fail++;
            }
        }
        
        free(buf.data);
        sqlite3_close(db);
        curl_global_cleanup();
        
        printf("\n=== BLOCKCHAIN COLLECTOR RESULT ===\n");
        printf("Charts OK: %d, FAIL: %d\n", ok, fail);
        return 0;
    }
    
    printf("Usage: blockchain_com_collector [fetch|stats]\n");
    return 0;
}
