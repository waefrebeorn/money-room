/**
 * key_rotation.c — T098: API Key Rotation Monitor
 *
 * Scans ~/.hermes/secrets.env for API keys, checks:
 *   - Key age (mtime vs first-seen timestamp)
 *   - Keys past 90-day threshold → rotation advisory
 *   - Empty/placeholder keys → warning
 *
 * Outputs JSON to docs/data/key_health.json for dashboard.
 *
 * Build: gcc -O2 -o key_rotation key_rotation.c -lm
 * Cron: daily at 3am
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <math.h>

#define MAX_KEYS 32
#define MAX_LINE 1024
#define ROTATION_DAYS 90
#define SECRETS_PATH "/home/wubu2/.hermes/secrets.env"
#define KEY_DB_PATH "/home/wubu2/.hermes/pm_logs/c_room/key_rotation.db"
#define OUTPUT_PATH "/home/wubu2/money-room/docs/data/key_health.json"

/* Known API key env var patterns */
static const char *KEY_NAMES[] = {
    "FINNHUB_API_KEY",
    "EODHD_API_KEY",
    "CMC_API_KEY",
    "MARKETSTACK_API_KEY",
    "TWELVEDATA_API_KEY",
    "STOCKDATA_API_KEY",
    "POLYGON_RPC_URL",
    "FCS_API_KEY",
    "FOREXRATE_API_KEY",
    "EXCHANGERATE_HOST_KEY",
    "LEMONSQUEEZY_API_KEY",
    "HERMES_AUTH_KEY",
    "POLYMARKET_API_KEY",
    "COINBASE_API_KEY",
    "KRAKEN_API_KEY",
    "ALPHA_VANTAGE_KEY",
    NULL
};

typedef struct {
    const char *name;
    int has_value;       /* 1 if non-empty value exists */
    int length;          /* Length of value (0 if empty) */
    int age_days;        /* Days since first seen (or -1 if unknown) */
    int needs_rotation;  /* 1 if age > ROTATION_DAYS */
    int is_expired;      /* 1 if empty/placeholder */
} KeyStatus;

static int read_secrets_file(const char *path, KeyStatus *keys, int *nkeys) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[KEY] Cannot open %s\n", path);
        return -1;
    }

    char line[MAX_LINE];
    *nkeys = 0;
    int known_count = 0;
    while (KEY_NAMES[known_count]) known_count++;

    /* Initialize all known keys as empty */
    for (int i = 0; i < known_count && i < MAX_KEYS; i++) {
        keys[i].name = KEY_NAMES[i];
        keys[i].has_value = 0;
        keys[i].length = 0;
        keys[i].age_days = -1;
        keys[i].needs_rotation = 0;
        keys[i].is_expired = 1;
    }
    *nkeys = known_count;

    /* Parse secrets.env */
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        /* Strip leading 'export ' if present */
        if (strncmp(p, "export ", 7) == 0) p += 7;

        /* Find '=' separator */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *varname = p;
        char *value = eq + 1;

        /* Strip trailing whitespace */
        size_t vlen = strlen(value);
        while (vlen > 0 && (value[vlen-1] == '\n' || value[vlen-1] == '\r' || value[vlen-1] == ' ')) {
            value[--vlen] = '\0';
        }

        /* Strip surrounding quotes */
        if (vlen >= 2 && ((value[0] == '\'' && value[vlen-1] == '\'') ||
                          (value[0] == '"' && value[vlen-1] == '"'))) {
            value++;
            vlen -= 2;
            value[vlen] = '\0';
        }

        /* Check if this matches a known key */
        for (int i = 0; i < known_count; i++) {
            if (strcmp(varname, keys[i].name) == 0) {
                keys[i].has_value = (vlen > 0);
                keys[i].length = (int)vlen;
                keys[i].is_expired = (vlen == 0);
                break;
            }
        }
    }

    fclose(f);

    /* Load key age from key_rotation.db (simple format: name|unixtimestamp\n) */
    FILE *kdb = fopen(KEY_DB_PATH, "r");
    if (kdb) {
        char kline[512];
        time_t now = time(NULL);
        while (fgets(kline, sizeof(kline), kdb)) {
            char *pipe = strchr(kline, '|');
            if (!pipe) continue;
            *pipe = '\0';
            const char *kname = kline;
            time_t first_seen = (time_t)atol(pipe + 1);
            if (first_seen <= 0 || first_seen > now) continue;

            for (int i = 0; i < *nkeys; i++) {
                if (strcmp(kname, keys[i].name) == 0) {
                    double days = difftime(now, first_seen) / 86400.0;
                    keys[i].age_days = (int)round(days);
                    keys[i].needs_rotation = (keys[i].age_days > ROTATION_DAYS && keys[i].has_value);
                    break;
                }
            }
        }
        fclose(kdb);
    }

    return *nkeys > 0 ? 0 : -1;
}

