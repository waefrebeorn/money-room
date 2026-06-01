/*
 * earnings_cal.c — P32: Earnings Calendar Features
 *
 * Maintains hardcoded earnings calendar for SPY top 10+ holdings
 * (by weight). Computes event proximity and density features.
 *
 * Output: JSON → feed_bridge → engine features.
 *
 * Build: gcc -O3 -march=native earnings_cal.c -o earnings_cal -ljansson -lm
 * Cron: hermes cron create --daily-at 0 "./earnings_cal"
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <jansson.h>

#define HOME_DIR    "/home/wubu2"
#define OUT_FILE    HOME_DIR "/.hermes/options_cache/earnings_features.json"

// ── Earnings date entry ──
typedef struct { const char *ticker; const char *next_earn_date; double weight; } EarningsEntry;

// SPY top 10+ holdings by approximate weight (May 2026)
// Each entry: next expected earnings date (quarterly, ~3mo apart)
static EarningsEntry EARNINGS[] = {
    {"AAPL", "2026-07-23", 0.069},
    {"MSFT", "2026-07-21", 0.065},
    {"NVDA", "2026-08-20", 0.059},
    {"AMZN", "2026-07-30", 0.039},
    {"META", "2026-07-29", 0.025},
    {"GOOGL","2026-07-23", 0.020},
    {"GOOG", "2026-07-23", 0.017},
    {"BRK.B","2026-08-01", 0.017},
    {"JPM",  "2026-07-14", 0.015},
    {"LLY",  "2026-08-04", 0.015},
    {"TSLA", "2026-07-15", 0.014},
    {"V",    "2026-07-21", 0.014},
    {"XOM",  "2026-08-01", 0.013},
    {"UNH",  "2026-07-15", 0.013},
    {"AVGO", "2026-08-27", 0.013},
    {"MA",   "2026-07-22", 0.012},
    {"HD",   "2026-08-18", 0.011},
    {"PG",   "2026-07-28", 0.011},
    {"COST", "2026-07-09", 0.010},
    {"JNJ",  "2026-07-14", 0.010},
    {"ABBV", "2026-07-23", 0.010},
    {"WMT",  "2026-08-14", 0.010},
    {"CVX",  "2026-08-01", 0.009},
    {"MRK",  "2026-07-30", 0.009},
    {"CRM",  "2026-08-20", 0.009},
    {"ORCL", "2026-09-10", 0.009},
    {"AMD",  "2026-07-28", 0.008},
    {"NFLX", "2026-07-16", 0.008},
    {"KO",   "2026-07-21", 0.008},
    {"PEP",  "2026-07-09", 0.008},
    {NULL, NULL, 0}
};

// Parse date string "YYYY-MM-DD" to days from now
static int days_until(const char *date_str) {
    int y, m, d;
    if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3) return -1;
    
    time_t now = time(NULL);
    struct tm now_tm = *gmtime(&now);
    
    struct tm earn_tm = {0};
    earn_tm.tm_year = y - 1900;
    earn_tm.tm_mon = m - 1;
    earn_tm.tm_mday = d;
    earn_tm.tm_hour = 16; // After market close
    time_t earn_ts = timegm(&earn_tm);
    
    double secs = difftime(earn_ts, now);
    int days = (int)(secs / 86400.0 + 0.5);
    return days;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    int n = 0;
    while (EARNINGS[n].ticker) n++;
    
    double total_weight = 0;
    double weighted_days = 0;
    double weighted_score = 0;  // Higher = more earnings activity
    int reports_this_week = 0;
    int reports_this_month = 0;
    double density_score = 0;  // 0-1, how dense is earnings season
    
    char closest_ticker[16] = "";
    int closest_days = 999;
    double closest_weight = 0;
    
    char next_date[16] = "";
    
    for (int i = 0; i < n; i++) {
        int days = days_until(EARNINGS[i].next_earn_date);
        if (days < 0) continue;
        
        total_weight += EARNINGS[i].weight;
        weighted_days += days * EARNINGS[i].weight;
        
        // Reports this week (≤7 days)
        if (days <= 7) reports_this_week++;
        // Reports this month (≤30 days)
        if (days <= 30) reports_this_month++;
        
        // Closest report
        if (days < closest_days) {
            closest_days = days;
            closest_weight = EARNINGS[i].weight;
            strncpy(closest_ticker, EARNINGS[i].ticker, 15);
            strncpy(next_date, EARNINGS[i].next_earn_date, 15);
        }
        
        // Score: inverse-distance weighted by weight, scaled 0-1
        weighted_score += EARNINGS[i].weight / (days + 1.0);
    }
    
    // Normalize: typical max for 30 stocks within 90 days
    double days_to_next_earn = (double)closest_days;
    double earn_density = (double)reports_this_week / 10.0;
    if (earn_density > 1.0) earn_density = 1.0;
    
    double earn_activity = weighted_score / 5.0;  // Normalize, cap at 1
    if (earn_activity > 1.0) earn_activity = 1.0;
    
    // Build output JSON
    json_t *root = json_object();
    json_object_set_new(root, "days_to_next_earnings", json_real(days_to_next_earn));
    json_object_set_new(root, "closest_earnings_ticker", json_string(closest_ticker));
    json_object_set_new(root, "next_earnings_date", json_string(next_date));
    json_object_set_new(root, "closest_weight", json_real(closest_weight));
    json_object_set_new(root, "earnings_this_week", json_integer(reports_this_week));
    json_object_set_new(root, "earnings_this_month", json_integer(reports_this_month));
    json_object_set_new(root, "earn_density", json_real(earn_density));
    json_object_set_new(root, "earn_activity", json_real(earn_activity));
    json_object_set_new(root, "total_tracked", json_integer(n));
    json_object_set_new(root, "source", json_string("earnings_cal"));
    
    FILE *f = fopen(OUT_FILE, "w");
    if (!f) { fprintf(stderr, "Can't write %s\n", OUT_FILE); json_decref(root); return 1; }
    json_dumpf(root, f, JSON_INDENT(2) | JSON_SORT_KEYS);
    fclose(f);
    json_decref(root);
    
    printf("Wrote %s\n", OUT_FILE);
    printf("  next: %s (%s, %.1f days, w=%.1f%%)\n", closest_ticker, next_date, days_to_next_earn, closest_weight * 100);
    printf("  this_week=%d this_month=%d density=%.2f activity=%.2f\n",
           reports_this_week, reports_this_month, earn_density, earn_activity);
    
    return 0;
}
