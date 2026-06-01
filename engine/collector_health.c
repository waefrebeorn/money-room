/*
 * collector_health.c — Cron health dashboard
 * Parses collector_runner.log, reports last run time per collector.
 * Output: JSON array with {name, last_run, status, age_minutes, stale}
 *
 * gcc -O2 -o collector_health collector_health.c -lcurl -ljansson -lsqlite3
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define MAX_COLLECTORS 64
#define MAX_NAME 64
#define MAX_LINE 1024

typedef struct {
    char name[MAX_NAME];
    char last_run[64];
    char status[16];    /* ok, timeout, fail, warn */
    int  age_minutes;
    int  stale;         /* >360 min = stale */
    int  found;
} Collector;

static Collector collectors[MAX_COLLECTORS];
static int n_collectors = 0;

static const char *KNOWN_COLLECTORS[] = {
    "cycle_all_rooms", "outlier_filter", "ws_feed_watchdog",
    "sports", "options_features", "onchain_features",
    "stablecoin_features", "funding_features", "open_interest",
    "hashrate_features", "orderbook_archive", "liquidation_features",
    "ls_ratio", "whale_tracking", "etf_flow",
    "gdelt_sentiment", "news_rss", "options_flow_monitor",
    "earnings_calendar", "dark_pool", "congress_trades",
    "insider_trades", "13f_holdings", "short_interest",
    "market_tide", "etf_holdings", "seasonality",
    "teacher_bridge", "param_tuner", "polymarket_scan",
    "auto_test", "bounty_scan",
    "economic", "weather", "kalshi", NULL
};

static int find_or_create(const char *name) {
    for (int i = 0; i < n_collectors; i++)
        if (strcmp(collectors[i].name, name) == 0)
            return i;
    if (n_collectors >= MAX_COLLECTORS) return -1;
    snprintf(collectors[n_collectors].name, MAX_NAME, "%s", name);
    collectors[n_collectors].found = 1;
    return n_collectors++;
}

