/*
 * stale_alerter.c — Stale collector alerting
 * Runs collector_health, counts stale/fail/timeout/missing collectors.
 * Silent if all healthy (no_agent=True pattern).
 * Only outputs alert text when issues detected.
 *
 * gcc -O2 -o stale_alerter stale_alerter.c -lcurl -ljansson -lsqlite3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_OUTPUT 65536
#define MAX_ALERT 8192

int main(void) {
    FILE *fp = popen("/home/wubu2/money-room/engine/source_health 2>/dev/null", "r");
    if (!fp) return 0;

    char buf[MAX_OUTPUT];
    size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[len] = '\0';
    int exit_code = pclose(fp);
    if (exit_code != 0 || len < 10) return 0; /* silent */

    /* Line-by-line parse: scan for {"name":"...","status":"...","age_min":...,"stale":...} */
    int total = 0, ok = 0, warn = 0, fail = 0, timeout = 0, missing = 0, running = 0, stale = 0;
    char stale_list[1024] = {0};
    char *line = buf;
    char *nl;

    while ((nl = strchr(line, '\n')) != NULL) {
        *nl = '\0';

        /* Check for collector objects specifically */
        if (strstr(line, "\"name\":") && strstr(line, "\"status\":")) {
            total++;

            /* Extract status */
            char *st = strstr(line, "\"status\": \"");
            if (st) {
                st += 11; /* skip past "status": " */
                char *end = strchr(st, '"');
                if (end) {
                    *end = '\0';
                    if (strcmp(st, "ok") == 0) ok++;
                    else if (strcmp(st, "warn") == 0) warn++;
                    else if (strcmp(st, "fail") == 0) fail++;
                    else if (strcmp(st, "timeout") == 0) timeout++;
                    else if (strcmp(st, "missing") == 0) missing++;
                    else if (strcmp(st, "running") == 0) running++;
                    else warn++; /* unknown status is a warn */
                }
            }

            /* Check stale */
            char *sage = strstr(line, "\"stale\": 1");
            if (sage) {
                stale++;
                /* Extract name */
                char *nm = strstr(line, "\"name\": \"");
                if (nm) {
                    nm += 9;
                    char *nm_end = strchr(nm, '"');
                    if (nm_end) {
                        size_t nmlen = nm_end - nm;
                        if (stale_list[0]) strncat(stale_list, ", ", sizeof(stale_list) - strlen(stale_list) - 1);
                        strncat(stale_list, nm, nmlen < 40 ? nmlen : 40);
                    }
                }
            }
        }

        line = nl + 1;
    }

    int issues = fail + timeout + missing;
    int needs_alert = (issues > 0) || (stale > 3);

    if (needs_alert) {
        time_t now = time(NULL);
        char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

        char alert[MAX_ALERT];
        int n = snprintf(alert, sizeof(alert),
            "[%s] COLLECTOR HEALTH ALERT — %d/%d issues\n"
            "  OK: %d | Warn: %d | Fail: %d | Timeout: %d | Missing: %d | Stale: %d\n",
            ts, issues, total, ok, warn, fail, timeout, missing, stale);

        if (issues > 0) {
            n += snprintf(alert + n, sizeof(alert) - n,
                "  Broken: %d collectors need attention (fail=%d, timeout=%d, missing=%d)\n",
                issues, fail, timeout, missing);
        }
        if (stale > 0 && stale_list[0]) {
            n += snprintf(alert + n, sizeof(alert) - n, "  Stale: [%s]\n", stale_list);
        }

        /* Write to log file */
        FILE *log = fopen("/home/wubu2/.hermes/pm_logs/stale_alerter.log", "a");
        if (log) {
            fprintf(log, "%s", alert);
            fclose(log);
        }

        /* Output to stdout for cron delivery */
        printf("%s", alert);
    }

    return 0; /* always success */
}
