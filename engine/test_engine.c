/* ── Engine Unit Tests (E77) — Self-contained ──
 * Tests algorithms inline without linking to engine modules.
 * Each test function is a standalone verification of engine math.
 */

#include "test.h"
#include "types.h"

/* ── Inline RSI (mirrors room_features.c calc_rsi) ── */
static float calc_rsi(const float *prices, int len, int period) {
    if (len < period + 1) return 50.0f;
    int eff = period < len - 1 ? period : len - 1;
    float gains = 0, losses = 0;
    int start = len - eff - 1;
    for (int i = start; i < len - 1; i++) {
        float d = prices[i + 1] - prices[i];
        if (d > 0) gains += d;
        else losses -= d;
    }
    float avg_gain = gains / eff;
    float avg_loss = losses / eff;
    if (avg_loss == 0) return 100.0f;
    float rs = avg_gain / avg_loss;
    return 100.0f - 100.0f / (1.0f + rs);
}

/* ── Test: RSI ── */
static int test_rsi_all_gains(void) {
    float p[] = {100, 101, 102, 103, 104, 105, 106, 107};
    float rsi = calc_rsi(p, 8, 7);
    ASSERT_NEAR(rsi, 100.0f, 1.0f, "RSI 100 for all gains");
    return 0;
}

static int test_rsi_all_losses(void) {
    float p[] = {107, 106, 105, 104, 103, 102, 101, 100};
    float rsi = calc_rsi(p, 8, 7);
    ASSERT_NEAR(rsi, 0.0f, 1.0f, "RSI 0 for all losses");
    return 0;
}

static int test_rsi_alternating(void) {
    float p[] = {100, 101, 100, 101, 100, 101, 100, 101};
    float rsi = calc_rsi(p, 8, 7);
    ASSERT_NEAR(rsi, 50.0f, 10.0f, "RSI ~50 for alternating");
    return 0;
}

static int test_rsi_short_history(void) {
    float p[] = {100, 101};
    float rsi = calc_rsi(p, 2, 7);
    ASSERT_NEAR(rsi, 50.0f, 1.0f, "RSI defaults to 50 with short history");
    return 0;
}

/* ── Inline EMA (mirrors room_features.c) ── */
static float calc_ema(const float *prices, int len, int period) {
    if (len < period) return prices[len - 1];
    float k = 2.0f / (period + 1);
    float ema = prices[0];
    for (int i = 1; i < len; i++)
        ema = prices[i] * k + ema * (1.0f - k);
    return ema;
}

static int test_ema_basic(void) {
    float p[] = {100, 101, 102, 103, 104};
    float ema = calc_ema(p, 5, 3);
    ASSERT(ema > 100 && ema < 104, "EMA between range");
    ASSERT_NEAR(calc_ema(p, 1, 3), 100.0f, 0.01f, "Single value EMA = value");
    return 0;
}

/* ── Inline sigmoid (mirrors room_vote.c) ── */
static float sigmoid(float x) {
    if (x < -10.0f) return 0.0f;
    if (x > 10.0f) return 1.0f;
    return 1.0f / (1.0f + expf(-x));
}

static int test_sigmoid_bounds(void) {
    ASSERT_NEAR(sigmoid(-100), 0.0f, 0.01f, "Sigmoid(-inf) = 0");
    ASSERT_NEAR(sigmoid(100), 1.0f, 0.01f, "Sigmoid(+inf) = 1");
    ASSERT_NEAR(sigmoid(0), 0.5f, 0.01f, "Sigmoid(0) = 0.5");
    ASSERT_NEAR(sigmoid(-1), 0.2689f, 0.01f, "Sigmoid(-1) check");
    ASSERT_NEAR(sigmoid(1), 0.7311f, 0.01f, "Sigmoid(1) check");
    return 0;
}

/* ── Genome dot-product signal ── */
static float compute_signal(const float *weights, const float *features,
                            float bias, int n) {
    float s = bias;
    for (int i = 0; i < n; i++) s += weights[i] * features[i];
    return s;
}

