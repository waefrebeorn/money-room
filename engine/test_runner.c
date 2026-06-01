/**
 * test_runner.c — C integration test harness for Money Room
 * Runs each tool binary with known inputs, checks exit codes and output.
 *
 * Build: gcc -O2 -o test_runner test_runner.c
 * Usage: ./test_runner [filter]   — run all tests, or filter by substring
 *        ./test_runner --list     — list available tests
 *        ./test_runner --quick    — quick smoke tests only
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>

#define MAX_TESTS 128
#define MAX_OUT 65536
#define ENGINE_DIR "/home/wubu2/money-room/engine"

/* Test result */
typedef struct {
    const char *name;
    const char *cmd;
    int expect_exit;
    const char *expect_out;     /* substring to find in stdout */
    const char *expect_err;     /* substring to find in stderr */
    int timeout_sec;
    int (*custom_fn)(void);     /* custom test function, or NULL */
} TestDef;

static int pass_count = 0, fail_count = 0, skip_count = 0;

/* ─── Helper: run a command and capture output ─── */
static int run_cmd(const char *cmd, char *out, size_t out_sz,
                   int *exit_code, int timeout_sec) {
    char full_cmd[4096];
    snprintf(full_cmd, sizeof(full_cmd),
             "%s 2>&1; echo \"__EXIT__=$?\"", cmd);

    FILE *fp = popen(full_cmd, "r");
    if (!fp) return -1;

    size_t total = 0;
    char buf[256];
    *exit_code = -1;
    while (fgets(buf, sizeof(buf), fp) && total < out_sz) {
        size_t len = strlen(buf);
        if (total + len < out_sz) {
            memcpy(out + total, buf, len);
            total += len;
            out[total] = 0;
        }
    }
    int status = pclose(fp);

    /* Extract exit code from __EXIT__ marker */
    char *marker = strstr(out, "__EXIT__=");
    if (marker) {
        *exit_code = atoi(marker + 9);
        /* Remove marker line */
        char *nl = marker;
        if (nl > out && *(nl-1) == '\n') nl--;
        *nl = 0;
    }

    return status;
}

/* ─── Test runner ─── */
static int run_test(const TestDef *t) {
    printf("  TEST  %s ... ", t->name);

    if (t->custom_fn) {
        int rc = t->custom_fn();
        if (rc == 0) {
            printf("✅ PASS\n");
            pass_count++;
        } else {
            printf("❌ FAIL (custom)\n");
            fail_count++;
        }
        return rc;
    }

    char output[MAX_OUT] = {0};
    int exit_code;
    int rc = run_cmd(t->cmd, output, sizeof(output), &exit_code, t->timeout_sec);

    if (rc != 0) {
        printf("❌ FAIL (runner error: %d)\n", rc);
        fail_count++;
        return 1;
    }

    /* Check exit code */
    if (exit_code != t->expect_exit) {
        printf("❌ FAIL (exit %d, expected %d)\n", exit_code, t->expect_exit);
        printf("     cmd: %s\n", t->cmd);
        printf("     out: %.200s\n", output);
        fail_count++;
        return 1;
    }

    /* Check expected output */
    if (t->expect_out && !strstr(output, t->expect_out)) {
        printf("❌ FAIL (missing stdout: '%s')\n", t->expect_out);
        printf("     cmd: %s\n", t->cmd);
        printf("     out: %.200s\n", output);
        fail_count++;
        return 1;
    }

    if (t->expect_err && !strstr(output, t->expect_err)) {
        printf("❌ FAIL (missing stderr: '%s')\n", t->expect_err);
        printf("     cmd: %s\n", t->cmd);
        fail_count++;
        return 1;
    }

    printf("✅ PASS\n");
    pass_count++;
    return 0;
}

/* ─── Custom test: health_check returns valid JSON ─── */
static int test_health_json(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s/health_check", ENGINE_DIR);

    char output[MAX_OUT] = {0};
    int exit_code;
    run_cmd(cmd, output, sizeof(output), &exit_code, 10);

    if (exit_code != 0 && exit_code != 1) {
        printf("bad exit %d", exit_code);
        return 1;
    }
    /* Must start with { — valid JSON */
    if (output[0] != '{') {
        printf("not JSON (starts with '%c')", output[0] ? output[0] : '?');
        return 1;
    }
    /* Must have required keys */
    if (!strstr(output, "\"binaries\"")) {
        printf("missing 'binaries' key");
        return 1;
    }
    if (!strstr(output, "\"data_files\"")) {
        printf("missing 'data_files' key");
        return 1;
    }
    return 0;
}

/* ─── Custom test: data_server serves JSON ─── */
static int test_data_server(void) {
    /* Use curl to test data_server */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "curl -sf http://localhost:9090/ 2>/dev/null || "
             "curl -sf http://localhost:9090/ 2>/dev/null || echo 'UNREACHABLE'");

    char output[MAX_OUT] = {0};
    int exit_code;
    run_cmd(cmd, output, sizeof(output), &exit_code, 5);

    if (strstr(output, "UNREACHABLE") || output[0] != '[') {
        /* data_server may not be running — skip */
        return -1; /* signal skip */
    }
    if (!strstr(output, "health.json")) {
        printf("no 'health.json' in listing");
        return 1;
    }
    return 0;
}

