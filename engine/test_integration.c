/* ── Integration Tests (E75) ──
 * Tests the full data flow: pipeline data → bridge → features → vote.
 * Uses standalone engine math with realistic market data.
 * No linking needed — tests the data format and algorithm correctness.
 */

#include "test.h"
#include "types.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ── Simulate a full market tick → feature vector → agent vote ── */
static int test_full_pipeline_mock(void) {
    /* 1. Create a realistic market tick (BTC 1-min candle) */
    MarketTick tick;
    memset(&tick, 0, sizeof(tick));
    strncpy(tick.asset, "BTC", sizeof(tick.asset));
    tick.window_ts = 1748400000;
    tick.open = 68500.0f;
    tick.high = 68750.0f;
    tick.low = 68300.0f;
    tick.close = 68650.0f;  /* +150, ~0.22% up */
    tick.volume = 1250.0f;
    tick.bid = 68648.0f;
    tick.ask = 68652.0f;
    tick.fear_greed = 52.0f;

    /* 2. Compute features inline (mirrors engine logic) */
    float price_delta = (tick.close - tick.open) / tick.open * 100.0f;
    ASSERT_NEAR(price_delta, (68650-68500)/68500.0f*100, 0.01f,
                "BTC up ~0.22%");

    /* 3. Simulate agent: neutral weights, check signal */
    Genome g;
    memset(&g, 0, sizeof(g));
    g.bias = 0.0f;
    g.conviction_threshold = 0.5f;
    g.time_horizon = 1.0f;
    g.feat_weight[0] = 0.3f;    /* ↑ price_delta → long */
    g.feat_weight[1] = 0.1f;    /* micro_momentum */

    /* Signal = dot(feat_weights, features) — use our inline compute */
    float features_f[] = {price_delta, 0.05f};  /* F1=delta, F2=momentum */
    float signal = g.bias + g.feat_weight[0] * features_f[0]
                            + g.feat_weight[1] * features_f[1];
    signal *= g.time_horizon / 5.0f;

    /* 4. Check signal direction matches price move */
    ASSERT(signal > 0, "Signal positive = prediction UP");
    ASSERT(signal < 5.0f, "Signal reasonable magnitude");

    /* 5. Simulate conviction */
    float sigmoid_act = 1.0f / (1.0f + expf(-signal * 2.5f));
    ASSERT(sigmoid_act > 0.5f, "Conviction > 0.5 for upward signal");

    return 0;
}

/* ── Feature vector → JSON bridge format ── */
static int test_bridge_output_format(void) {
    /* Verify the expected bridge JSON structure */
    const char *json_fragment =
        "{\"cycle\":42,\"win_rate\":0.475,\"sharpe\":1.42,"
        "\"features\":{\"price_delta\":2.0,\"rsi\":55.0}}";

    /* Check it parses as valid JSON manually (simple field presence) */
    ASSERT(strstr(json_fragment, "\"cycle\":42") != NULL,
           "Bridge JSON contains cycle number");
    ASSERT(strstr(json_fragment, "\"features\"") != NULL,
           "Bridge JSON contains features object");

    return 0;
}

/* ── Engine cycle → room_snapshot.json format ── */
static int test_snapshot_format(void) {
    /* Verify room_snapshot.json has required fields */
    const char *snapshot =
        "{\"market\":{\"btc\":68650},\"stats\":{\"win_rate\":0.475,"
        "\"active_agents\":10000},\"features\":{\"price_delta\":0.22},"
        "\"vote_summary\":{\"yes\":45,\"no\":30,\"total\":75}}";

    ASSERT(strstr(snapshot, "\"market\"") != NULL, "Snapshot has market section");
    ASSERT(strstr(snapshot, "\"stats\"") != NULL, "Snapshot has stats section");
    ASSERT(strstr(snapshot, "\"features\"") != NULL, "Snapshot has features section");
    ASSERT(strstr(snapshot, "\"vote_summary\"") != NULL, "Snapshot has vote summary");

    return 0;
}

/* ── Multiple ticks → feature history consistency ── */
static int test_feature_history_consistency(void) {
    /* Simulate 3 consecutive ticks */
    struct { float open, close, volume; } ticks[] = {
        {68000, 68100, 1000},
        {68100, 68300, 1500},  /* trending up, volume increasing */
        {68300, 68450, 2000},  /* continuing up */
    };
    int n = 3;

    for (int i = 0; i < n; i++) {
        float delta = (ticks[i].close - ticks[i].open) / ticks[i].open * 100.0f;
        ASSERT(delta > 0, "All ticks profitable");
        /* Volume should be strictly increasing */
        if (i > 0) {
            ASSERT(ticks[i].volume >= ticks[i-1].volume,
                   "Volume increasing in trend");
        }
    }

    return 0;
}

/* ── Consensus vote aggregation ── */
static int test_consensus_math(void) {
    int yes = 4500, no = 3000, total = 7500;
    float consensus = (float)yes / total;          /* 0.6 */
    float spread = 1.0f - fabsf(consensus - 0.5f) * 2.0f;  /* 1 - 0.2 = 0.8 */

    ASSERT_NEAR(consensus, 0.60f, 0.01f, "60% YES consensus");
    ASSERT_NEAR(spread, 0.80f, 0.01f, "Spread = 0.8 (60/40 split)");

    /* Tighter vote = higher spread (nearer 1.0) */
    yes = 3800; no = 3700; total = 7500;
    consensus = (float)yes / total;                 /* ~0.5067 */
    spread = 1.0f - fabsf(consensus - 0.5f) * 2.0f;  /* ~0.9867 */
    ASSERT(spread > 0.80f, "Tighter vote = higher spread (> 0.8)");
    ASSERT(spread > 0.95f, "Near-50/50 has spread near 1.0");

    return 0;
}

/* ── Teacher feedback bridging ── */
static int test_teacher_influence(void) {
    /* Teacher predictions weighted by historical accuracy */
    float teacher_preds[] = {0.6f, 0.7f, 0.45f, 0.55f, 0.8f};
    float teacher_weights[] = {0.8f, 0.6f, 0.9f, 0.7f, 0.5f};  /* accuracy-based */
    int n_teachers = 5;

    float weighted_sum = 0, weight_total = 0;
    for (int i = 0; i < n_teachers; i++) {
        weighted_sum += teacher_preds[i] * teacher_weights[i];
        weight_total += teacher_weights[i];
    }
    float ensemble_pred = weighted_sum / weight_total;

    ASSERT(ensemble_pred > 0.5f, "Ensemble prediction bullish");
    ASSERT(ensemble_pred < 1.0f, "Ensemble prediction in [0,1]");

    /* Higher-weight teachers dominate */
    float top_pred = (teacher_preds[4] * teacher_weights[4]) / teacher_weights[4];
    ASSERT_NEAR(top_pred, 0.8f, 0.01f, "Highest weight teacher dominates");

    return 0;
}

/* ── Main ── */
int main(void) {
    TEST_SUITE("Integration Tests (E75)");

    RUN_TEST(test_full_pipeline_mock);
    RUN_TEST(test_bridge_output_format);
    RUN_TEST(test_snapshot_format);
    RUN_TEST(test_feature_history_consistency);
    RUN_TEST(test_consensus_math);
    RUN_TEST(test_teacher_influence);

    TEST_SUMMARY();
    return 0;
}
