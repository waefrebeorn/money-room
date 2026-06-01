/**
 * weather_collector.c — Daily weather binary outcome data
 * Open-Meteo: free, no API key, historical data 1940-2026
 * 
 * For each city, records binary outcome: temp_max > 30-day avg = YES(1)
 * Writes to weather_outcomes.json for multi-market trainer.
 *
 * Compile: gcc -O2 -o weather_collector weather_collector.c -lcurl -ljansson -lm
 * Usage:   ./weather_collector [--days N] [--cities N]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sys/stat.h>
#include <sys/types.h>

#define OUT_DIR  "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/weather_data.json"

// Helper: parse YYYY-MM-DD to epoch
static int64_t date_to_epoch(const char *date) {
    struct tm tm = {0};
    sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
    tm.tm_year -= 1900; tm.tm_mon -= 1;
    return (int64_t)mktime(&tm);
}

typedef struct { char *data; size_t len; } http_buf_t;

static size_t write_cb(void *ptr, size_t s, size_t n, void *u) {
    size_t t = s * n; http_buf_t *b = (http_buf_t*)u;
    char *np = realloc(b->data, b->len + t + 1);
    if (!np) return 0; b->data = np;
    memcpy(b->data + b->len, ptr, t); b->len += t; b->data[b->len] = '\0';
    return t;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    http_buf_t b = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "weather-collector/1.0");
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) { fprintf(stderr, "[WX] HTTP error: %s\n", curl_easy_strerror(r)); free(b.data); return NULL; }
    return b.data;
}

// Cities to track
typedef struct { const char *name, *lat, *lon; } City;
static City CITIES[] = {
    {"NYC",     "40.7128",  "-74.0060"},
    {"London",  "51.5074",  "-0.1278"},
    {"Tokyo",   "35.6762",  "139.6503"},
    {"Chicago", "41.8781",  "-87.6298"},
    {"Dubai",   "25.2048",  "55.2708"},
    {"Sydney",  "-33.8688", "151.2093"},
    {"Mumbai",  "19.0760",  "72.8777"},
    {"Moscow",  "55.7558",  "37.6173"},
};
#define N_CITIES 8

int main(int argc, char **argv) {
    int days_back = 365;  // 1 year of daily data
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--days") == 0 && i+1 < argc) days_back = atoi(argv[++i]);
        if (strcmp(argv[i], "--help") == 0 || strncmp(argv[i], "-h", 2) == 0) {
            printf("Usage: %s [--days N]\n", argv[0]); return 0;
        }
    }
    
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Compute date range
    time_t now = time(NULL);
    time_t past = now - (time_t)days_back * 86400;
    struct tm *tp = gmtime(&past);
    char start_date[32]; snprintf(start_date, sizeof(start_date), "%04d-%02d-%02d", tp->tm_year+1900, tp->tm_mon+1, tp->tm_mday);
    tp = gmtime(&now);
    char end_date[32]; snprintf(end_date, sizeof(end_date), "%04d-%02d-%02d", tp->tm_year+1900, tp->tm_mon+1, tp->tm_mday);
    
    printf("[WX] Weather Collector — %d days, %d cities\n", days_back, N_CITIES);
    printf("[WX] Range: %s → %s\n", start_date, end_date);
    
    // JSON array for output
    json_t *root = json_array();
    int total_days = 0;
    
    for (int c = 0; c < N_CITIES; c++) {
        printf("[WX] Fetching %s...\n", CITIES[c].name);
        
        char url[512];
        snprintf(url, sizeof(url),
            "https://archive-api.open-meteo.com/v1/archive"
            "?latitude=%s&longitude=%s"
            "&start_date=%s&end_date=%s"
            "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,wind_speed_10m_max,wind_gusts_10m_max,shortwave_radiation_sum"
            "&timezone=UTC",
            CITIES[c].lat, CITIES[c].lon, start_date, end_date);
        
        char *resp = http_get(url);
        if (!resp) { fprintf(stderr, "[WX] FAILED %s\n", CITIES[c].name); continue; }
        
        json_error_t err;
        json_t *j = json_loads(resp, 0, &err);
        free(resp);
        if (!j) { fprintf(stderr, "[WX] JSON parse error: %s\n", err.text); continue; }
        
        json_t *daily = json_object_get(j, "daily");
        if (!daily || !json_is_object(daily)) { json_decref(j); continue; }
        
        json_t *jtime = json_object_get(daily, "time");
        json_t *jmax = json_object_get(daily, "temperature_2m_max");
        json_t *jmin = json_object_get(daily, "temperature_2m_min");
        json_t *jprecip = json_object_get(daily, "precipitation_sum");
        json_t *jwind = json_object_get(daily, "wind_speed_10m_max");
        json_t *jgust = json_object_get(daily, "wind_gusts_10m_max");
        json_t *jsolar = json_object_get(daily, "shortwave_radiation_sum");
        
        if (!jtime || !jmax || !json_is_array(jtime)) { json_decref(j); continue; }
        
        int n = (int)json_array_size(jtime);
        int city_days = 0;
        
        // Compute 30-day rolling average for baseline
        double *temps = calloc(n, sizeof(double));
        for (int i = 0; i < n; i++) {
            json_t *v = json_array_get(jmax, i);
            temps[i] = v ? json_number_value(v) : 0;
        }
        
        // Build window for 30-day avg
        double window_sum = 0;
        int window_count = 0;
        for (int i = 0; i < n && i < 30; i++) { window_sum += temps[i]; window_count++; }
        
        for (int i = 0; i < n; i++) {
            const char *date = json_string_value(json_array_get(jtime, i));
            if (!date) continue;
            
            double tmax = temps[i];
            double tmin = json_number_value(json_array_get(jmin, i));
            double precip = jprecip ? json_number_value(json_array_get(jprecip, i)) : 0;
            double wind = jwind ? json_number_value(json_array_get(jwind, i)) : 0;
            double gust = jgust ? json_number_value(json_array_get(jgust, i)) : 0;
            double solar = jsolar ? json_number_value(json_array_get(jsolar, i)) : 0;
            
            // Update rolling window
            if (i >= 30) {
                double old = temps[i-30];
                window_sum = window_sum - old + tmax;
            } else {
                window_sum += tmax;
                window_count++;
            }
            double avg30 = window_sum / window_count;
            
            // Binary outcome: temp_max > 30-day avg = WARM (1), else COOL (0)
            int outcome = (tmax > avg30) ? 1 : 0;
            
            // Create feature vector for training
            json_t *entry = json_object();
            json_object_set_new(entry, "city", json_string(CITIES[c].name));
            json_object_set_new(entry, "date", json_string(date));
            json_object_set_new(entry, "timestamp", json_integer(date_to_epoch(date)));
            json_object_set_new(entry, "temp_max", json_real(tmax));
            json_object_set_new(entry, "temp_min", json_real(tmin));
            json_object_set_new(entry, "precip", json_real(precip));
            json_object_set_new(entry, "wind_speed", json_real(wind));
            json_object_set_new(entry, "wind_gust", json_real(gust));
            json_object_set_new(entry, "solar_radiation", json_real(solar));
            json_object_set_new(entry, "avg_30day", json_real(avg30));
            json_object_set_new(entry, "outcome", json_integer(outcome));
            
            // Features for ML training
            json_t *feats = json_array();
            json_array_append_new(feats, json_real((tmax - avg30) / fmax(avg30, 1.0)));  // F0: temp deviation
            json_array_append_new(feats, json_real(tmax / 50.0));  // F1: normalized max temp
            // F2: Was yesterday warm? (0/1)
            int prev_outcome = (i > 0) ? ((temps[i-1] > (i>1 ? (window_sum - tmax + (i>=30 ? temps[i-30] : 0)) / (window_count) : avg30)) ? 1 : 0) : outcome;
            json_array_append_new(feats, json_integer(prev_outcome));
            // F3: Precipitation intensity (normalized to 0-1 by 50mm cap)
            json_array_append_new(feats, json_real(fmin(precip / 50.0, 1.0)));
            // F4: Wind speed normalized (0-1 by 20 m/s cap)
            json_array_append_new(feats, json_real(fmin(wind / 20.0, 1.0)));
            // F5: Wind gust / speed ratio (gustiness)
            json_array_append_new(feats, json_real(wind > 0 ? fmin(gust / wind, 3.0) / 3.0 : 0.0));
            // F6: Solar radiation normalized (0-1 by 400 Wh/m² cap)
            json_array_append_new(feats, json_real(fmin(solar / 400.0, 1.0)));
            json_object_set_new(entry, "features", feats);
            
            json_array_append_new(root, entry);
            city_days++;
            total_days++;
        }
        
        free(temps);
        json_decref(j);
        printf("[WX] %s: %d days\n", CITIES[c].name, city_days);
    }
    
    // Write output
    mkdir(OUT_DIR, 0755);
    if (json_dump_file(root, OUT_FILE, JSON_INDENT(2)) == 0) {
        printf("\n[WX] Written: %s (%d entries)\n", OUT_FILE, total_days);
    } else {
        fprintf(stderr, "[WX] Failed to write %s\n", OUT_FILE);
    }
    
    json_decref(root);
    curl_global_cleanup();
    return 0;
}
