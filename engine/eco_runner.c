/**
 * eco_runner.c — T2/E2: Money room ecosystem runner.
 * Replaces eco_runner.py (139 lines Python, runs every 5min).
 * 
 * Checks eco freshness, warms Kraken feed, cycles all 4 rooms.
 * Writes heartbeat on completion.
 * 
 * Compile: gcc -O3 -o eco_runner eco_runner.c -lcurl -ljansson -lm
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <jansson.h>

#define ECO_DIR      "/home/wubu2/.hermes/pm_logs/eco"
#define C_ROOM_DIR   "/home/wubu2/.hermes/pm_logs/c_room"
#define ROOMS_DIR    "/home/wubu2/.hermes/pm_logs/rooms"
#define HB_DIR       "/home/wubu2/.hermes/infra/heartbeats"

static void log_msg(const char *msg) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    printf("[eco] %02d:%02d:%02d %s\n",
           tm->tm_hour, tm->tm_min, tm->tm_sec, msg);
}

// ── Check if eco is running (minute_log.jsonl freshness) ──
typedef struct {
    int frozen;
    int age_min;
    double mean_pnl;
    int total_trades;
    int open_trades;
    char reason[128];
} EcoStatus;

static EcoStatus check_eco_freshness(void) {
    EcoStatus eco = {1, 999, 0, 0, 0, "unknown"};
    
    char path[512];
    snprintf(path, sizeof(path), "%s/minute_log.jsonl", ECO_DIR);
    
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(eco.reason, sizeof(eco.reason), "minute_log missing");
        return eco;
    }
    
    time_t now = time(NULL);
    int age = (int)(now - st.st_mtime);
    
    // Read last line for summary
    FILE *f = fopen(path, "r");
    if (f) {
        fseek(f, -2048, SEEK_END);  // Read last ~2KB
        char buf[4096];
        int n = fread(buf, 1, sizeof(buf)-1, f);
        fclose(f);
        buf[n] = '\0';
        
        // Find last complete line
        char *last_newline = strrchr(buf, '\n');
        if (last_newline && last_newline > buf) {
            *last_newline = '\0';
            char *last_line = strrchr(buf, '\n');
            if (!last_line) last_line = buf;
            else last_line++;
            
            json_error_t err;
            json_t *j = json_loads(last_line, 0, &err);
            if (j) {
                json_t *jp = json_object_get(j, "mean_pnl");
                if (jp) eco.mean_pnl = json_number_value(jp);
                json_t *jt = json_object_get(j, "total_trades");
                if (jt) eco.total_trades = (int)json_integer_value(jt);
                json_t *jo = json_object_get(j, "open_trades");
                if (jo) eco.open_trades = (int)json_integer_value(jo);
                json_decref(j);
            }
        }
        
        eco.frozen = (age > 1800);  // 30 min threshold
        eco.age_min = age / 60;
        snprintf(eco.reason, sizeof(eco.reason), "%s", eco.frozen ? "stale" : "fresh");
    } else {
        snprintf(eco.reason, sizeof(eco.reason), "can't open minute_log");
    }
    
    return eco;
}

// ── HTTP buffer (reuse from room_feed_bridge) ──
typedef struct {
    char *data;
    size_t len;
} http_buf_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    http_buf_t *buf = (http_buf_t*)userdata;
    char *newp = realloc(buf->data, buf->len + total + 1);
    if (!newp) return 0;
    buf->data = newp;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *http_get(const char *url, long timeout_sec) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    http_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "eco-runner/1.0");
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[eco] HTTP error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

// ── Poll Kraken for fresh BTC data ──
static json_t *warmup_kraken_feed(void) {
    // POST to Kraken OHLC endpoint
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    
    http_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.kraken.com/0/public/OHLC");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "pair=XBTUSD&interval=1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "eco-runner/1.0");
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || !buf.data) {
        free(buf.data);
        return NULL;
    }
    
    json_error_t err;
    json_t *root = json_loads(buf.data, 0, &err);
    free(buf.data);
    if (!root) return NULL;
    
    json_t *result = json_object_get(root, "result");
    if (!result) { json_decref(root); return NULL; }
    
    // Find first key (the pair name from Kraken)
    const char *key;
    json_t *candles;
    json_object_foreach(result, key, candles) {
        if (json_is_array(candles) && json_array_size(candles) > 0) {
            json_t *last = json_array_get(candles, json_array_size(candles) - 1);
            if (!last || !json_is_array(last)) { json_decref(root); return NULL; }
            json_t *out = json_object();
            // [time, open, high, low, close, vwap, volume, count]
            json_t *j_ts = json_array_get(last, 0);
            json_t *j_close = json_array_get(last, 4);
            json_t *j_vol = json_array_get(last, 6);
            json_object_set_new(out, "ts", j_ts ? json_integer((json_int_t)json_number_value(j_ts)) : json_integer(0));
            double cval = 0, vval = 0;
            if (j_close) {
                if (json_is_string(j_close)) cval = atof(json_string_value(j_close));
                else cval = json_number_value(j_close);
            }
            if (j_vol) {
                if (json_is_string(j_vol)) vval = atof(json_string_value(j_vol));
                else vval = json_number_value(j_vol);
            }
            json_object_set_new(out, "close", json_real(cval));
            json_object_set_new(out, "volume", json_real(vval));
            json_object_set_new(out, "pair", json_string(key ? key : "XBTUSD"));
            json_decref(root);
            return out;
        }
    }
    
    json_decref(root);
    return NULL;
}

// ── Write heartbeat ──
static void write_heartbeat(void) {
    mkdir(HB_DIR, 0755);
    char path[512];
    snprintf(path, sizeof(path), "%s/eco-runner.heartbeat", HB_DIR);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%ld", (long)time(NULL));
        fclose(f);
    }
}

// ── Cycle all room engines ──
const char *ROOMS[] = {"btc_main", "macro", "momentum", "polymarket"};
#define N_ROOMS 4

static void cycle_rooms(void) {
    for (int i = 0; i < N_ROOMS; i++) {
        char engine_path[512];
        snprintf(engine_path, sizeof(engine_path), "%s/%s/room_engine", ROOMS_DIR, ROOMS[i]);
        
        struct stat st;
        if (stat(engine_path, &st) != 0) continue;  // No engine in this room
        
        char cmd[1024];
        char workdir[512];
        snprintf(workdir, sizeof(workdir), "%s/%s", ROOMS_DIR, ROOMS[i]);
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && timeout 15 '%s' 2>/dev/null | tail -c 80",
                 workdir, engine_path);
        
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char out[256] = {0};
            if (fgets(out, sizeof(out), fp)) {
                size_t len = strlen(out);
                while (len > 0 && (out[len-1] == '\n' || out[len-1] == '\r')) out[--len] = '\0';
            }
            int exit_code = pclose(fp);
            char msg[512];
            if (exit_code == 124 || exit_code == 0) {
                snprintf(msg, sizeof(msg), "Room %s: done (%s)", ROOMS[i],
                         exit_code == 124 ? "timeout" : "ok");
            } else {
                snprintf(msg, sizeof(msg), "Room %s: exit=%d", ROOMS[i], exit_code);
            }
            log_msg(msg);
        } else {
            char msg[512];
            snprintf(msg, sizeof(msg), "Room %s: failed to execute", ROOMS[i]);
            log_msg(msg);
        }
    }
}

int main(void) {
    log_msg("Eco runner starting...");
    
    // 1. Check eco freshness
    EcoStatus eco = check_eco_freshness();
    if (eco.frozen) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Eco FROZEN (%dm stale) — %s",
                 eco.age_min, eco.reason);
        log_msg(msg);
    } else {
        char msg[512];
        snprintf(msg, sizeof(msg), "Eco alive: %dm stale, mean_pnl=$%.2f, trades=%d",
                 eco.age_min, eco.mean_pnl, eco.total_trades);
        log_msg(msg);
    }
    
    // 2. Warm up Kraken feed
    json_t *candle = warmup_kraken_feed();
    if (candle) {
        json_t *j_pair = json_object_get(candle, "pair");
        json_t *j_close = json_object_get(candle, "close");
        json_t *j_vol = json_object_get(candle, "volume");
        const char *pair = j_pair ? json_string_value(j_pair) : "?";
        double close = j_close ? json_number_value(j_close) : 0;
        double vol = j_vol ? json_number_value(j_vol) : 0;
        char msg[512];
        snprintf(msg, sizeof(msg), "Kraken: %s @ $%.2f vol=%.1f", pair, close, vol);
        log_msg(msg);
        json_decref(candle);
    } else {
        log_msg("Kraken: no data");
    }
    
    // 3. Cycle all rooms
    cycle_rooms();
    
    // 4. Heartbeat
    write_heartbeat();
    
    log_msg("Eco runner complete");
    return 0;
}
