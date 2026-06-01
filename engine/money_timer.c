/**
 * money_timer.c — Daily financial report (replaces money_timer.py)
 * Checks NOWPayments, paper PnL, timeline size, open bounties.
 * Build: gcc -O2 money_timer.c -o money_timer -lcurl -ljansson -lsqlite3 -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>

#define HOME "/home/wubu2"
#define DEADLINE_Y 2026
#define DEADLINE_M 8
#define DEADLINE_D 21
#define START_Y 2026
#define START_M 5
#define START_D 25
#define STIPEND_MONTHLY 1760.0

static const char *TIER_NAMES[] = {"survival","comfort","cluster","empire","infinity"};
static const double TIER_AMOUNTS[] = {1760, 5000, 20000, 200000, 1000000};

struct MemBuf { char *data; size_t size; };
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct MemBuf *mb = (struct MemBuf *)userp;
    char *np = realloc(mb->data, mb->size + total + 1);
    if (!np) return 0;
    mb->data = np;
    memcpy(mb->data + mb->size, ptr, total);
    mb->size += total;
    mb->data[mb->size] = 0;
    return total;
}

static char *http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    struct MemBuf mb = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "money-timer/1.0");
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) { free(mb.data); return NULL; }
    return mb.data;
}

static double get_nowpayments(void) {
    char *raw = http_get("https://api.nowpayments.io/v1/status");
    if (!raw) return 0.0;
    json_error_t err;
    json_t *root = json_loads(raw, 0, &err); free(raw);
    if (!root) return 0.0;
    // NOWPayments status endpoint doesn't have payment data
    // This is a placeholder — real data needs API key from vault
    json_decref(root);
    return 0.0;
}

static double get_paper_pnl(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.hermes/pm_logs/eco/portfolios.json", HOME);
    FILE *f = fopen(path, "r");
    if (!f) return 0.0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *raw = malloc(sz + 1); if (!raw) { fclose(f); return 0.0; }
    size_t n = fread(raw, 1, sz, f); raw[n] = 0; fclose(f);
    json_error_t err;
    json_t *pf = json_loads(raw, 0, &err); free(raw);
    if (!pf || !json_is_object(pf)) { json_decref(pf); return 0.0; }
    double total_pnl = 0;
    const char *k; json_t *v;
    json_object_foreach(pf, k, v) {
        json_t *p = json_object_get(v, "total_pnl");
        if (p) total_pnl += json_number_value(p);
    }
    json_decref(pf);
    return total_pnl;
}

static double get_timeline_gb(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.hermes/pm_logs/timeline.db", HOME);
    FILE *f = fopen(path, "r");
    if (!f) return 0.0;
    fseek(f, 0, SEEK_END);
    double sz = ftell(f) / 1e9;
    fclose(f);
    return sz;
}

static int get_open_bounties(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.hermes/bounty_scanner/bounties.db", HOME);
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM bounties WHERE status='new'", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return count;
}

int main(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    struct tm deadline = {0,0,0,DEADLINE_D,DEADLINE_M-1,DEADLINE_Y-1900};
    struct tm start = {0,0,0,START_D,START_M-1,START_Y-1900};
    double days_left = difftime(mktime(&deadline), now) / 86400.0;
    double days_elapsed = difftime(now, mktime(&start)) / 86400.0;
    int week = (int)(days_elapsed / 7.0) + 1;
    if (week < 1) week = 1;

    double payments = get_nowpayments();
    double paper_pnl = get_paper_pnl();
    double timeline_gb = get_timeline_gb();
    int bounties = get_open_bounties();
    double total = payments;

    printf("=== MONEY TIMER ===\n");
    char tb[32]; strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", tm);
    printf("%s\n", tb);
    printf("Week %d | %.0f days until token expires\n\n", week, days_left < 0 ? 0 : days_left);
    printf("  Paid revenue:     $%.2f\n", payments);
    printf("  Paper PnL:        $%.2f\n", paper_pnl);
    printf("  Timeline:         %.2f GB\n", timeline_gb);
    printf("  Open bounties:    %d\n\n", bounties);

    for (int i = 0; i < 5; i++) {
        int fill = (int)fmin(total / TIER_AMOUNTS[i] * 30.0, 30.0);
        double pct = total / TIER_AMOUNTS[i] * 100.0;
        printf("  %10s: $%6.0f/mo  [", TIER_NAMES[i], TIER_AMOUNTS[i]);
        for (int b = 0; b < fill; b++) putchar('#');
        for (int b = fill; b < 30; b++) putchar('.');
        printf("] %.1f%%\n", pct);
    }
    printf("\nNeed $%.0f/day for survival, $%.0f/day for comfort\n",
           1760.0/30.0, 5000.0/30.0);
    printf("=== TIME IS MONEY ===\n");
    return 0;
}
