/**
 * hourly_ticker.c — T57: Hourly engine digest for Telegram
 *
 * Produces a compact one-line summary of engine + market state.
 * This is the ONE cron that texts the user (delivered via Telegram).
 *
 * Build: gcc -O2 -o hourly_ticker hourly_ticker.c -lcurl -ljansson
 * Usage: ./hourly_ticker
 * Output: plain text to stdout (Telegram-ready)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>

#define STATS_PATH "/home/wubu2/money-room/docs/data/stats.json"
#define PRICES_PATH "/home/wubu2/money-room/docs/data/prices.json"

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
    size_t total = size * nmemb;
    FILE *fp = (FILE*)stream;
    return fwrite(ptr, 1, total, fp);
}

static char *fetch_url(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    char *data = malloc(65536);
    if (!data) { curl_easy_cleanup(curl); return NULL; }
    data[0] = '\0';

    FILE *fp = fmemopen(data, 65536, "w");
    if (!fp) { free(data); curl_easy_cleanup(curl); return NULL; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "money-room-ticker/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { free(data); return NULL; }
    return data;
}

static double json_get_num(const json_t *o, const char *key) {
    json_t *v = json_object_get(o, key);
    return v ? json_number_value(v) : 0;
}

int main(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%a %b %d %I:%M %p", tm);

    /* ── Read local stats.json ── */
    FILE *f = fopen(STATS_PATH, "r");
    double cycle = 0, win_rate = 0, capital = 0, agents = 0;
    double trades = 0, sharpe = 0, drawdown = 0;

    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 10 && len < 1048576) {
            char *buf = malloc(len + 1);
            if (buf) {
                fread(buf, 1, len, f);
                buf[len] = '\0';
                json_error_t err;
                json_t *root = json_loads(buf, 0, &err);
                if (root) {
                    cycle = json_get_num(root, "cycle");
                    win_rate = json_get_num(root, "win_rate");
                    capital = json_get_num(root, "capital");
                    agents = json_get_num(root, "agents");
                    trades = json_get_num(root, "trades_total");
                    sharpe = json_get_num(root, "sharpe");
                    drawdown = json_get_num(root, "drawdown");
                    json_decref(root);
                }
                free(buf);
            }
        }
        fclose(f);
    }

    /* ── Fetch CoinGecko prices ── */
    double btc = 0, btc_chg = 0, eth = 0, eth_chg = 0, sol = 0, sol_chg = 0;
    char *cg_data = fetch_url(
        "https://api.coingecko.com/api/v3/simple/price"
        "?ids=bitcoin,ethereum,solana&vs_currencies=usd&include_24hr_change=true");
    if (cg_data) {
        json_error_t err;
        json_t *root = json_loads(cg_data, 0, &err);
        if (root) {
            json_t *b = json_object_get(root, "bitcoin");
            if (b) { btc = json_get_num(b, "usd"); btc_chg = json_get_num(b, "usd_24h_change"); }
            json_t *e = json_object_get(root, "ethereum");
            if (e) { eth = json_get_num(e, "usd"); eth_chg = json_get_num(e, "usd_24h_change"); }
            json_t *s = json_object_get(root, "solana");
            if (s) { sol = json_get_num(s, "usd"); sol_chg = json_get_num(s, "usd_24h_change"); }
            json_decref(root);
        }
        free(cg_data);
    }

    /* ── Build output ── */
    printf("⚡ Money Room — %s\n", timebuf);
    printf("\n");
    printf("🧬 Engine: %.0f cycles · %.1f%% WR · $%.0f capital\n", cycle, win_rate*100, capital);
    printf("👥 Agents: %.0f active · %.0f trades · %.2f sharpe\n", agents, trades, sharpe);
    if (drawdown > 0) printf("📉 Drawdown: %.1f%%\n", drawdown*100);

    printf("\n");
    printf("💲 BTC: $%.0f", btc);
    if (btc_chg != 0) printf(" (%+.2f%%)", btc_chg);
    printf("\n");
    printf("💲 ETH: $%.0f", eth);
    if (eth_chg != 0) printf(" (%+.2f%%)", eth_chg);
    printf("\n");
    printf("💲 SOL: $%.2f", sol);
    if (sol_chg != 0) printf(" (%+.2f%%)", sol_chg);

    printf("\n");
    printf("🔗 https://waefrebeorn.github.io/money-room/\n");

    return 0;
}
