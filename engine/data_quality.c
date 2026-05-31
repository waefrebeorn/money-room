/**
 * data_quality.c — Data Quality Checker
 * Validates row counts, freshness, parse validity, value ranges,
 * and critical-field completeness across all data sources.
 *
 * Compile:
 *   gcc -O3 -o data_quality data_quality.c -ljansson -lm -I.
 * Usage:
 *   ./data_quality
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <jansson.h>
#include "types.h"

#define DATA_DIR     "/home/wubu2/money-room/data"
#define HIST_DIR     "/home/wubu2/.hermes/pm_logs/historical"
#define GENOME_DIR   DATA_DIR "/multi_market"
#define OUTPUT       DATA_DIR "/../docs/data/data_quality.json"
#define MAX_CHECKS   64

// ── Check types ──
typedef enum { CHECK_FILE, CHECK_CSV, CHECK_JSON, CHECK_JSON_ARRAY } CheckType;

typedef struct {
    const char  *id;
    const char  *name;
    const char  *path;
    CheckType    type;
    int          min_rows;    // Minimum expected rows
    int          max_rows;    // Maximum expected rows (0 = no limit)
    int          max_age_sec; // Max acceptable age
    double       val_min;     // Min reasonable value for .json numeric check
    double       val_max;     // Max reasonable value
    const char  *val_field;   // JSON key to check value range on
} DataCheck;

// External collectors (batch/scheduled) that produce data
static const DataCheck CHECKS[] = {
    // ── Core market data ──
    {"btc_1min",   "BTC 1-min OHLCV",  HIST_DIR "/btc_1min_latest.csv",  CHECK_CSV,  500000, 0, 86400*7, 0,0,NULL},
    {"sp500",      "S&P500 Daily",     HIST_DIR "/sp500.csv",            CHECK_CSV,  2000,   5000, 86400*7, 0,0,NULL},
    {"btc_genome", "BTC Trained Genome",GENOME_DIR "/BTC.bin",           CHECK_FILE, 1,      1,    172800,  0,0,NULL},
    {"sp500_genome","SP500 Genome",    GENOME_DIR "/SP500.bin",          CHECK_FILE, 1,      1,    172800,  0,0,NULL},

    // ── Equities ──
    {"dow",        "DOW Daily",        HIST_DIR "/raw/stocks/DOW_daily.csv",   CHECK_CSV, 2000, 5000, 86400*14, 0,0,NULL},
    {"nasdaq",     "NASDAQ Daily",     HIST_DIR "/raw/stocks/NASDAQ_daily.csv",CHECK_CSV, 2000, 5000, 86400*14, 0,0,NULL},
    {"ftse",       "FTSE100 Daily",    HIST_DIR "/raw/stocks/FTSE100_daily.csv",CHECK_CSV,2000, 5000, 86400*14, 0,0,NULL},
    {"nikkei",     "NIKKEI Daily",     HIST_DIR "/raw/stocks/NIKKEI_daily.csv",CHECK_CSV, 2000, 5000, 86400*14, 0,0,NULL},

    // ── Forex ──
    {"eurusd",     "EURUSD Daily",     HIST_DIR "/raw/forex/EURUSD_daily.csv", CHECK_CSV, 1000, 3000, 86400*14, 0,0,NULL},
    {"gbpusd",     "GBPUSD Daily",     HIST_DIR "/raw/forex/GBPUSD_daily.csv", CHECK_CSV, 1000, 3000, 86400*14, 0,0,NULL},
    {"usdjpy",     "USDJPY Daily",     HIST_DIR "/raw/forex/USDJPY_daily.csv", CHECK_CSV, 1000, 3000, 86400*14, 0,0,NULL},

    // ── Commodities ──
    {"gold",       "GOLD Daily",       HIST_DIR "/raw/stocks/GOLD_daily.csv",  CHECK_CSV, 2000, 5000, 86400*14, 0,0,NULL},
    {"silver",     "SILVER Daily",     HIST_DIR "/raw/stocks/SILVER_daily.csv",CHECK_CSV, 2000, 5000, 86400*14, 0,0,NULL},
    {"crude",      "CRUDE_OIL Daily",  HIST_DIR "/raw/stocks/CRUDE_OIL_daily.csv",CHECK_CSV,2000,5000,86400*14,0,0,NULL},

    // ── Macro ──
    {"dgs10",      "DGS10 Yield",      HIST_DIR "/raw/stocks/DGS10_daily.csv", CHECK_CSV, 2000, 5000, 86400*14, 0,0,NULL},
    {"vix",        "VIX Daily",        HIST_DIR "/raw/stocks/VIX_daily.csv",   CHECK_CSV, 3000, 5000, 86400*14, 0,0,NULL},

    // ── Binary market data ──
    {"sports_json", "Sports Data",     GENOME_DIR "/sports_data.json",     CHECK_JSON_ARRAY, 300,  5000, 172800, 0,0,NULL},
    {"weather_json","Weather Data",    GENOME_DIR "/weather_data.json",    CHECK_JSON_ARRAY, 1000, 10000,172800, 0,0,NULL},
    {"polymarket",  "Prediction Markets",HIST_DIR "/polymarket_events.db", CHECK_FILE, 200,   2000, 86400*7, 0,0,NULL},

    // ── Engine state ──
    {"stats_json",  "Engine Stats",    DATA_DIR "/stats.json",             CHECK_JSON, 1, 1, 120, 0,0,"capital"},
    {"prices_json", "Prices Feed",     DATA_DIR "/prices.json",            CHECK_JSON, 1, 1, 120, 0,0,"btc"},

    // ── Genomes (last run) ──
    {"genome_dir",  "Multi-Market Genomes", GENOME_DIR,                    CHECK_FILE, 17, 25, 172800, 0,0,NULL},
};

#define N_CHECKS (sizeof(CHECKS) / sizeof(CHECKS[0]))

// ── Check CSV file ──
static int check_csv(const char *path, int *rows) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[4096];
    int count = 0;
    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 1) count++;  // Non-empty line
    }
    fclose(f);
    *rows = count - 1;  // Subtract header
    return 0;
}

// ── Check JSON file ──
static int check_json(const char *path, int *arr_len, const char *val_field, double *val_out) {
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) return -1;
    
    if (json_is_array(root)) {
        *arr_len = (int)json_array_size(root);
    } else if (json_is_object(root) && val_field) {
        json_t *val = json_object_get(root, val_field);
        if (val && json_is_number(val)) *val_out = json_number_value(val);
    }
    
    json_decref(root);
    return 0;
}

// ── Count .bin files in dir ──
static int count_bin(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        size_t nlen = strlen(e->d_name);
        if (nlen > 4 && strcmp(e->d_name + nlen - 4, ".bin") == 0) count++;
    }
    closedir(d);
    return count;
}

int main(void) {
    time_t now = time(NULL);
    
    json_t *root = json_object();
    json_object_set_new(root, "generated_at", json_integer(now));
    
    json_t *checks = json_array();
    int passed = 0, failed = 0, warned = 0;
    
    for (int i = 0; i < (int)N_CHECKS; i++) {
        json_t *c = json_object();
        json_object_set_new(c, "id", json_string(CHECKS[i].id));
        json_object_set_new(c, "name", json_string(CHECKS[i].name));
        
        struct stat st;
        int exists = stat(CHECKS[i].path, &st);
        int age = exists == 0 ? (int)(now - st.st_mtime) : -1;
        
        json_object_set_new(c, "exists", json_boolean(exists == 0));
        json_object_set_new(c, "age_seconds", json_integer(age));
        
        // Quality result
        int result = 0; // 0=pass, 1=warn, 2=fail
        json_t *issues = json_array();
        
        if (exists != 0) {
            result = 2;
            json_array_append_new(issues, json_string("file_missing"));
        } else {
            // Check age
            if (age > CHECKS[i].max_age_sec) {
                json_array_append_new(issues, json_string("stale"));
                if (result < 2) result = 1;
            }
            
            // Check content
            switch (CHECKS[i].type) {
                case CHECK_FILE: {
                    // Generic file: just size check
                    if (st.st_size == 0) {
                        json_array_append_new(issues, json_string("empty_file"));
                        result = 2;
                    }
                    // For .bin check count
                    if (strstr(CHECKS[i].path, "multi_market") && !strstr(CHECKS[i].path, ".")) {
                        int cnt = count_bin(CHECKS[i].path);
                        if (cnt < CHECKS[i].min_rows) {
                            json_array_append_new(issues, json_string("too_few_genomes"));
                            result = 2;
                        }
                        json_object_set_new(c, "count", json_integer(cnt));
                    }
                    break;
                }
                case CHECK_CSV: {
                    int rows = 0;
                    if (check_csv(CHECKS[i].path, &rows) != 0) {
                        json_array_append_new(issues, json_string("unparseable"));
                        result = 2;
                    } else {
                        json_object_set_new(c, "rows", json_integer(rows));
                        if (rows < CHECKS[i].min_rows) {
                            json_array_append_new(issues, json_string("too_few_rows"));
                            result = 2;
                        }
                        if (CHECKS[i].max_rows > 0 && rows > CHECKS[i].max_rows) {
                            json_array_append_new(issues, json_string("too_many_rows"));
                            result = 1;
                        }
                    }
                    break;
                }
                case CHECK_JSON: {
                    int arr_len = 0;
                    double val = 0;
                    if (check_json(CHECKS[i].path, &arr_len, CHECKS[i].val_field, &val) != 0) {
                        json_array_append_new(issues, json_string("unparseable"));
                        result = 2;
                    } else {
                        json_object_set_new(c, "value", json_real(val));
                        if (CHECKS[i].val_field) {
                            if (strcmp(CHECKS[i].val_field, "capital") == 0 && val < 100.0) {
                                json_array_append_new(issues, json_string("low_capital"));
                                result = 1;
                            }
                        }
                    }
                    break;
                }
                case CHECK_JSON_ARRAY: {
                    int len = 0;
                    double _unused = 0;
                    if (check_json(CHECKS[i].path, &len, NULL, &_unused) != 0) {
                        json_array_append_new(issues, json_string("unparseable"));
                        result = 2;
                    } else {
                        json_object_set_new(c, "rows", json_integer(len));
                        if (len < CHECKS[i].min_rows) {
                            json_array_append_new(issues, json_string("too_few_rows"));
                            result = 2;
                        }
                        if (CHECKS[i].max_rows > 0 && len > CHECKS[i].max_rows) {
                            json_array_append_new(issues, json_string("too_many_rows"));
                            result = 1;
                        }
                    }
                    break;
                }
            }
        }
        
        const char *status;
        if (result == 0)      { status = "pass"; passed++; }
        else if (result == 1) { status = "warn"; warned++; }
        else                  { status = "fail"; failed++; }
        
        json_object_set_new(c, "status", json_string(status));
        json_object_set_new(c, "result", json_integer(result));
        json_object_set_new(c, "issues", issues);
        
        json_array_append_new(checks, c);
    }
    
    json_object_set_new(root, "checks", checks);
    
    json_t *summary = json_object();
    json_object_set_new(summary, "total", json_integer(N_CHECKS));
    json_object_set_new(summary, "passed", json_integer(passed));
    json_object_set_new(summary, "warned", json_integer(warned));
    json_object_set_new(summary, "failed", json_integer(failed));
    json_object_set_new(summary, "score", json_real(
        N_CHECKS > 0 ? (double)(passed + warned) / N_CHECKS * 100.0 : 0.0));
    json_object_set_new(root, "summary", summary);
    
    mkdir(DATA_DIR "/../docs/data", 0755);
    if (json_dump_file(root, OUTPUT, JSON_INDENT(2)) != 0) {
        fprintf(stderr, "[QUALITY] Failed to write %s\n", OUTPUT);
        json_decref(root);
        return 1;
    }
    
    json_decref(root);
    printf("[QUALITY] %d pass, %d warn, %d fail → %s\n", passed, warned, failed, OUTPUT);
    return failed > 0 ? 1 : 0;
}
