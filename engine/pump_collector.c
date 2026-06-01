/**
 * pump_collector.c — Read news_cache and write pump_score to news/runs/
 * Bridge between new news_rss.c output and feed_bridge.c pump_score reader.
 *
 * Build: gcc -O2 pump_collector.c -o pump_collector -ljansson -lm
 * Output: ~/.hermes/pm_logs/news/runs/news_<ts>.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <jansson.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#define HOME_DIR "/home/wubu2"
#define CACHE_FILE HOME_DIR "/.hermes/news_cache/news_features.json"
#define RUNS_DIR HOME_DIR "/.hermes/pm_logs/news/runs"

int main(void) {
    double pump_score = 0.0, sent_norm = 0.5, vol_norm = 0.5;
    int total = 100, pump_count = 48;

    /* Try reading news cache */
    FILE *f = fopen(CACHE_FILE, "r");
    if (f) {
        json_error_t err;
        json_t *root = json_loadf(f, 0, &err);
        fclose(f);
        if (root) {
            json_t *j_sent = json_object_get(root, "news_sentiment_norm");
            json_t *j_vol = json_object_get(root, "news_volume_norm");
            json_t *j_total = json_object_get(root, "total_articles");
            sent_norm = json_is_real(j_sent) ? json_real_value(j_sent) : 0.5;
            vol_norm = json_is_real(j_vol) ? json_real_value(j_vol) : 0.5;
            total = json_is_integer(j_total) ? json_integer_value(j_total) : 0;

            /* Map sent_norm [0,1] to pump_score [-1,1], scaled by volume confidence */
            pump_score = (sent_norm - 0.5) * 2.0;
            double vol_factor = 0.5 + vol_norm * 0.5; /* [0.5, 1.0] */
            pump_score *= vol_factor;
            if (pump_score > 1.0) pump_score = 1.0;
            if (pump_score < -1.0) pump_score = -1.0;
            pump_count = total > 0 ? (int)(total * (pump_score + 1.0) / 2.0) : 0;

            json_decref(root);
            fprintf(stderr, "[pump] cache: sent=%.4f vol=%.4f total=%d score=%.4f\n",
                    sent_norm, vol_norm, total, pump_score);
        } else {
            fprintf(stderr, "[pump] JSON parse error, using fallback\n");
        }
    } else {
        fprintf(stderr, "[pump] No cache file, using fallback\n");
    }

    /* Write news_<ts>.json */
    mkdir(RUNS_DIR, 0755);
    time_t now = time(NULL);
    char ts_str[32];
    strftime(ts_str, sizeof(ts_str), "%Y%m%d_%H%M%S", gmtime(&now));
    char path[512];
    snprintf(path, sizeof(path), "%s/news_%s.json", RUNS_DIR, ts_str);

    char json[1024];
    int n = snprintf(json, sizeof(json),
        "{\n"
        "  \"timestamp\": %ld,\n"
        "  \"total_articles\": %d,\n"
        "  \"pump_articles\": %d,\n"
        "  \"pump_score\": %.4f,\n"
        "  \"sentiment_norm\": %.4f,\n"
        "  \"volume_norm\": %.4f\n"
        "}\n",
        (long)now, total, pump_count,
        pump_score, sent_norm, vol_norm);

    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) { write(fd, json, n); close(fd); }

    printf("[pump] wrote %s score=%.4f articles=%d\n", path, pump_score, total);
    return 0;
}
