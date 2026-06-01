/*
 * outlier_filter.c — E60: Outlier detection on market inputs
 *
 * Reads market_feed.json before the engine uses it.
 * Checks for: price spikes, price gaps, flatlines, stale data.
 * Writes outlier flags and filtered values back to market_feed.json.
 *
 * Build: gcc -O3 -march=native outlier_filter.c -o outlier_filter -lm -ljansson
 * Usage: ./outlier_filter [market_feed_path]
 *   Default: /home/wubu2/.hermes/pm_logs/c_room/market_feed.json
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <jansson.h>

#define DEFAULT_FEED "/home/wubu2/.hermes/pm_logs/c_room/market_feed.json"
#define HISTORY_FILE "/home/wubu2/.hermes/eco_cache/outlier_history.json"
#define MAX_SPIKE_PCT 5.0   /* 5% price change in one tick = spike */
#define MAX_GAP_PCT 10.0    /* 10% gap from last known = major gap */
#define STALE_SECONDS 300   /* 5 minutes stale = stale data */
#define FLATLINE_TICKS 5    /* Same price for 5 ticks = flatline */

/* ─── Read JSON file ─── */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ─── Load price history ─── */
static int load_history(double *history, int max, double *last_ts) {
    char *json_str = read_file(HISTORY_FILE);
    if (!json_str) {
        *last_ts = 0;
        return 0;
    }

    json_t *root = json_loads(json_str, 0, NULL);
    free(json_str);
    if (!root || !json_is_object(root)) {
        if (root) json_decref(root);
        *last_ts = 0;
        return 0;
    }

    *last_ts = json_real_value(json_object_get(root, "last_ts"));
    json_t *arr = json_object_get(root, "prices");
    int count = 0;
    if (json_is_array(arr)) {
        count = json_array_size(arr);
        if (count > max) count = max;
        for (int i = 0; i < count; i++) {
            history[i] = json_real_value(json_array_get(arr, i));
        }
    }

    json_decref(root);
    return count;
}

/* ─── Save price history ─── */
static void save_history(double *history, int count, double last_ts) {
    /* Keep last 10 prices */
    int keep = count > 10 ? 10 : count;
    int start = count > 10 ? count - 10 : 0;

    json_t *arr = json_array();
    for (int i = start; i < count; i++) {
        json_array_append_new(arr, json_real(history[i]));
    }

    json_t *root = json_object();
    json_object_set_new(root, "prices", arr);
    json_object_set_new(root, "last_ts", json_real(last_ts));

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p /home/wubu2/.hermes/eco_cache");
    system(cmd);

    char *out = json_dumps(root, JSON_INDENT(2));
    FILE *f = fopen(HISTORY_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", out);
        fclose(f);
    }
    free(out);
    json_decref(root);
}

int main(int argc, char **argv) {
    const char *feed_path = argc > 1 ? argv[1] : DEFAULT_FEED;

    /* Read current market feed */
    char *json_str = read_file(feed_path);
    if (!json_str) {
        fprintf(stderr, "[outlier] Cannot read %s\n", feed_path);
        return 1;
    }

    json_t *feed = json_loads(json_str, 0, NULL);
    free(json_str);
    if (!feed) {
        fprintf(stderr, "[outlier] Invalid JSON in %s\n", feed_path);
        return 1;
    }

    /* Get current BTC price from known fields */
    const char *price_fields[] = {"cb_price", "close", "ok_price", "btc_price",
                                  "price", "last", "cb_last", NULL};
    double current_price = 0;

    for (int i = 0; price_fields[i]; i++) {
        json_t *j = json_object_get(feed, price_fields[i]);
        if (j) {
            double val = json_real_value(j);
            if (val > 1000 && val < 1000000) {
                current_price = val;
                break;
            }
        }
    }

    time_t now = time(NULL);
    double last_ts = 0;
    double history[20] = {0};
    int hist_count = load_history(history, 20, &last_ts);

    /* ─── Outlier checks ─── */
    int outlier_flag = 0;
    char warnings[512] = "";
    int warn_len = 0;

    if (current_price <= 0) {
        warn_len += snprintf(warnings + warn_len, sizeof(warnings) - warn_len,
                             "zero_price ");
        outlier_flag = 1;
    }

    /* Spike check */
    if (current_price > 0 && hist_count > 0) {
        double last_price = history[hist_count - 1];
        if (last_price > 0) {
            double pct_change = fabs((current_price - last_price) / last_price) * 100;
            if (pct_change > MAX_SPIKE_PCT) {
                warn_len += snprintf(warnings + warn_len, sizeof(warnings) - warn_len,
                                     "spike_%.1f%% ", pct_change);
                outlier_flag = 1;

                /* Clamp to last price */
                json_object_set_new(feed, "btc_price_filtered", json_real(last_price));
            }
            if (pct_change > MAX_GAP_PCT) {
                warn_len += snprintf(warnings + warn_len, sizeof(warnings) - warn_len,
                                     "gap_%.1f%% ", pct_change);
                outlier_flag = 2;
            }
        }
    }

    /* Flatline check */
    if (current_price > 0 && hist_count >= FLATLINE_TICKS) {
        int flat = 1;
        for (int i = hist_count - FLATLINE_TICKS + 1; i < hist_count; i++) {
            if (fabs(history[i] - current_price) / fmax(current_price, 0.01) > 0.0001) {
                flat = 0;
                break;
            }
        }
        if (flat && hist_count >= FLATLINE_TICKS) {
            warn_len += snprintf(warnings + warn_len, sizeof(warnings) - warn_len,
                                 "flatline ");
            outlier_flag = 1;
        }
    }

    /* Staleness check */
    double feed_ts = 0;
    json_t *j_ts = json_object_get(feed, "timestamp");
    if (j_ts) feed_ts = json_real_value(j_ts);
    if (feed_ts > 0 && (now - feed_ts) > STALE_SECONDS) {
        warn_len += snprintf(warnings + warn_len, sizeof(warnings) - warn_len,
                             "stale_%ds ", (int)(now - feed_ts));
        outlier_flag = 1;
    }

    /* ─── Write outlier results back to feed ─── */
    json_object_set_new(feed, "outlier_flag", json_integer(outlier_flag));
    json_object_set_new(feed, "outlier_warnings",
                        warn_len > 0 ? json_string(warnings) : json_string("none"));
    json_object_set_new(feed, "outlier_checked_at", json_integer((long long)now));
    json_object_set_new(feed, "outlier_price", json_real(current_price));

    char *out = json_dumps(feed, JSON_INDENT(2));
    FILE *f = fopen(feed_path, "w");
    if (f) {
        fprintf(f, "%s\n", out);
        fclose(f);
    }

    /* ─── Update history ─── */
    history[hist_count] = current_price;
    save_history(history, hist_count + 1, (double)now);

    /* ─── Report ─── */
    if (outlier_flag) {
        printf("[outlier] ⚠️  FLAGGED: %s (price=%.2f)\n", warnings, current_price);
    } else {
        printf("[outlier] ✅ OK (price=%.2f, hist=%d)\n", current_price, hist_count + 1);
    }

    free(out);
    json_decref(feed);
    return outlier_flag ? 1 : 0;
}
