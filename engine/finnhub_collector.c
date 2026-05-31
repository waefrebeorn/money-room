/*
 * finnhub_collector.c — Unified Finnhub API collector
 * Supports: IPO calendar, economic calendar, SEC filings, stock data
 * Usage: ./finnhub_collector [endpoint] [args...]
 *
 * Endpoints:
 *   ipo       — IPO calendar (default: 90-day window)
 *   economic  — Economic calendar (default: 90-day window)
 *   filings   — SEC filings (symbol required)
 *   quote     — Stock quote by symbol
 *   news      — Company news by symbol
 *
 * Compile: gcc -O3 -o finnhub_collector finnhub_collector.c -lcurl -ljansson -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>

#define OUTPUT_DIR "/home/wubu2/money-room/data"

/* Buffer for API response */
struct mem_buf {
    char *data;
    size_t len;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    size_t total = size * nmemb;
    struct mem_buf *buf = (struct mem_buf *)user;
    char *newp = realloc(buf->data, buf->len + total + 1);
    if (!newp) return 0;
    buf->data = newp;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *fetch_json(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct mem_buf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "finnhub-collector/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[FINNHUB] curl error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

static int write_output(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "%s\n", data ? data : "null");
    fclose(f);
    return 0;
}

static void ipo_calendar(void) {
    const char *key = getenv("FINNHUB_API_KEY");
    if (!key) { fprintf(stderr, "[FINNHUB] FINNHUB_API_KEY not set\n"); return; }

    time_t now = time(NULL);
    char from[16], to[16];
    struct tm *t = gmtime(&now);
    strftime(from, sizeof(from), "%Y-%m-%d", t);
    t->tm_mday += 90; mktime(t);
    strftime(to, sizeof(to), "%Y-%m-%d", t);

    char url[512];
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/calendar/ipo?from=%s&to=%s&token=%s",
        from, to, key);

    char *json = fetch_json(url);
    if (!json) { fprintf(stderr, "[FINNHUB] IPO: no data\n"); return; }

    char path[256];
    snprintf(path, sizeof(path), "%s/ipo_calendar.json", OUTPUT_DIR);
    write_output(path, json);
    printf("[FINNHUB] IPO calendar -> %s (%zu bytes)\n", path, strlen(json));
    free(json);
}

static void economic_calendar(void) {
    const char *key = getenv("FINNHUB_API_KEY");
    if (!key) { fprintf(stderr, "[FINNHUB] FINNHUB_API_KEY not set\n"); return; }

    time_t now = time(NULL);
    char from[16], to[16];
    struct tm *t = gmtime(&now);
    strftime(from, sizeof(from), "%Y-%m-%d", t);
    t->tm_mday += 90; mktime(t);
    strftime(to, sizeof(to), "%Y-%m-%d", t);

    char url[512];
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/calendar/economic?from=%s&to=%s&token=%s",
        from, to, key);

    char *json = fetch_json(url);
    if (!json) { fprintf(stderr, "[FINNHUB] Economic: no data\n"); return; }

    char path[256];
    snprintf(path, sizeof(path), "%s/economic_calendar.json", OUTPUT_DIR);
    write_output(path, json);
    printf("[FINNHUB] Economic calendar -> %s (%zu bytes)\n", path, strlen(json));
    free(json);
}

static void stock_quote(const char *symbol) {
    const char *key = getenv("FINNHUB_API_KEY");
    if (!key) { fprintf(stderr, "[FINNHUB] FINNHUB_API_KEY not set\n"); return; }

    char url[512];
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/quote?symbol=%s&token=%s", symbol, key);

    char *json = fetch_json(url);
    if (!json) { fprintf(stderr, "[FINNHUB] Quote %s: no data\n", symbol); return; }

    char path[256];
    snprintf(path, sizeof(path), "%s/quote_%s.json", OUTPUT_DIR, symbol);
    write_output(path, json);
    printf("[FINNHUB] Quote %s -> %s (%zu bytes)\n", symbol, path, strlen(json));
    free(json);
}

static void sec_filings(const char *symbol) {
    const char *key = getenv("FINNHUB_API_KEY");
    if (!key) { fprintf(stderr, "[FINNHUB] FINNHUB_API_KEY not set\n"); return; }

    char url[512];
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/stock/filings?symbol=%s&token=%s", symbol, key);

    char *json = fetch_json(url);
    if (!json) { fprintf(stderr, "[FINNHUB] Filings %s: no data\n", symbol); return; }

    char path[256];
    snprintf(path, sizeof(path), "%s/filings_%s.json", OUTPUT_DIR, symbol);
    write_output(path, json);
    printf("[FINNHUB] Filings %s -> %s (%zu bytes)\n", symbol, path, strlen(json));
    free(json);
}

