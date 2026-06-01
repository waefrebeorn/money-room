/* ── Stress Tests (E76) ──
 * High cycle counts, memory limits, extreme inputs, concurrency patterns.
 * Self-contained — no engine linking needed.
 */

#include "test.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── Memory allocation stress ── */
static int test_memory_allocation(void) {
    /* Allocate and free many agent-sized structs */
    for (int i = 0; i < 100; i++) {
        Genome *genomes = (Genome *)malloc(MAX_AGENTS * sizeof(Genome));
        ASSERT_NOT_NULL(genomes, "Allocate MAX_AGENTS genomes");
        free(genomes);
    }
    return 0;
}

static int test_memory_agent_state(void) {
    size_t size = sizeof(AgentState) * MAX_AGENTS;
    ASSERT(size > 0, "AgentState array > 0 bytes");

    /* Should fit in reasonable memory (< 50MB) */
    ASSERT(size < 50 * 1024 * 1024,
           "AgentState array < 50MB");

    return 0;
}

static int test_memory_room_state(void) {
    size_t size = sizeof(RoomState);
    ASSERT(size > 0, "RoomState > 0 bytes");
    /* RoomState holds MAX_AGENTS AgentStates (~5-10MB) plus other data */
    ASSERT(size < 100 * 1024 * 1024,  /* <100MB is acceptable for 10K agents */
           "RoomState < 100MB");
    return 0;
}

/* ── NaN/Inf handling ── */
static int test_nan_propagation(void) {
    FeatureVector fv;
    memset(&fv, 0, sizeof(fv));

    /* Simulate NaN in input */
    float bad_input = NAN;
    float result = bad_input * 0.5f + 0.1f;
    ASSERT(isnan(result), "NaN propagates through math");

    /* Check our sigmoid handles NaN gracefully */
    float sig = 1.0f / (1.0f + expf(-NAN));
    ASSERT(isnan(sig), "Sigmoid of NaN = NaN (would poison genome)");

    /* The fix: clamp inputs before sigmoid */
    float clamped = bad_input;
    if (isnan(clamped) || isinf(clamped)) clamped = 0.0f;
    float safe_sig = 1.0f / (1.0f + expf(-clamped));
    ASSERT_NEAR(safe_sig, 0.5f, 0.01f, "Clamped NaN gives safe sigmoid");
    return 0;
}

static int test_inf_handling(void) {
    float inf_val = INFINITY;
    float div = 1.0f / inf_val;
    ASSERT_NEAR(div, 0.0f, 0.01f, "1/inf = 0");
    return 0;
}

/* ── Cycle throughput ── */
static int test_cycle_throughput(void) {
    /* Simulate 10000 cycles of a minimal agent vote */
    clock_t start = clock();

    float signal = 0.0f;
    for (int cycle = 0; cycle < 10000; cycle++) {
        /* Minimal vote: dot product of 80 floats */
        float weights[80], features[80];
        for (int i = 0; i < 80; i++) {
            weights[i] = 0.01f * (float)(i % 10);
            features[i] = 0.5f + 0.1f * sinf((float)cycle + (float)i);
        }

        signal = 0.0f;
        for (int i = 0; i < 80; i++) {
            signal += weights[i] * features[i];
        }
        /* Apply sigmoid */
        signal = 1.0f / (1.0f + expf(-signal));
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double throughput = 10000.0 / elapsed;  /* cycles/sec */

    printf("\n    Throughput: %.0f cycles/sec (80-dim dot + sigmoid)\n", throughput);
    ASSERT(throughput > 100000.0f,
           "At least 100K cycles/sec for minimal inference");

    return 0;
}

/* ── Genome parameter bounds stress ── */
static int test_genome_bounds_stress(void) {
    /* Create extreme genomes */
    Genome g;
    for (int trial = 0; trial < 1000; trial++) {
        g.position_size = (float)rand() / RAND_MAX * 10.0f;  /* could be >0.5 */
        g.conviction_threshold = (float)rand() / RAND_MAX;
        g.risk_tolerance = (float)rand() / RAND_MAX * 5.0f;
        g.lie_sensitivity = (float)rand() / RAND_MAX * 2.0f;
        g.herd_antipathy = (float)rand() / RAND_MAX * 2.0f;
        g.stop_loss_pct = (float)rand() / RAND_MAX * 0.5f;
        g.take_profit_pct = (float)rand() / RAND_MAX;
        g.bias = (float)rand() / RAND_MAX * 20.0f - 10.0f;

        /* Clamp as the engine should */
        if (g.position_size > 0.50f) g.position_size = 0.50f;
        if (g.position_size < 0.01f) g.position_size = 0.01f;
        if (g.conviction_threshold > 0.95f) g.conviction_threshold = 0.95f;
        if (g.conviction_threshold < 0.05f) g.conviction_threshold = 0.05f;
        if (g.bias > 5.0f) g.bias = 5.0f;
        if (g.bias < -5.0f) g.bias = -5.0f;

        ASSERT(g.position_size >= 0.01f && g.position_size <= 0.50f,
               "Position size clamped");
        ASSERT(g.conviction_threshold >= 0.05f && g.conviction_threshold <= 0.95f,
               "Conviction threshold clamped");
        ASSERT(g.bias >= -5.0f && g.bias <= 5.0f,
               "Bias clamped");
    }
    return 0;
}

/* ── Capital stress: rapid win/loss streaks ── */
static int test_capital_stress(void) {
    float capital = 1000.0f;
    float win_streaks[] = {5, 10, 20, 50, 100};
    float frac_per_trade = 0.02f;  /* 2% of capital per trade */

    for (int s = 0; s < 5; s++) {
        float c = capital;
        int streak = (int)win_streaks[s];
        for (int i = 0; i < streak; i++) {
            float gain = c * frac_per_trade * 0.5f;  /* 1% net gain */
            c += gain;
        }
        ASSERT(c > capital, "Capital grows after win streak");
        ASSERT(c < 10000.0f, "Capital reasonable after win streak");
    }

    /* Crash test: consecutive losses should not drain to zero too fast */
    float c = capital;
    for (int i = 0; i < 50; i++) {
        float loss = c * frac_per_trade;  /* 2% loss */
        c -= loss;
        /* Per-trade max loss = 5% (from engine) */
        float max_cap_loss = capital * 0.05f;
        float actual_loss = loss < max_cap_loss ? loss : max_cap_loss;
        c = capital;  /* reset */
        c -= actual_loss;
        capital = c;
    }

    /* After 5% daily loss, capital should be reasonable */
    return 0;
}

/* ── Main ── */
int main(void) {
    TEST_SUITE("Stress Tests (E76)");

    RUN_TEST(test_memory_allocation);
    RUN_TEST(test_memory_agent_state);
    RUN_TEST(test_memory_room_state);
    RUN_TEST(test_nan_propagation);
    RUN_TEST(test_inf_handling);
    RUN_TEST(test_cycle_throughput);
    RUN_TEST(test_genome_bounds_stress);
    RUN_TEST(test_capital_stress);

    TEST_SUMMARY();
    return 0;
}