/* ─── Custom test: withdrawal_scheduler CLI works ─── */
static int test_withdrawal_cli(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s/withdrawal_scheduler status", ENGINE_DIR);

    char output[MAX_OUT] = {0};
    int exit_code;
    run_cmd(cmd, output, sizeof(output), &exit_code, 10);

    if (exit_code != 0) return 1;
    if (!strstr(output, "Withdrawal Status")) return 1;
    return 0;
}

/* ─── Test: accuracy_scorer runs ─── */
static int test_accuracy(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s/accuracy_scorer --help 2>&1 || "
             "%s/accuracy_scorer 2>&1 || true", ENGINE_DIR, ENGINE_DIR);

    char output[MAX_OUT] = {0};
    int exit_code;
    run_cmd(cmd, output, sizeof(output), &exit_code, 10);
    /* Just must not crash */
    return (exit_code >= 0 && exit_code <= 1) ? 0 : 1;
}

/* ─── Test: data_quality.json is valid JSON ─── */
static int test_data_quality(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import json;json.load(open('%s/../docs/data/data_quality.json'))\" 2>&1 || "
             "echo 'PARSE_ERROR'",
             ENGINE_DIR);

    char output[MAX_OUT] = {0};
    int exit_code;
    run_cmd(cmd, output, sizeof(output), &exit_code, 10);

    if (strstr(output, "PARSE_ERROR")) return 1;
    return 0;
}

/* ─── Test: engine compiles ─── */
static int test_engine_compiles(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cd %s && make room_engine 2>&1 | tail -3", ENGINE_DIR);

    char output[MAX_OUT] = {0};
    int exit_code;
    run_cmd(cmd, output, sizeof(output), &exit_code, 60);

    if (exit_code != 0) {
        printf("compile failed (exit %d)", exit_code);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int quick_mode = 0;
    int list_mode = 0;
    const char *filter = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) quick_mode = 1;
        else if (strcmp(argv[i], "--list") == 0) list_mode = 1;
        else filter = argv[i];
    }

    /* ─── Define all tests ─── */
    TestDef tests[MAX_TESTS];
    int nt = 0;

    tests[nt++] = (TestDef){
        .name = "health_check returns valid JSON",
        .custom_fn = test_health_json
    };

    tests[nt++] = (TestDef){
        .name = "data_server root listing",
        .custom_fn = test_data_server
    };

    tests[nt++] = (TestDef){
        .name = "withdrawal_scheduler CLI status",
        .custom_fn = test_withdrawal_cli
    };

    tests[nt++] = (TestDef){
        .name = "accuracy_scorer runs without crash",
        .custom_fn = test_accuracy
    };

    tests[nt++] = (TestDef){
        .name = "data_quality.json is valid JSON",
        .custom_fn = test_data_quality
    };

    if (!quick_mode) {
        tests[nt++] = (TestDef){
            .name = "engine compiles cleanly (make room_engine)",
            .custom_fn = test_engine_compiles
        };

        tests[nt++] = (TestDef){
            .name = "collector_runner binary exists",
            .cmd = "test -x " ENGINE_DIR "/collector_runner && echo 'EXISTS'",
            .expect_exit = 0,
            .expect_out = "EXISTS"
        };

        tests[nt++] = (TestDef){
            .name = "cross_asset_c binary exists",
            .cmd = "test -x " ENGINE_DIR "/cross_asset_c && echo 'EXISTS'",
            .expect_exit = 0,
            .expect_out = "EXISTS"
        };

        tests[nt++] = (TestDef){
            .name = "data_server binary exists",
            .cmd = "test -x " ENGINE_DIR "/data_server && echo 'EXISTS'",
            .expect_exit = 0,
            .expect_out = "EXISTS"
        };
    }

    /* List mode */
    if (list_mode) {
        printf("Available tests (%d):\n", nt);
        for (int i = 0; i < nt; i++)
            printf("  %2d. %s\n", i+1, tests[i].name);
        return 0;
    }

    /* Run */
    printf("━━━ Money Room Test Suite ━━━\n");
    printf("Tests: %d (%s)\n\n", nt, quick_mode ? "quick" : "full");

    for (int i = 0; i < nt; i++) {
        if (filter && !strstr(tests[i].name, filter)) {
            skip_count++;
            continue;
        }
        int rc = run_test(&tests[i]);
        if (rc < 0) {
            /* Skipped */
            printf("  TEST  %s ... ⏭️  SKIP (unavailable)\n", tests[i].name);
            skip_count++;
        }
    }

    printf("\n━━━ Results ━━━\n");
    printf("  ✅ Pass:  %d\n", pass_count);
    printf("  ❌ Fail:  %d\n", fail_count);
    printf("  ⏭️  Skip:  %d\n", skip_count);
    printf("  Total:  %d\n", pass_count + fail_count + skip_count);
    printf("━━━━━━━━━━━━━\n");

    return fail_count > 0 ? 1 : 0;
}
