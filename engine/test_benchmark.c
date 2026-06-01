/* ── Performance Benchmarks (E79) ──
 * Measures throughput of key engine operations.
 * Outputs timing data to stdout in a parseable format.
 */

#include "test.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── Timing helper ── */
static double time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ── Benchmark: 80-dim dot product ── */
static int bench_dot_product(void) {
    float w[80], f[80];
    for (int i = 0; i < 80; i++) {
        w[i] = 0.01f * (float)(i % 10);
        f[i] = 0.5f + 0.1f * sinf((float)i);
    }

    int iterations = 1000000;
    double start = time_ns();
    volatile float result = 0;
    for (int i = 0; i < iterations; i++) {
        result = 0;
        for (int j = 0; j < 80; j++) {
            result += w[j] * f[j];
        }
    }
    double elapsed_ns = time_ns() - start;
    double ns_per = elapsed_ns / iterations;

    printf("\n    80-dim dot product: %.0f ns/op (%.0fM ops/sec)\n",
           ns_per, 1e9 / ns_per / 1e6);
    ASSERT(ns_per < 500, "Dot product < 500ns/op");
    return 0;
}

/* ── Benchmark: Sigmoid throughput ── */
static int bench_sigmoid(void) {
    int iterations = 1000000;
    double start = time_ns();
    volatile float result = 0;
    for (int i = 0; i < iterations; i++) {
        float x = (float)(i % 200) / 100.0f - 10.0f;
        if (x < -10.0f) result = 0.0f;
        else if (x > 10.0f) result = 1.0f;
        else result = 1.0f / (1.0f + expf(-x));
    }
    double elapsed_ns = time_ns() - start;
    double ns_per = elapsed_ns / iterations;

    printf("\n    Sigmoid: %.0f ns/op (%.0fM ops/sec)\n",
           ns_per, 1e9 / ns_per / 1e6);
    ASSERT(ns_per < 200, "Sigmoid < 200ns/op");
    return 0;
}

/* ── Benchmark: Full genome eval (dot + sigmoid) ── */
static int bench_genome_eval(void) {
    float w[80], f[80];
    for (int i = 0; i < 80; i++) {
        w[i] = 0.01f;
        f[i] = 0.5f;
    }

    int iterations = 100000;
    double start = time_ns();
    volatile float result = 0;
    for (int i = 0; i < iterations; i++) {
        result = 0.0f;
        for (int j = 0; j < 80; j++) result += w[j] * f[j];
        result = 1.0f / (1.0f + expf(-result));
    }
    double elapsed_ns = time_ns() - start;
    double ns_per = elapsed_ns / iterations;

    printf("\n    Genome eval (dot + sigmoid): %.0f ns/genome (%.0fM/sec)\n",
           ns_per, 1e9 / ns_per / 1e6);
    ASSERT(ns_per < 1000, "Genome eval < 1000ns");
    return 0;
}

/* ── Benchmark: 10K agent cycle simulation ── */
static int bench_10k_cycle(void) {
    float w[80], f[80];
    float biases[10000], results[10000];

    for (int i = 0; i < 80; i++) {
        w[i] = 0.01f; f[i] = 0.5f;
    }
    for (int i = 0; i < 10000; i++) {
        biases[i] = (float)(i % 100) / 100.0f - 0.5f;
    }

    int iterations = 100;  /* 100 cycles of 10K agents = 1M evals */
    double start = time_ns();

    for (int cycle = 0; cycle < iterations; cycle++) {
        for (int agent = 0; agent < 10000; agent++) {
            float signal = biases[agent];
            for (int j = 0; j < 80; j++) signal += w[j] * f[j];
            results[agent] = 1.0f / (1.0f + expf(-signal));
        }
    }

    double elapsed_ns = time_ns() - start;
    double per_cycle_ms = elapsed_ns / iterations / 1e6;

    printf("\n    10K agent cycle: %.2f ms/cycle (%.0f agents/sec)\n",
           per_cycle_ms, 10000.0 / (per_cycle_ms / 1000));
    ASSERT(per_cycle_ms < 100, "10K agent cycle < 100ms");

    return 0;
}

/* ── Benchmark: RSI computation ── */
static int bench_rsi(void) {
    float prices[50];
    for (int i = 0; i < 50; i++) prices[i] = 100.0f + sinf((float)i);

    int iterations = 100000;
    double start = time_ns();
    volatile float result = 0;
    for (int i = 0; i < iterations; i++) {
        int len = 50, period = 14;
        int eff = period < len - 1 ? period : len - 1;
        float gains = 0, losses = 0;
        int start_idx = len - eff - 1;
        for (int j = start_idx; j < len - 1; j++) {
            float d = prices[j + 1] - prices[j];
            if (d > 0) gains += d; else losses -= d;
        }
        float avg_gain = gains / eff;
        float avg_loss = losses / eff;
        result = avg_loss == 0 ? 100.0f : 100.0f - 100.0f / (1.0f + avg_gain / avg_loss);
    }
    double elapsed_ns = time_ns() - start;
    double ns_per = elapsed_ns / iterations;

    printf("\n    RSI(14) on 50 bars: %.0f ns/op (%.0fK ops/sec)\n",
           ns_per, 1e6 / ns_per);
    ASSERT(ns_per < 5000, "RSI computation < 5000ns");
    return 0;
}

/* ── Benchmark: Memory copy (simulating mmap load) ── */
static int bench_memory_copy(void) {
    char src[1024*1024], dst[1024*1024];  /* 1MB */
    memset(src, 0x42, sizeof(src));

    int iterations = 1000;
    double start = time_ns();
    for (int i = 0; i < iterations; i++) {
        memcpy(dst, src, sizeof(src));
    }
    double elapsed_ns = time_ns() - start;
    double mb_per_sec = (double)(sizeof(src) * iterations) / (elapsed_ns / 1e9) / (1024*1024);

    printf("\n    Memory copy (1MB): %.0f MB/sec\n", mb_per_sec);
    ASSERT(mb_per_sec > 100.0, "Memory copy > 100 MB/s");
    return 0;
}

/* ── Main ── */
int main(void) {
    TEST_SUITE("Performance Benchmarks (E79)");

    RUN_TEST(bench_dot_product);
    RUN_TEST(bench_sigmoid);
    RUN_TEST(bench_genome_eval);
    RUN_TEST(bench_10k_cycle);
    RUN_TEST(bench_rsi);
    RUN_TEST(bench_memory_copy);

    TEST_SUMMARY();
    return 0;
}
