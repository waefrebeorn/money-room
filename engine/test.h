#ifndef TEST_H
#define TEST_H

/* ── Lightweight C test framework ──
 * No external deps. Compile with -DTEST_MAIN={testfile} for self-test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* Colors for TAP-like output */
#define T_GREEN  "\033[32m"
#define T_RED    "\033[31m"
#define T_YELLOW "\033[33m"
#define T_CYAN   "\033[36m"
#define T_RESET  "\033[0m"

/* Global counters */
static int test_passed = 0;
static int test_failed = 0;
static int test_skipped = 0;
static int test_assertions = 0;
static char test_suite[256] = "";

/* Start a test suite */
#define TEST_SUITE(name) do { \
    snprintf(test_suite, sizeof(test_suite), "%s", name); \
    printf("\n" T_CYAN "── %s ──" T_RESET "\n", name); \
} while (0)

/* Assertions */
#define ASSERT(cond, msg) do { \
    test_assertions++; \
    if (!(cond)) { \
        printf(T_RED "  ✗ %s:%d: %s" T_RESET "\n", __FILE__, __LINE__, msg); \
        test_failed++; \
        return -1; \
    } \
} while (0)

#define ASSERT_EQ(a, b, msg) do { \
    test_assertions++; \
    if ((a) != (b)) { \
        printf(T_RED "  ✗ %s:%d: %s — expected %lld, got %lld" T_RESET "\n", \
               __FILE__, __LINE__, msg, (long long)(b), (long long)(a)); \
        test_failed++; \
        return -1; \
    } \
} while (0)

#define ASSERT_NEAR(a, b, eps, msg) do { \
    test_assertions++; \
    float _diff = fabsf((float)(a) - (float)(b)); \
    if (_diff > (eps)) { \
        printf(T_RED "  ✗ %s:%d: %s — expected %.6f ± %.6f, got %.6f" T_RESET "\n", \
               __FILE__, __LINE__, msg, (double)(b), (double)(eps), (double)(a)); \
        test_failed++; \
        return -1; \
    } \
} while (0)

#define ASSERT_STREQ(a, b, msg) do { \
    test_assertions++; \
    if (strcmp((a), (b)) != 0) { \
        printf(T_RED "  ✗ %s:%d: %s — expected '%s', got '%s'" T_RESET "\n", \
               __FILE__, __LINE__, msg, (b), (a)); \
        test_failed++; \
        return -1; \
    } \
} while (0)

#define ASSERT_NULL(ptr, msg) do { \
    test_assertions++; \
    if ((ptr) != NULL) { \
        printf(T_RED "  ✗ %s:%d: %s — expected NULL" T_RESET "\n", \
               __FILE__, __LINE__, msg); \
        test_failed++; \
        return -1; \
    } \
} while (0)

#define ASSERT_NOT_NULL(ptr, msg) do { \
    test_assertions++; \
    if ((ptr) == NULL) { \
        printf(T_RED "  ✗ %s:%d: %s — expected non-NULL" T_RESET "\n", \
               __FILE__, __LINE__, msg); \
        test_failed++; \
        return -1; \
    } \
} while (0)

/* Run a test function */
#define RUN_TEST(fn) do { \
    printf("  · " #fn " ... "); \
    fflush(stdout); \
    int _r = fn(); \
    if (_r == 0) { \
        printf(T_GREEN "PASS" T_RESET "\n"); \
        test_passed++; \
    } else if (_r == -2) { \
        printf(T_YELLOW "SKIP" T_RESET "\n"); \
        test_skipped++; \
    } else { \
        test_failed++; \
    } \
} while (0)

/* Skip a test (not applicable in this environment) */
#define TEST_SKIP() do { return -2; } while (0)

/* Final summary */
#define TEST_SUMMARY() do { \
    printf("\n" T_CYAN "══════════════════════════════════════" T_RESET "\n"); \
    printf("  Suite: %s\n", test_suite[0] ? test_suite : "(unnamed)"); \
    printf("  Assertions: %d\n", test_assertions); \
    printf("  " T_GREEN "Passed: %d" T_RESET, test_passed); \
    if (test_failed > 0) printf("  " T_RED "Failed: %d" T_RESET, test_failed); \
    if (test_skipped > 0) printf("  " T_YELLOW "Skipped: %d" T_RESET, test_skipped); \
    printf("\n"); \
    printf(T_CYAN "══════════════════════════════════════" T_RESET "\n"); \
    exit(test_failed > 0 ? 1 : 0); \
} while (0)

#endif /* TEST_H */