static void technical_indicators(const char *symbol, const char *indicator) {
    const char *key = getenv("FINNHUB_API_KEY");
    if (!key) { fprintf(stderr, "[FINNHUB] FINNHUB_API_KEY not set\n"); return; }

    char url[512];
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/indicator?symbol=%s&resolution=D&indicator=%s&token=%s",
        symbol, indicator, key);

    char *json = fetch_json(url);
    if (!json) { fprintf(stderr, "[FINNHUB] Indicator %s(%s): no data\n", symbol, indicator); return; }

    char path[256];
    snprintf(path, sizeof(path), "%s/indicator_%s_%s.json", OUTPUT_DIR, symbol, indicator);
    write_output(path, json);
    printf("[FINNHUB] Indicator %s/%s -> %s (%zu bytes)\n", symbol, indicator, path, strlen(json));
    free(json);
}

static void etf_holdings(const char *symbol) {
    const char *key = getenv("FINNHUB_API_KEY");
    if (!key) { fprintf(stderr, "[FINNHUB] FINNHUB_API_KEY not set\n"); return; }

    char url[512];
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/etf/holdings?symbol=%s&token=%s", symbol, key);

    char *json = fetch_json(url);
    if (!json) { fprintf(stderr, "[FINNHUB] ETF holdings %s: no data\n", symbol); return; }

    char path[256];
    snprintf(path, sizeof(path), "%s/etf_holdings_%s.json", OUTPUT_DIR, symbol);
    write_output(path, json);
    printf("[FINNHUB] ETF holdings %s -> %s (%zu bytes)\n", symbol, path, strlen(json));
    free(json);
}

static void mutual_fund_holdings(const char *symbol) {
    const char *key = getenv("FINNHUB_API_KEY");
    if (!key) { fprintf(stderr, "[FINNHUB] FINNHUB_API_KEY not set\n"); return; }

    char url[512];
    snprintf(url, sizeof(url),
        "https://finnhub.io/api/v1/mutual-fund/holdings?symbol=%s&token=%s", symbol, key);

    char *json = fetch_json(url);
    if (!json) { fprintf(stderr, "[FINNHUB] Mutual fund %s: no data\n", symbol); return; }

    char path[256];
    snprintf(path, sizeof(path), "%s/mutual_fund_%s.json", OUTPUT_DIR, symbol);
    write_output(path, json);
    printf("[FINNHUB] Mutual fund %s -> %s (%zu bytes)\n", symbol, path, strlen(json));
    free(json);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <endpoint> [args]\n", prog);
    fprintf(stderr, "Endpoints:\n");
    fprintf(stderr, "  ipo                  IPO calendar (90-day window)\n");
    fprintf(stderr, "  economic             Economic calendar (90-day window)\n");
    fprintf(stderr, "  quote <symbol>       Stock quote\n");
    fprintf(stderr, "  filings <symbol>     SEC filings\n");
    fprintf(stderr, "  indicator <sym> <f>  Technical indicator (sma, ema, rsi, macd, bb)\n");
    fprintf(stderr, "  etf <symbol>         ETF holdings\n");
    fprintf(stderr, "  mutual <symbol>      Mutual fund holdings\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char *endpoint = argv[1];

    if (strcmp(endpoint, "ipo") == 0) {
        ipo_calendar();
    } else if (strcmp(endpoint, "economic") == 0) {
        economic_calendar();
    } else if (strcmp(endpoint, "quote") == 0) {
        if (argc < 3) { fprintf(stderr, "Missing symbol\n"); return 1; }
        stock_quote(argv[2]);
    } else if (strcmp(endpoint, "filings") == 0) {
        if (argc < 3) { fprintf(stderr, "Missing symbol\n"); return 1; }
        sec_filings(argv[2]);
    } else if (strcmp(endpoint, "indicator") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s indicator <symbol> <indicator>\n", argv[0]); return 1; }
        technical_indicators(argv[2], argv[3]);
    } else if (strcmp(endpoint, "etf") == 0) {
        if (argc < 3) { fprintf(stderr, "Missing symbol\n"); return 1; }
        etf_holdings(argv[2]);
    } else if (strcmp(endpoint, "mutual") == 0) {
        if (argc < 3) { fprintf(stderr, "Missing symbol\n"); return 1; }
        mutual_fund_holdings(argv[2]);
    } else {
        fprintf(stderr, "Unknown endpoint: %s\n", endpoint);
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
