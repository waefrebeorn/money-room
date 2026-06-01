/**
 * evolution_progress.c — Real-time evolution metrics
 * 
 * Reads paper training logs and engine state to produce
 * evolution progress JSON for the website dashboard.
 * 
 * Compile: gcc -O2 -o evolution_progress evolution_progress.c -lm
 * Usage:   ./evolution_progress
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>

#define LOG_DIR     "/home/wubu2/.hermes/pm_logs/paper_training"
#define STATE_DIR   "/home/wubu2/.hermes/pm_logs/c_room"
#define OUT_DIR     "/home/wubu2/money-room/data"

// Simple JSON writer
typedef struct {
    char *buf;
    size_t len, cap;
} json_buf_t;

static void jb_init(json_buf_t *jb) {
    jb->len = 0; jb->cap = 65536;
    jb->buf = malloc(jb->cap);
    jb->buf[0] = '\0';
}

static void jb_put(json_buf_t *jb, const char *s) {
    size_t sl = strlen(s);
    if (jb->len + sl + 1 > jb->cap) {
        jb->cap = jb->len + sl + 65536;
        jb->buf = realloc(jb->buf, jb->cap);
    }
    memcpy(jb->buf + jb->len, s, sl);
    jb->len += sl;
    jb->buf[jb->len] = '\0';
}

static void jb_printf(json_buf_t *jb, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) jb_put(jb, tmp);
}

// ── Read latest paper training log ──
static int scan_training_log(char *cycle_buf, size_t sz, int *cycle, int *total_candles) {
    DIR *d = opendir(LOG_DIR);
    if (!d) return -1;
    
    struct dirent *e;
    char latest[1024] = {0};
    time_t latest_mtime = 0;
    
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, "paper_run_") || strstr(e->d_name, "paper_train_")) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", LOG_DIR, e->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && st.st_mtime > latest_mtime) {
                latest_mtime = st.st_mtime;
                strncpy(latest, path, sizeof(latest) - 1);
            }
        }
    }
    closedir(d);
    
    if (!latest[0]) return -1;
    
    // Read last 100 lines for cycle info
    FILE *f = fopen(latest, "r");
    if (!f) return -1;
    
    char buf[4096];
    char last_cycle_line[256] = {0};
    *cycle = 0;
    *total_candles = 0;
    
    while (fgets(buf, sizeof(buf), f)) {
        // Look for cycle progress
        if (strstr(buf, "cycle=")) {
            strncpy(last_cycle_line, buf, sizeof(last_cycle_line) - 1);
            // Extract cycle number
            const char *p = strstr(buf, "cycle=");
            if (p) {
                p += 6;
                int c = atoi(p);
                if (c > *cycle) *cycle = c;
            }
        }
        // Look for candle count
        if (strstr(buf, "candles") || strstr(buf, "Candles")) {
            const char *p = strstr(buf, "candle");
            if (p) {
                // Scan backward for number
                const char *q = p - 2;
                while (q > buf && (*q >= '0' && *q <= '9')) q--;
                *total_candles = atoi(q + 1);
            }
        }
    }
    fclose(f);
    
    if (last_cycle_line[0])
        strncpy(cycle_buf, last_cycle_line, sz - 1);
    
    return 0;
}

// ── Check paper stats ──
static int read_paper_stats(double *avg_cap, double *avg_wr, int *trades) {
    FILE *f = fopen("/home/wubu2/money-room/data/paper_stats.json", "r");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 10) { fclose(f); return -1; }
    rewind(f);
    
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    fclose(f);
    buf[sz] = '\0';
    
    // Simple JSON field extraction
    const char *p;
    if ((p = strstr(buf, "\"avg_capital\":"))) {
        p += 14;
        *avg_cap = atof(p);
    }
    if ((p = strstr(buf, "\"avg_win_rate\":"))) {
        p += 16;
        *avg_wr = atof(p);
    }
    if ((p = strstr(buf, "\"total_trades\":"))) {
        p += 16;
        *trades = atoi(p);
    }
    
    free(buf);
    return 0;
}

int main(void) {
    json_buf_t jb;
    jb_init(&jb);
    
    jb_put(&jb, "{\n");
    
    // Timestamp
    jb_printf(&jb, "  \"timestamp\": %ld,\n", (long)time(NULL));
    
    // Paper training progress
    char cycle_info[256] = {0};
    int cycle = 0, total_candles = 0;
    if (scan_training_log(cycle_info, sizeof(cycle_info), &cycle, &total_candles) == 0) {
        jb_printf(&jb, "  \"paper_training\": {\n");
        jb_printf(&jb, "    \"current_cycle\": %d,\n", cycle);
        jb_printf(&jb, "    \"total_candles\": %d,\n", total_candles);
        jb_printf(&jb, "    \"progress_pct\": %.1f,\n",
                  total_candles > 0 ? (double)cycle / total_candles * 100.0 : 0.0);
        jb_printf(&jb, "    \"status\": \"%s\",\n",
                  cycle > 0 ? (cycle >= total_candles ? "complete" : "running") : "idle");
        // Estimated time remaining
        if (cycle > 10 && total_candles > 0) {
            double elapsed_sec = cycle * 0.005; // 5ms per cycle
            double total_sec = total_candles * 0.005;
            double remaining_sec = total_sec - elapsed_sec;
            if (remaining_sec > 0) {
                jb_printf(&jb, "    \"estimated_remaining_sec\": %.0f,\n", remaining_sec);
            }
        }
        // Latest log snippet
        if (cycle_info[0]) {
            // Escape for JSON
            json_buf_t escaped;
            jb_init(&escaped);
            for (char *c = cycle_info; *c; c++) {
                if (*c == '"' || *c == '\\') jb_put(&escaped, "\\");
                if (*c == '\n') jb_put(&escaped, "\\n");
                else { char s[2] = {*c, 0}; jb_put(&escaped, s); }
            }
            jb_printf(&jb, "    \"latest_output\": \"%s\",\n", escaped.buf);
            free(escaped.buf);
        }
        jb_printf(&jb, "    \"agent_count\": 2500\n");
        jb_put(&jb, "  },\n");
    } else {
        jb_put(&jb, "  \"paper_training\": {\"status\": \"no_data\"},\n");
    }
    
    // Paper live stats
    double avg_cap = 0, avg_wr = 0;
    int trades = 0;
    if (read_paper_stats(&avg_cap, &avg_wr, &trades) == 0) {
        jb_printf(&jb, "  \"paper_live\": {\n");
        jb_printf(&jb, "    \"avg_capital\": %.2f,\n", avg_cap);
        jb_printf(&jb, "    \"avg_win_rate\": %.4f,\n", avg_wr);
        jb_printf(&jb, "    \"total_trades\": %d,\n", trades);
        jb_printf(&jb, "    \"status\": \"active\"\n");
        jb_put(&jb, "  },\n");
    } else {
        jb_put(&jb, "  \"paper_live\": {\"status\": \"starting\"},\n");
    }
    
    // BTC CSV freshness
    struct stat csv_st;
    if (stat("/home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv", &csv_st) == 0) {
        int age_sec = (int)(time(NULL) - csv_st.st_mtime);
        jb_printf(&jb, "  \"btc_csv\": {\n");
        jb_printf(&jb, "    \"age_sec\": %d,\n", age_sec);
        jb_printf(&jb, "    \"fresh\": %s\n", age_sec < 900 ? "true" : "false");
        jb_put(&jb, "  },\n");
    }
    
    // System health
    jb_put(&jb, "  \"system\": {\n");
    jb_printf(&jb, "    \"paper_daemon_running\": %s,\n",
              system("pgrep -x paper_live_bridge >/dev/null 2>&1") == 0 ? "true" : "false");
    jb_printf(&jb, "    \"paper_training_running\": %s,\n",
              system("pgrep -f paper_train.sh >/dev/null 2>&1") == 0 ? "true" : "false");
    jb_printf(&jb, "    \"live_engine_running\": %s\n",
              system("pgrep -x room_engine >/dev/null 2>&1") == 0 ? "true" : "false");
    jb_put(&jb, "  }\n");
    
    jb_put(&jb, "}\n");
    
    // Write output
    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/evolution_progress.json", OUT_DIR);
    FILE *f = fopen(out_path, "w");
    if (f) {
        fputs(jb.buf, f);
        fclose(f);
        printf("[EVOLUTION] Written %zu bytes to %s\n", jb.len, out_path);
    }
    
    free(jb.buf);
    return 0;
}