/* Parse ISO-8601 timestamp like [2026-05-29T15:15:16-04:00] */
static time_t parse_timestamp(const char *ts) {
    struct tm tm = {0};
    int tz_h = 0, tz_m = 0;
    if (sscanf(ts, "[%d-%d-%dT%d:%d:%d", 
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = -1;
        /* Find timezone offset: it's after the seconds (after 3rd colon or last +/-) */
        const char *tz = strrchr(ts, '-');
        if (!tz || tz == ts) tz = strrchr(ts, '+');
        if (tz && strlen(tz) >= 6 && tz > ts + 25) {
            sscanf(tz + 1, "%d:%d", &tz_h, &tz_m);
            /* If the char before tz is '+' the offset is negative */
            if (tz > ts && tz[-1] == '+') { tz_h = -tz_h; tz_m = -tz_m; }
        }
        time_t utc = timegm(&tm);
        utc -= (tz_h * 3600 + tz_m * 60);
        return utc;
    }
    return 0;
}

/* Extract display-friendly timestamp (strip tz offset, keep date+time) */
static void fmt_timestamp(const char *src, char *dst, size_t dstlen) {
    /* src = "[2026-05-29T15:15:16-04:00]" */
    const char *ts_s = src;
    if (*ts_s == '[') ts_s++;
    /* Find the last +/- which is the timezone offset start */
    const char *tz = strrchr(ts_s, '-');
    if (!tz || tz == ts_s) tz = strrchr(ts_s, '+');
    if (tz && strlen(tz) >= 6 && tz > ts_s) {
        size_t len = (size_t)(tz - ts_s);
        if (len >= dstlen) len = dstlen - 1;
        memcpy(dst, ts_s, len);
        dst[len] = '\0';
    } else {
        /* No timezone, just copy until ']' or end */
        size_t len = 0;
        while (ts_s[len] && ts_s[len] != ']' && len < dstlen - 1) {
            dst[len] = ts_s[len];
            len++;
        }
        dst[len] = '\0';
    }
}

int main(int argc, char **argv) {
    const char *logpath = "/home/wubu2/.hermes/pm_logs/collector_runner.log";
    if (argc > 1) logpath = argv[1];

    /* Initialize known collectors */
    for (int i = 0; KNOWN_COLLECTORS[i]; i++) {
        snprintf(collectors[n_collectors].name, MAX_NAME, "%s", KNOWN_COLLECTORS[i]);
        strcpy(collectors[n_collectors].status, "unknown");
        collectors[n_collectors].found = 0;
        n_collectors++;
    }

    FILE *f = fopen(logpath, "r");
    if (!f) {
        printf("{\"error\": \"Cannot open %s\", \"collectors\": []}\n", logpath);
        return 1;
    }

    char line[MAX_LINE];
    time_t now = time(NULL);

    while (fgets(line, sizeof(line), f)) {
        /* Extract timestamp */
        if (line[0] != '[') continue;
        char *ts_end = strchr(line, ']');
        if (!ts_end) continue;
        size_t ts_len = ts_end - line + 1;
        char timestamp[64];
        snprintf(timestamp, ts_len < 64 ? ts_len : 63, "%s", line);
        time_t ts = parse_timestamp(timestamp);
        if (ts == 0) continue;

        /* Check for RUN: pattern */
        char *run = strstr(line, "  RUN: ");
        if (run) {
            char name[MAX_NAME] = {0};
            sscanf(run + 6, "%63s", name);
            if (name[0]) {
                int idx = find_or_create(name);
                if (idx >= 0) {
                    fmt_timestamp(timestamp, collectors[idx].last_run, 64);
                    collectors[idx].age_minutes = (int)((now - ts) / 60);
                    collectors[idx].stale = (collectors[idx].age_minutes > 360) ? 1 : 0;
                    strcpy(collectors[idx].status, "running");
                }
            }
            continue;
        }

        /* Check for OK: pattern */
        char *ok = strstr(line, "  OK: ");
        if (ok) {
            char name[MAX_NAME] = {0};
            sscanf(ok + 5, "%63s", name);
            if (name[0]) {
                int idx = find_or_create(name);
                if (idx >= 0) {
                    fmt_timestamp(timestamp, collectors[idx].last_run, 64);
                    collectors[idx].age_minutes = (int)((now - ts) / 60);
                    collectors[idx].stale = (collectors[idx].age_minutes > 360) ? 1 : 0;
                    strcpy(collectors[idx].status, "ok");
                }
            }
            continue;
        }

        /* Check for TIMEOUT: pattern */
        char *to = strstr(line, "  TIMEOUT: ");
        if (to) {
            char name[MAX_NAME] = {0};
            sscanf(to + 10, "%63s", name);
            if (name[0]) {
                int idx = find_or_create(name);
                if (idx >= 0) {
                    fmt_timestamp(timestamp, collectors[idx].last_run, 64);
                    collectors[idx].age_minutes = (int)((now - ts) / 60);
                    collectors[idx].stale = 1;
                    strcpy(collectors[idx].status, "timeout");
                }
            }
            continue;
        }

        /* Check for FAIL: pattern */
        char *fail = strstr(line, "  FAIL: ");
        if (fail) {
            char name[MAX_NAME] = {0};
            sscanf(fail + 7, "%63s", name);
            if (name[0]) {
                int idx = find_or_create(name);
                if (idx >= 0) {
                    fmt_timestamp(timestamp, collectors[idx].last_run, 64);
                    collectors[idx].age_minutes = (int)((now - ts) / 60);
                    collectors[idx].stale = 1;
                    strcpy(collectors[idx].status, "fail");
                }
            }
            continue;
        }

        /* Check for WARN: pattern */
        char *warn = strstr(line, "  WARN: ");
        if (warn) {
            char name[MAX_NAME] = {0};
            sscanf(warn + 7, "%63s", name);
            if (name[0]) {
                int idx = find_or_create(name);
                if (idx >= 0) {
                    fmt_timestamp(timestamp, collectors[idx].last_run, 64);
                    collectors[idx].age_minutes = (int)((now - ts) / 60);
                    collectors[idx].stale = 1;
                    strcpy(collectors[idx].status, "warn");
                }
            }
        }
    }
    fclose(f);

    /* Check wrapper scripts exist */
    struct stat st;
    for (int i = 0; i < n_collectors; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/home/wubu2/.hermes/scripts/%s.sh", collectors[i].name);
        /* Check if sh wrapper exists (some use _feat.sh suffix, _fetch.sh, etc.) */
        if (stat(path, &st) != 0) {
            /* Try alternate paths */
            snprintf(path, sizeof(path), "/home/wubu2/.hermes/scripts/%s", collectors[i].name);
            if (stat(path, &st) != 0) {
                /* Not found — check if collector marks as missing */
                if (collectors[i].found == 0 && strcmp(collectors[i].status, "unknown") == 0) {
                    strcpy(collectors[i].status, "missing");
                }
            }
        }
    }

    /* Output JSON */
    printf("{\n");
    printf("  \"generated_at\": %ld,\n", (long)now);
    printf("  \"collectors\": [\n");
    int first = 1;
    for (int i = 0; i < n_collectors; i++) {
        if (!first) printf(",\n");
        first = 0;
        printf("    {\"name\": \"%s\", \"status\": \"%s\", \"last_run\": \"%s\", \"age_min\": %d, \"stale\": %d}",
               collectors[i].name, collectors[i].status,
               collectors[i].last_run[0] ? collectors[i].last_run : "never",
               collectors[i].age_minutes, collectors[i].stale);
    }
    printf("\n  ]\n");
    printf("}\n");

    return 0;
}