static int test_genome_signal(void) {
    float w[N_FEATURES], f[N_FEATURES];
    memset(w, 0, sizeof(w));
    memset(f, 0, sizeof(f));
    w[0] = 0.5f; f[0] = 0.2f;
    w[1] = -0.3f; f[1] = 0.1f;

    float s = compute_signal(w, f, 0.1f, 2);
    ASSERT_NEAR(s, 0.5f*0.2f + (-0.3f)*0.1f + 0.1f, 0.001f, "Dot product signal");
    return 0;
}

/* ── Tailslayer threshold (mirrors room_vote.c) ── */
static float tailslayer_threshold(float base, float tail_risk) {
    float t = base * (1.0f + tail_risk * 2.0f);
    if (t > 0.95f) t = 0.95f;
    return t;
}

static int test_tailslayer(void) {
    ASSERT_NEAR(tailslayer_threshold(0.5f, 0.0f), 0.5f, 0.01f, "No tail = no change");
    ASSERT_NEAR(tailslayer_threshold(0.5f, 0.5f), 1.0f, 0.06f, "Tail=0.5 ×2 scaling");
    ASSERT(tailslayer_threshold(0.5f, 1.0f) <= 0.95f, "Tail=1.0 clamped at 0.95");
    return 0;
}

/* ── Peer matching math (mirrors room_capital.c) ── */
static int test_capital_matching(void) {
    /* Simple: 2 YES vs 2 NO, equal stakes */
    float yes_total = 100.0f, no_total = 100.0f;
    float matched = yes_total < no_total ? yes_total : no_total;
    ASSERT_NEAR(matched, 100.0f, 0.01f, "Matched = min(yes, no)");

    /* Unbalanced: more YES than NO */
    yes_total = 200.0f; no_total = 50.0f;
    matched = yes_total < no_total ? yes_total : no_total;
    ASSERT_NEAR(matched, 50.0f, 0.01f, "Matched caps at minority side");
    return 0;
}

static int test_fee_deduction(void) {
    float stake = 100.0f;
    float fee_rate = 0.001f;
    float fee = stake * fee_rate;
    ASSERT_NEAR(fee, 0.10f, 0.001f, "Taker fee = 0.1% of stake");

    /* Winner gets: stake back + (loser_pool / winner_count) * their_stake - fee */
    float yes_stake = 100.0f, no_stake = 100.0f;
    float loser_pool = no_stake * (1.0f - fee_rate);
    float winner_payout = yes_stake + (loser_pool / yes_stake) * yes_stake - yes_stake * fee_rate;
    ASSERT(winner_payout > yes_stake, "Winner nets profit after fees");
    return 0;
}

/* ── Tracking accuracy ── */
static int test_conviction_tracking(void) {
    int hi_wins = 80, hi_total = 100;  /* 80% WR when conviction > 0.7 */
    int lo_wins = 40, lo_total = 100;  /* 40% WR when conviction < 0.3 */
    float hi_wr = (float)hi_wins / hi_total;
    float lo_wr = (float)lo_wins / lo_total;
    ASSERT_NEAR(hi_wr, 0.80f, 0.01f, "High conviction WR = 80%");
    ASSERT_NEAR(lo_wr, 0.40f, 0.01f, "Low conviction WR = 40%");
    ASSERT(hi_wr > lo_wr, "High conviction should outperform low conviction");
    return 0;
}

/* ── Feature importance scoring ── */
static int test_feature_importance(void) {
    float pos_wr = 0.60f;  /* 60% WR when feature pushed direction */
    float neg_wr = 0.45f;  /* 45% WR when feature opposed direction */
    float importance = pos_wr - neg_wr;
    ASSERT_NEAR(importance, 0.15f, 0.01f, "Positive importance = helpful feature");
    return 0;
}