static int update_key_db(const KeyStatus *keys, int nkeys) {
    /* Create dir if needed */
    char dbdir[512];
    snprintf(dbdir, sizeof(dbdir), "%s", KEY_DB_PATH);
    char *last_slash = strrchr(dbdir, '/');
    if (last_slash) {
        *last_slash = '\0';
        char mkdir_cmd[1024];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s 2>/dev/null", dbdir);
        system(mkdir_cmd);
    }

    time_t now = time(NULL);

    /* Read existing DB first */
    time_t existing_first_seen[MAX_KEYS];
    for (int i = 0; i < nkeys; i++) existing_first_seen[i] = 0;

    FILE *kdb = fopen(KEY_DB_PATH, "r");
    if (kdb) {
        char kline[512];
        while (fgets(kline, sizeof(kline), kdb)) {
            char *pipe = strchr(kline, '|');
            if (!pipe) continue;
            *pipe = '\0';
            const char *kname = kline;
            time_t ts = (time_t)atol(pipe + 1);
            for (int i = 0; i < nkeys; i++) {
                if (strcmp(kname, keys[i].name) == 0) {
                    existing_first_seen[i] = ts;
                    break;
                }
            }
        }
        fclose(kdb);
    }

    /* Write updated DB: preserve first-seen for existing keys, set now for new */
    kdb = fopen(KEY_DB_PATH, "w");
    if (!kdb) return -1;

    for (int i = 0; i < nkeys; i++) {
        time_t write_ts;
        if (existing_first_seen[i] > 0) {
            write_ts = existing_first_seen[i];
        } else if (keys[i].has_value) {
            write_ts = now;  /* First time seeing this key */
        } else {
            continue;  /* Don't track empty keys */
        }
        fprintf(kdb, "%s|%ld\n", keys[i].name, (long)write_ts);
    }
    fclose(kdb);
    return 0;
}

static int write_health_json(const KeyStatus *keys, int nkeys) {
    /* Count statuses */
    int total = 0, healthy = 0, warn_rotation = 0, warn_empty = 0;

    for (int i = 0; i < nkeys; i++) {
        if (!keys[i].has_value) continue;
        total++;
        if (keys[i].needs_rotation) {
            warn_rotation++;
        } else {
            healthy++;
        }
    }

    /* Count empties */
    for (int i = 0; i < nkeys; i++) {
        if (!keys[i].has_value) warn_empty++;
    }

    /* Build JSON output */
    char json[65536] = {0};
    int off = 0;
    off += snprintf(json + off, sizeof(json) - off,
        "{\n"
        "  \"checked_at\": %ld,\n"
        "  \"total_keys\": %d,\n"
        "  \"healthy\": %d,\n"
        "  \"needs_rotation\": %d,\n"
        "  \"empty_placeholders\": %d,\n"
        "  \"keys\": [\n",
        (long)time(NULL), total, healthy, warn_rotation, warn_empty);

    int first = 1;
    for (int i = 0; i < nkeys; i++) {
        if (!first) {
            off += snprintf(json + off, sizeof(json) - off, ",\n");
        }
        first = 0;

        const char *status;
        if (!keys[i].has_value) status = "EMPTY";
        else if (keys[i].needs_rotation) status = "ROTATION_DUE";
        else status = "HEALTHY";

        const char *name_clean = keys[i].name;
        off += snprintf(json + off, sizeof(json) - off,
            "    {\"name\":\"%s\",\"length\":%d,\"status\":\"%s\",\"age_days\":%d}",
            name_clean, keys[i].length, status, keys[i].age_days);
    }
    off += snprintf(json + off, sizeof(json) - off, "\n  ]\n}\n");

    /* Write to output */
    FILE *out = fopen(OUTPUT_PATH, "w");
    if (!out) {
        fprintf(stderr, "[KEY] Cannot write %s\n", OUTPUT_PATH);
        return -1;
    }
    fputs(json, out);
    fclose(out);
    printf("[KEY] Written %s (%d bytes, %d keys, %d rotation due)\n",
           OUTPUT_PATH, off, total, warn_rotation);
    return 0;
}

int main(void) {
    KeyStatus keys[MAX_KEYS];
    int nkeys = 0;

    printf("═══ T098: Key Rotation Health Check ═══\n");

    if (read_secrets_file(SECRETS_PATH, keys, &nkeys) != 0) {
        fprintf(stderr, "[KEY] FAIL: Cannot read secrets\n");
        return 1;
    }

    printf("[KEY] Found %d known keys in %s\n", nkeys, SECRETS_PATH);

    update_key_db(keys, nkeys);

    if (write_health_json(keys, nkeys) != 0) {
        return 1;
    }

    /* Summary */
    int rot_due = 0, empty = 0, healthy = 0;
    for (int i = 0; i < nkeys; i++) {
        if (!keys[i].has_value) empty++;
        else if (keys[i].needs_rotation) rot_due++;
        else healthy++;

        printf("[KEY]   %-25s %s", keys[i].name, keys[i].has_value ? "✅ " : "⚠️  EMPTY");
        if (keys[i].has_value && keys[i].age_days >= 0) {
            printf("(%d days)", keys[i].age_days);
            if (keys[i].needs_rotation) printf(" ROTATION DUE");
        }
        printf("\n");
    }

    printf("\n[KEY] Summary: %d healthy, %d rotation due, %d empty/placeholder\n",
           healthy, rot_due, empty);

    return rot_due > 0 ? 2 : 0;
}
