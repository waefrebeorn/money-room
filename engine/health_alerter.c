/**
 * health_alerter.c — Monitoring/alerting wrapper for health_check
 *
 * Runs health_check --check. If exit != 0, writes an alert marker file
 * with timestamp + issue summary from health.json. If healthy, removes
 * any stale alert.
 *
 * Compile:
 *   gcc -O2 -o health_alerter health_alerter.c -ljansson
 *
 * Usage:
 *   ./health_alerter
 *
 * Cron: every 5 minutes (same cadence as health_check)
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <jansson.h>

#define HEALTH_CHECK   "/home/wubu2/money-room/engine/health_check"
#define HEALTH_JSON    "/home/wubu2/money-room/docs/data/health.json"
#define ALERT_FILE     "/home/wubu2/.hermes/pm_logs/.health_alert"
#define HISTORY_LOG    "/home/wubu2/.hermes/pm_logs/health_alert_history.log"
#define DATA_FILE      "/home/wubu2/money-room/data/alert_status.json"

static void log_alert(const char *msg) {
    FILE *f = fopen(HISTORY_LOG, "a");
    if (!f) return;
    time_t t = time(0);
    struct tm *tm = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(f, "[%s] %s\n", buf, msg);
    fclose(f);
}

int main(void) {
    /* Run health_check --check (exit code only) */
    int ret = system(HEALTH_CHECK " --check >/dev/null 2>&1");
    int healthy = (ret == 0);

    if (healthy) {
        /* Remove alert if present */
        if (access(ALERT_FILE, F_OK) == 0) {
            remove(ALERT_FILE);
            log_alert("HEALTH RESTORED — alert cleared");
        }
        /* Remove stale alert status JSON */
        FILE *f = fopen(DATA_FILE, "w");
        if (f) {
            fprintf(f, "{\"status\":\"healthy\",\"timestamp\":%ld}\n", (long)time(0));
            fclose(f);
        }
        return 0;
    }

    /* Degraded — read health.json for details */
    FILE *f = fopen(HEALTH_JSON, "r");
    if (!f) {
        /* health.json not yet written or stale */
        FILE *af = fopen(ALERT_FILE, "w");
        if (af) {
            fprintf(af, "HEALTH_DEGRADED — health_check exited %d\n", ret);
            fclose(af);
        }
        log_alert("HEALTH_DEGRADED — health_check exited non-zero, no JSON");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    json_error_t err;
    json_t *root = json_loads(buf, 0, &err);
    free(buf);

    if (!root) {
        log_alert("HEALTH_DEGRADED — health.json parse error");
        return 1;
    }

    /* Extract failure info */
    const char *issues = "";
    json_t *j_issues = json_object_get(root, "issues");
    if (j_issues && json_is_string(j_issues)) {
        issues = json_string_value(j_issues);
    }

    int n_fail = 0, n_warn = 0;
    json_t *j_fail = json_object_get(root, "checks_fail");
    if (j_fail) n_fail = (int)json_integer_value(j_fail);
    json_t *j_warn = json_object_get(root, "checks_warn");
    if (j_warn) n_warn = (int)json_integer_value(j_warn);

    /* Write alert file with details */
    FILE *af = fopen(ALERT_FILE, "w");
    if (af) {
        time_t t = time(0);
        fprintf(af, "HEALTH_DEGRADED — %d fail, %d warn at %ld\n%s\n",
                n_fail, n_warn, (long)t, issues);
        fclose(af);
    }

    /* Write alert_status.json for dashboard */
    af = fopen(DATA_FILE, "w");
    if (af) {
        fprintf(af, "{\"status\":\"degraded\",\"fail\":%d,\"warn\":%d,\"issues\":%s,\"timestamp\":%ld}\n",
                n_fail, n_warn, json_dumps(j_issues ? j_issues : json_null(), 0), (long)time(0));
        fclose(af);
    }

    /* Log to history */
    char hmsg[512];
    snprintf(hmsg, sizeof(hmsg), "HEALTH_DEGRADED — %d fail, %d warn", n_fail, n_warn);
    log_alert(hmsg);

    json_decref(root);
    return 1;
}