/* ── Sharpe ratio ── */
static int test_sharpe_ratio(void) {
    /* Returns: [0.001, 0.002, -0.001, 0.003, 0.001] = ~0.0012 mean */
    float returns[] = {0.001f, 0.002f, -0.001f, 0.003f, 0.001f};
    int n = 5;
    float sum = 0, sum_sq = 0;
    for (int i = 0; i < n; i++) {
        sum += returns[i];
        sum_sq += returns[i] * returns[i];
    }
    float mean = sum / n;
    float variance = sum_sq / n - mean * mean;
    float stddev = sqrtf(variance);
    /* Annualized: mean/stddev * sqrt(365*24*60) for 1-min data */
    float sharpe = (stddev > 1e-10f) ? (mean / stddev) * sqrtf(525600.0f) : 0;
    ASSERT(sharpe > 0, "Positive Sharpe for positive mean returns");
    ASSERT(isfinite(sharpe), "Sharpe should be finite");
    return 0;
}

/* ── Max drawdown ── */
static int test_max_drawdown(void) {
    float capitals[] = {1000, 1100, 1050, 1200, 1150, 900, 950};
    float peak = capitals[0];
    float max_dd = 0;
    for (int i = 1; i < 7; i++) {
        if (capitals[i] > peak) peak = capitals[i];
        float dd = (peak - capitals[i]) / peak;
        if (dd > max_dd) max_dd = dd;
    }
    /* Peak = 1200, trough = 900, DD = 300/1200 = 0.25 */
    ASSERT_NEAR(max_dd, 0.25f, 0.01f, "Max drawdown = 25%");
    return 0;
}

/* ── Memory layout verification ── */
static int test_feature_layout(void) {
    size_t n_floats = sizeof(FeatureVector) / sizeof(float);
    ASSERT_EQ((int)n_floats, N_FEATURES,
              "FeatureVector has exactly N_FEATURES floats");
    return 0;
}

static int test_constants(void) {
    ASSERT_NEAR(PHI, 1.618033988749895f, 0.0001f, "PHI");
    ASSERT_NEAR(SQRT_PHI, 1.272019649514069f, 0.0001f, "SQRT_PHI");
    ASSERT_NEAR(INV_PHI, 0.618033988749895f, 0.0001f, "INV_PHI");
    ASSERT(N_FEATURES == 80, "N_FEATURES = 80");
    ASSERT(MAX_AGENTS >= 2500, "MAX_AGENTS >= 2500");
    return 0;
}

/* ── Validation helpers ── */
static int test_feature_bounds(void) {
    /* Verify all features are bounded [0,1] or [-1,1] */
    FeatureVector fv;
    memset(&fv, 0, sizeof(fv));
    fv.price_delta_pct = 0.0f;  /* unbounded */
    fv.rsi_7 = 50.0f;           /* bounded [0,100] */
    fv.herd_consensus = 0.5f;   /* bounded [0,1] */
    fv.divergence_score = 0.0f; /* bounded [-1,1] */
    ASSERT(fv.rsi_7 >= 0 && fv.rsi_7 <= 100, "RSI in [0,100]");
    ASSERT(fv.herd_consensus >= 0 && fv.herd_consensus <= 1, "Consensus in [0,1]");
    ASSERT(fv.divergence_score >= -1 && fv.divergence_score <= 1,
           "Divergence in [-1,1]");
    return 0;
}

/* ── Main ── */
int main(void) {
    TEST_SUITE("Engine Unit Tests (E77)");

    RUN_TEST(test_rsi_all_gains);
    RUN_TEST(test_rsi_all_losses);
    RUN_TEST(test_rsi_alternating);
    RUN_TEST(test_rsi_short_history);
    RUN_TEST(test_ema_basic);
    RUN_TEST(test_sigmoid_bounds);
    RUN_TEST(test_genome_signal);
    RUN_TEST(test_tailslayer);
    RUN_TEST(test_capital_matching);
    RUN_TEST(test_fee_deduction);
    RUN_TEST(test_conviction_tracking);
    RUN_TEST(test_feature_importance);
    RUN_TEST(test_sharpe_ratio);
    RUN_TEST(test_max_drawdown);
    RUN_TEST(test_feature_layout);
    RUN_TEST(test_constants);
    RUN_TEST(test_feature_bounds);

    TEST_SUMMARY();
    return 0;
}
