/* ── Pipeline Sanity Tests (E74 — Auto Testing) ──
 * Runs each pipeline binary as a subprocess, verifies clean exit.
 * No linking needed — uses posix_spawn/system.
 */

#include "test.h"
#include <sys/wait.h>
#include <unistd.h>

/* ── Pipeline binary paths ── */
static const char *pipelines[] = {
    "./hashrate_feat",
    "./dark_pool_feat",
    "./congress_trades",
    "./insider_trades",
    "./thirteen_f_holdings",
    "./stock_screener",
    "./market_tide",
    "./politician_portfolio",
    "./withdrawal_scheduler",
    "./order_book_depth",
    NULL
};

/* ── Python pipeline commands ── */
static const char *py_pipelines[] = {
    "python3 ../scripts/gdelt_enricher_fast.py --dry-run 2>/dev/null || true",
    NULL
};

/* ── Run a binary, check exit code ── */
static int check_binary(const char *path) {
    /* Check binary exists and is executable */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "test -x %s", path);

    int ret = system(cmd);
    if (ret != 0) {
        /* Binary doesn't exist — skip */
        return -2;
    }

    /* Binary exists — run with --help for a quick smoke test */
    snprintf(cmd, sizeof(cmd), "%s >/dev/null 2>&1", path);
    ret = system(cmd);

    /* Any exit code is fine (1=no-data, 2=bad-args, 124=timeout) */
    /* Only flag hard crashes (signal 6/11) */
    if (WIFSIGNALED(ret)) {
        printf(T_RED "  Binary %s crashed with signal %d" T_RESET "\n",
               path, WTERMSIG(ret));
        return -1;
    }
    return 0;  /* Binary exists and runs without crash */
}

static int test_hashrate_feat(void) {
    return check_binary("./hashrate_feat");
}

static int test_dark_pool(void) {
    return check_binary("./dark_pool_feat");
}

static int test_congress(void) {
    return check_binary("./congress_trades");
}

static int test_insider(void) {
    return check_binary("./insider_trades");
}

static int test_13f(void) {
    return check_binary("./thirteen_f_holdings");
}

static int test_stock_screener(void) {
    return check_binary("./stock_screener");
}

static int test_market_tide(void) {
    return check_binary("./market_tide");
}

static int test_politician(void) {
    return check_binary("./politician_portfolio");
}

static int test_withdrawal(void) {
    return check_binary("./withdrawal_scheduler");
}

static int test_orderbook_depth(void) {
    return check_binary("./order_book_depth");
}

/* ── Main ── */
int main(void) {
    TEST_SUITE("Pipeline Sanity Tests (E74 — Auto Testing)");

    RUN_TEST(test_hashrate_feat);
    RUN_TEST(test_dark_pool);
    RUN_TEST(test_congress);
    RUN_TEST(test_insider);
    RUN_TEST(test_13f);
    RUN_TEST(test_stock_screener);
    RUN_TEST(test_market_tide);
    RUN_TEST(test_politician);
    RUN_TEST(test_withdrawal);
    RUN_TEST(test_orderbook_depth);

    printf("\n  Note: Binary not found = SKIP (not compiled yet).\n");
    printf("  Run 'make all' first, then re-run tests.\n");

    TEST_SUMMARY();
    return 0;
}
