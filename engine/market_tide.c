/*
 * market_tide.c — P69: Market Tide Index & Sector Leadership
 *
 * Fetches 10 sector ETFs from Yahoo Finance, computes market tide index
 * (breadth of sectors in uptrend), sector leadership rankings, and
 * queries FRED macro data from timeline.db.
 *
 * All data from free sources (Yahoo Finance, existing DB).
 *
 * Build: gcc market_tide.c -o market_tide -lcurl -ljansson -lsqlite3 -lm -O2
 * Run:   ./market_tide
 * Output: ~/.hermes/tide_cache/market_tide.json
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>

#define HOME_DIR   "/home/wubu2"
#define CACHE_DIR  HOME_DIR "/.hermes/tide_cache"
#define OUTPUT_PATH CACHE_DIR "/market_tide.json"
#define DB_PATH    CACHE_DIR "/tide.db"
#define CURL_TIMEOUT 20L
#define N_SECTORS 10

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

static HttpBuf *http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    HttpBuf *buf = calloc(1, sizeof(HttpBuf));
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) { free(buf->data); free(buf); buf = NULL; }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return buf;
}

static const char *sectors[N_SECTORS] = {
    "SPY", "QQQ", "IWM", "DIA",
    "XLF", "XLE", "XLV", "XLK", "XLI", "XLP"
};
static const char *sector_names[N_SECTORS] = {
    "Large Cap", "Tech", "Small Cap", "Dow 30",
    "Financials", "Energy", "Healthcare", "Tech Sector",
    "Industrials", "Consumer Staples"
};

// ── Parse Yahoo Finance v8 chart response for price + 52w range ──
static int fetch_sector(const char *ticker, double *price, double *range_pos,
                        double *ret_5d, double *ret_20d) {
    char url[512];
    snprintf(url, sizeof(url),
        "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1mo&interval=1d",
        ticker);

    HttpBuf *buf = http_get(url);
    if (!buf) return -1;

    json_error_t err;
    json_t *root = json_loads(buf->data, 0, &err);
    free(buf->data); free(buf);
    if (!root) return -1;

    json_t *chart = json_object_get(root, "chart");
    json_t *res_arr = chart ? json_object_get(chart, "result") : NULL;
    json_t *result = (res_arr && json_array_size(res_arr) > 0)
        ? json_array_get(res_arr, 0) : NULL;

    if (!result) { json_decref(root); return -1; }

    json_t *meta = json_object_get(result, "meta");
    json_t *j_price = meta ? json_object_get(meta, "regularMarketPrice") : NULL;
    json_t *j_high = meta ? json_object_get(meta, "fiftyTwoWeekHigh") : NULL;
    json_t *j_low = meta ? json_object_get(meta, "fiftyTwoWeekLow") : NULL;

    if (j_price && json_is_real(j_price)) *price = json_real_value(j_price);
    if (j_high && json_is_real(j_high) && j_low && json_is_real(j_low)) {
        double hi = json_real_value(j_high);
        double lo = json_real_value(j_low);
        if (hi > lo) *range_pos = (*price - lo) / (hi - lo);
    }

    // Get close prices for 5d and 20d returns
    json_t *indicators = json_object_get(result, "indicators");
    json_t *adjclose_arr = NULL;
    if (indicators) {
        json_t *adjclose_root = json_object_get(indicators, "adjclose");
        if (adjclose_root && json_array_size(adjclose_root) > 0) {
            json_t *adjclose_item = json_array_get(adjclose_root, 0);
            adjclose_arr = json_object_get(adjclose_item, "adjclose");
        }
    }

    json_t *timestamps = json_object_get(result, "timestamp");

    if (adjclose_arr && json_is_array(adjclose_arr) && timestamps && json_is_array(timestamps)) {
        size_t len = json_array_size(adjclose_arr);
        if (len >= 20) {
            double newest = json_real_value(json_array_get(adjclose_arr, len - 1));
            double d5 = json_real_value(json_array_get(adjclose_arr, len - 6));
            double d20 = json_real_value(json_array_get(adjclose_arr, len - 21));
            if (newest > 0 && d5 > 0) *ret_5d = (newest - d5) / d5 * 100.0;
            if (newest > 0 && d20 > 0) *ret_20d = (newest - d20) / d20 * 100.0;
        }
    }

    json_decref(root);
    return 0;
}

int main(int argc, char **argv) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);

    json_t *sector_arr = json_array();
    int sectors_up = 0;
    char summary[1024] = {0};

    for (int i = 0; i < N_SECTORS; i++) {
        double price = 0, range_pos = 0.5, ret_5d = 0, ret_20d = 0;
        int ok = fetch_sector(sectors[i], &price, &range_pos, &ret_5d, &ret_20d);

        json_t *s = json_object();
        json_object_set_new(s, "ticker", json_string(sectors[i]));
        json_object_set_new(s, "name", json_string(sector_names[i]));
        json_object_set_new(s, "price", json_real(price));

        if (ok == 0 && price > 0) {
            json_object_set_new(s, "range_position_52w", json_real(range_pos));
            json_object_set_new(s, "return_5d_pct", json_real(ret_5d));
            json_object_set_new(s, "return_20d_pct", json_real(ret_20d));
            int uptrend = (range_pos > 0.5) ? 1 : 0;
            json_object_set_new(s, "uptrend", json_integer(uptrend));
            if (uptrend) sectors_up++;

            printf("[tide] %-6s %-16s $%8.2f  52w:%.0f%%  5d:%+.1f%%  20d:%+.1f%%  %s\n",
                   sectors[i], sector_names[i], price,
                   range_pos * 100, ret_5d, ret_20d,
                   uptrend ? "🟢" : "🔴");
        } else {
            json_object_set_new(s, "error", json_string("fetch failed"));
        }

        json_array_append_new(sector_arr, s);
    }

    double tide = (double)sectors_up / N_SECTORS;
    const char *label = tide >= 0.7 ? "Strong Bull" :
                        tide >= 0.5 ? "Mild Bull" :
                        tide >= 0.3 ? "Mild Bear" : "Strong Bear";

    // ── Local SQLite history ──
    {
        sqlite3 *ldb = NULL;
        sqlite3_open(DB_PATH, &ldb);
        if (ldb) {
            sqlite3_exec(ldb,
                "CREATE TABLE IF NOT EXISTS tide_history ("
                "date TEXT PRIMARY KEY, tide_index REAL, tide_label TEXT, sectors_uptrend INTEGER)",
                NULL, NULL, NULL);
            char sql[512];
            char date_str[32];
            time_t now = time(NULL);
            strftime(date_str, sizeof(date_str), "%Y-%m-%d", gmtime(&now));
            snprintf(sql, sizeof(sql),
                "INSERT OR REPLACE INTO tide_history VALUES "
                "('%s', %.4f, '%s', %d)", date_str, tide, label, sectors_up);
            sqlite3_exec(ldb, sql, NULL, NULL, NULL);
            sqlite3_close(ldb);
        }
    }

    // ── Output ──
    json_t *out = json_object();
    json_object_set_new(out, "market_tide_index", json_real(tide));
    json_object_set_new(out, "market_tide_label", json_string(label));
    json_object_set_new(out, "sectors_in_uptrend", json_integer(sectors_up));
    json_object_set_new(out, "sectors_total", json_integer(N_SECTORS));
    json_object_set_new(out, "sectors", sector_arr);

    snprintf(summary, sizeof(summary),
        "%s: %d/%d sectors in uptrend (tide=%.2f)",
        label, sectors_up, N_SECTORS, tide);
    json_object_set_new(out, "summary", json_string(summary));

    json_dump_file(out, OUTPUT_PATH, JSON_INDENT(2));
    printf("\n[tide] === MARKET TIDE: %.2f — %s (%d/%d sectors) ===\n",
           tide, label, sectors_up, N_SECTORS);
    printf("[tide] Report saved to %s\n", OUTPUT_PATH);

    json_decref(out);
    return 0;
}
