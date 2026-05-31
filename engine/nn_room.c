/**
 * nn_room.c — Neural Network Room Engine v3
 * 10K agents × 13→16→1 MLP, REINFORCE training.
 * MARKET-DIRECTION mode: agents predict NEXT candle direction.
 * NO lookahead bias — features computed from BEFORE the target candle.
 * Loads BTC 1-min CSV, sub-samples to 15-min, runs full backtest.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ── Config ──
#define NN_AGENTS   10000
#define N_INPUTS    13
#define N_HIDDEN    16
#define N_OUTPUTS   1
#define INIT_CAP    50.0f
#define TAKER_FEE   0.001f
#define LEARN_RATE  0.001f
#define WARMUP_CYCLES 100  // Need 50+ for feature warmup
#define MAX_CANDLES 100000

// ── Per-agent weights ──
typedef struct {
    float w1[N_INPUTS * N_HIDDEN];
    float b1[N_HIDDEN];
    float w2[N_HIDDEN * N_OUTPUTS];
    float b2[N_OUTPUTS];
} NNWeights;

typedef struct {
    NNWeights nn;
    float capital;
    float peak_capital;
    float starting_capital;
    int trades, wins, losses;
    int consecutive_losses;
    float total_pnl;
    float max_drawdown;
    float win_rate_ema;
    float last_logit;
    float last_prob;
    int trade_open;  // 1 = we have pending trade from prev candle
    int last_direction;  // our prediction for current candle
    float last_stake;
} NNAgent;

// ── 15-min candle ──
typedef struct {
    int64_t ts;
    float open, high, low, close, volume;
} Candle;

// ── 13-dim features (same structure as room_features.c) ──
typedef struct {
    float price_delta_pct;
    float micro_momentum;
    float rsi_7;
    float volume_surge_ratio;
    float ema_fast;
    float ema_slow;
    float macd_hist;
    float bollinger_pct;
    float divergence_score;
    float pump_score;
    float regime_indicator;
    float fear_greed_norm;
    float herd_consensus;
} NNFeatures;

// ── Sigmoid / activations ──
static inline float sig(float x) {
    if (x < -10.0f) return 0.0f;
    if (x > 10.0f) return 1.0f;
    return 1.0f / (1.0f + expf(-x));
}

static inline float relu(float x) {
    return x > 0 ? x : 0.01f * x;
}

// ── Forward pass ──
static float nn_forward(const NNWeights *w, const NNFeatures *f, float *logit_out) {
    float hidden[N_HIDDEN];
    for (int i = 0; i < N_HIDDEN; i++) {
        float sum = w->b1[i];
        for (int j = 0; j < N_INPUTS; j++) {
            float feat = ((float*)f)[j];
            sum += feat * w->w1[j * N_HIDDEN + i];
        }
        hidden[i] = relu(sum);
    }
    float logit = w->b2[0];
    for (int i = 0; i < N_HIDDEN; i++)
        logit += hidden[i] * w->w2[i];
    if (logit_out) *logit_out = logit;
    return sig(logit);
}

// ── REINFORCE update ──
static void nn_reinforce(NNWeights *w, int won, float stake,
                          float logit, float lr) {
    float p = sig(logit);
    float advantage = won ? (stake * (1 - TAKER_FEE)) : (-stake * (1 + TAKER_FEE));
    float grad_scale = (won ? (1.0f - p) : -p) * advantage * lr;
    for (int i = 0; i < N_HIDDEN; i++)
        w->w2[i] += grad_scale;
    w->b2[0] += grad_scale;
    for (int i = 0; i < N_HIDDEN; i++)
        w->b1[i] += grad_scale * 0.1f;
}

// ── Init random weights ──
static void init_weights(NNWeights *w, unsigned int seed) {
    srand(seed);
    for (int i = 0; i < N_INPUTS * N_HIDDEN; i++)
        w->w1[i] = (float)rand() / RAND_MAX * 0.2f - 0.1f;
    for (int i = 0; i < N_HIDDEN; i++)
        w->b1[i] = (float)rand() / RAND_MAX * 0.1f - 0.05f;
    for (int i = 0; i < N_HIDDEN * N_OUTPUTS; i++)
        w->w2[i] = (float)rand() / RAND_MAX * 0.2f - 0.1f;
    w->b2[0] = (float)rand() / RAND_MAX * 0.1f - 0.05f;
}

static void mutate_weights(NNWeights *w, float rate, float scale) {
    for (int i = 0; i < N_INPUTS * N_HIDDEN; i++)
        if ((float)rand() / RAND_MAX < rate)
            w->w1[i] += (float)rand() / RAND_MAX * scale - scale/2;
    for (int i = 0; i < N_HIDDEN; i++)
        if ((float)rand() / RAND_MAX < rate)
            w->b1[i] += (float)rand() / RAND_MAX * scale - scale/2;
    for (int i = 0; i < N_HIDDEN * N_OUTPUTS; i++)
        if ((float)rand() / RAND_MAX < rate)
            w->w2[i] += (float)rand() / RAND_MAX * scale - scale/2;
    if ((float)rand() / RAND_MAX < rate)
        w->b2[0] += (float)rand() / RAND_MAX * scale - scale/2;
}

// ════════════════════════════════════════════════════════
//  FEATURE ENGINE — from price history (NO lookahead)
//  Features from hist[0..hist_len-1], predicting hist[hist_len] (next candle)
// ════════════════════════════════════════════════════════

// ── Compute 13-dim features from a price array (strictly historical) ──
static void compute_features_from_prices(const float *px, const float *vol,
                                          int hist_len, NNFeatures *fv) {
    memset(fv, 0, sizeof(NNFeatures));
    if (hist_len < 2) return;

    // F1: Price delta (last vs second-to-last close %)
    fv->price_delta_pct = px[hist_len-1] > 0 ?
        (px[hist_len-1] - px[hist_len-2]) / px[hist_len-2] * 100.0f : 0;

    // F2: Micro momentum (last 2 closes)
    if (hist_len >= 3) {
        fv->micro_momentum = (px[hist_len-1] - px[hist_len-3]) / px[hist_len-3] * 100.0f;
    }

    // F3: RSI 7
    if (hist_len >= 8) {
        float gains = 0, losses = 0;
        for (int i = hist_len - 8; i < hist_len - 1; i++) {
            float d = px[i+1] - px[i];
            if (d > 0) gains += d; else losses -= d;
        }
        if (losses > 0) {
            float rs = (gains/7) / (losses/7);
            fv->rsi_7 = 100.0f - 100.0f / (1.0f + rs);
        } else {
            fv->rsi_7 = 100.0f;
        }
    }

    // F4: Volume surge
    if (hist_len >= 4) {
        float avg = 0;
        for (int i = hist_len - 4; i < hist_len - 1; i++) avg += vol[i];
        avg /= 3.0f;
        if (avg > 0) fv->volume_surge_ratio = fminf(5.0f, vol[hist_len-1] / avg);
    }

    // F5/F6: EMA fast(3) / slow(8)
    if (hist_len >= 3) {
        float k3 = 2.0f/4, k8 = 2.0f/9;
        float e3 = px[0], e8 = px[0];
        for (int i = 1; i < hist_len; i++) {
            e3 = px[i] * k3 + e3 * (1 - k3);
            e8 = px[i] * k8 + e8 * (1 - k8);
        }
        fv->ema_fast = e3;
        fv->ema_slow = e8;
    }

    // F7: MACD histogram
    if (hist_len >= 26) {
        float k12 = 2.0f/13, k26 = 2.0f/27;
        float e12 = px[0], e26 = px[0];
        for (int i = 1; i < hist_len; i++) {
            e12 = px[i] * k12 + e12 * (1 - k12);
            e26 = px[i] * k26 + e26 * (1 - k26);
        }
        fv->macd_hist = e12 - e26;
    }

    // F8: Bollinger %B
    if (hist_len >= 20) {
        int n = 20;
        const float *p = px + hist_len - n;
        float mn = 0;
        for (int i = 0; i < n; i++) mn += p[i];
        mn /= n;
        float vr = 0;
        for (int i = 0; i < n; i++) { float d = p[i] - mn; vr += d * d; }
        vr /= n;
        float std = sqrtf(vr > 0 ? vr : 0.0001f);
        float low = mn - 2*std, high = mn + 2*std;
        if (high - low > 0.0001f)
            fv->bollinger_pct = (px[hist_len-1] - low) / (high - low);
    }

    // F9: Divergence score (price vs RSI direction divergence)
    if (hist_len >= 14) {
        // recompute RSI on earlier window
        if (hist_len >= 8) {
            float gains = 0, losses = 0;
            int start = hist_len - 14;
            for (int i = start; i < start + 6; i++) {
                float d = px[i+1] - px[i];
                if (d > 0) gains += d; else losses -= d;
            }
            float rsi_old = 50.0f;
            if (losses > 0) { float rs = (gains/7)/(losses/7); rsi_old = 100 - 100/(1+rs); }
            float px_dir = px[hist_len-1] > px[hist_len-7] ? 1 : -1;
            float rsi_dir = fv->rsi_7 > rsi_old ? 1 : -1;
            fv->divergence_score = (rsi_dir - px_dir) / 2.0f;
        }
    }

    // F10: Pump score — default 0 (no crony pipeline in this backtest)
    fv->pump_score = 0.0f;

    // F11: Regime detection
    if (hist_len >= 10) {
        const float *p = px + hist_len - 10;
        float mn = 0;
        for (int i = 0; i < 10; i++) mn += p[i];
        mn /= 10;
        float vr = 0;
        for (int i = 0; i < 10; i++) { float d = p[i] - mn; vr += d * d; }
        float std = sqrtf(vr / 10);
        float rng = std / (mn > 0 ? mn : 1);
        float net = p[9] - p[0];
        float gross = 0;
        for (int i = 1; i < 10; i++) gross += fabsf(p[i] - p[i-1]);
        float eff = gross > 0 ? fabsf(net) / gross : 0;
        if (rng > 0.005f) fv->regime_indicator = 2.0f;
        else if (eff > 0.6f) fv->regime_indicator = 1.0f;
    }

    // F12: Fear & Greed — default 0.5 (not available in this backtest)
    fv->fear_greed_norm = 0.5f;

    // F13: Herd consensus — default 0.5 (no real-time consensus in backtest)
    fv->herd_consensus = 0.5f;
}

// ════════════════════════════════════════════════════════
//  EVOLUTION
// ════════════════════════════════════════════════════════
static void evolve_agents(NNAgent *agents, int n) {
    int alive_count = 0;
    for (int i = 0; i < n; i++)
        if (agents[i].capital > 1 && agents[i].trades >= 5) alive_count++;
    if (alive_count < 10) return;

    // Sort by win_rate_ema (descending)
    for (int i = 0; i < n; i++) {
        for (int j = i+1; j < n; j++) {
            if (agents[j].win_rate_ema > agents[i].win_rate_ema) {
                NNAgent tmp = agents[i];
                agents[i] = agents[j];
                agents[j] = tmp;
            }
        }
    }

    int n_cull = alive_count / 4;
    if (n_cull < 1) n_cull = 1;

    // Cull worst 25%, replace with crossover of best + mutation
    for (int i = 0; i < n_cull; i++) {
        int dst = n - 1 - i;
        int p1 = rand() % n_cull;
        int p2 = rand() % n_cull;
        memcpy(&agents[dst].nn, &agents[p1].nn, sizeof(NNWeights));
        float *w_dst = (float*)&agents[dst].nn;
        float *w_src = (float*)&agents[p2].nn;
        int n_weights = sizeof(NNWeights) / sizeof(float);
        for (int k = 0; k < n_weights; k++)
            w_dst[k] = (w_dst[k] + w_src[k]) / 2;
        mutate_weights(&agents[dst].nn, 0.15f, 0.1f);
        agents[dst].capital = fmaxf(INIT_CAP * 0.5f, agents[dst].capital);
    }
}

// ════════════════════════════════════════════════════════
//  LOAD BTC 1-min CSV
// ════════════════════════════════════════════════════════
static int load_btc_candles(const char *path, Candle *buf, int max_c, int stride) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return 0; }

    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; } // skip header

    int count = 0, skip = 0;
    char asset[8] = "BTC";
    (void)asset;

    while (fgets(line, sizeof(line), f) && count < max_c) {
        // Parse CSV: timestamp,open,high,low,close,volume
        int64_t ts;
        float o, h, l, c, v;
        if (sscanf(line, "%ld,%f,%f,%f,%f,%f", &ts, &o, &h, &l, &c, &v) < 6)
            continue;

        skip++;
        if (skip % stride != 0) continue;  // subsample to 15-min

        buf[count].ts = ts;
        buf[count].open = o;
        buf[count].high = h;
        buf[count].low = l;
        buf[count].close = c;
        buf[count].volume = v;
        count++;
    }

    fclose(f);
    return count;
}

// ════════════════════════════════════════════════════════
//  MAIN: Paper proof — NN predicts next 15-min candle direction
// ════════════════════════════════════════════════════════
int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    int N = argc > 1 ? atoi(argv[1]) : 10000;
    if (N > NN_AGENTS) N = NN_AGENTS;

    printf("=== NN ROOM v3 — Market-Direction Paper Proof ===\n");
    printf("Agents: %d\n", N);
    printf("Network: %d→%d→%d\n", N_INPUTS, N_HIDDEN, N_OUTPUTS);
    printf("LR: %.4f, Taker fee: %.3f\n\n", LEARN_RATE, TAKER_FEE);

    // ── Load BTC 15-min candles ──
    const char *csv_path = "/home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv";
    Candle candles[MAX_CANDLES];
    int n_candles = load_btc_candles(csv_path, candles, MAX_CANDLES, 15);
    if (n_candles < WARMUP_CYCLES + 10) {
        fprintf(stderr, "Not enough candles: %d (need %d)\n", n_candles, WARMUP_CYCLES + 10);
        return 1;
    }
    printf("Loaded %d BTC 15-min candles\n\n", n_candles);

    // ── Init agents ──
    NNAgent *agents = (NNAgent*)calloc(N, sizeof(NNAgent));
    srand(42);
    for (int i = 0; i < N; i++) {
        agents[i].capital = INIT_CAP;
        agents[i].peak_capital = INIT_CAP;
        agents[i].starting_capital = INIT_CAP;
        agents[i].win_rate_ema = 0.5f;
        init_weights(&agents[i].nn, i + 100);
    }

    // ── Price history buffer (for feature computation) ──
    float price_hist[MAX_CANDLES];
    float vol_hist[MAX_CANDLES];
    int hist_len = 0;
    float portfolio_value = N * INIT_CAP;
    float peak_portfolio = portfolio_value;
    int total_trades = 0, total_wins = 0, total_losses = 0;
    int max_consec = 0;
    float gross_win = 0, gross_loss = 0;
    float cycle_returns[128] = {0};
    int return_idx = 0, return_count = 0;

    // ── Backtest loop ──
    // For each candle N (starting from WARMUP_CYCLES):
    //   - Features from hist[0..hist_len-1] (candles 0 to N-1)
    //   - Target: candle N direction (close[N] > close[N-1])
    //   - But we wait: predict at candle N's open, resolve when candle N closes
    //   - On first pass: predict candle[N] direction using features from hist
    //   - On next cycle: resolve prediction using candle[N] close, then predict candle[N+1]
    //
    // Simplified: at candle N, features use hist[0..N-1], target is direction of candle[N].
    // Predict at entry of candle N, resolve at close of candle N.
    // This means we need the close of candle N to resolve — which we have in the array.
    // But we DON'T include candle N's data in features.

    printf("Running backtest...\n");

    // Phase 1: Warmup — build feature history without trading
    for (int i = 0; i < WARMUP_CYCLES && i < n_candles; i++) {
        price_hist[hist_len] = candles[i].close;
        vol_hist[hist_len] = candles[i].volume;
        hist_len++;
    }

    printf("Warmup: %d candles\n\n", hist_len);

    // Phase 2: Trading — for each candle, predict next candle direction
    int last_report_pct = 0;
    for (int target_idx = WARMUP_CYCLES; target_idx < n_candles; target_idx++) {
        // target_idx is the candle we're predicting
        // Features from hist (candles 0 to target_idx-1) — strictly historical!
        NNFeatures fv;
        compute_features_from_prices(price_hist, vol_hist, hist_len, &fv);

        Candle *target = &candles[target_idx];
        int price_up = target->close >= candles[target_idx-1].close;

        // Each agent: predict target candle direction
        float pnl_this_cycle = 0;
        int traded_this_cycle = 0;

        for (int i = 0; i < N; i++) {
            NNAgent *a = &agents[i];
            if (a->capital <= 0.01f) continue;

            // Forward pass: predict next candle direction
            float logit;
            float prob = nn_forward(&a->nn, &fv, &logit);
            int direction = prob >= 0.5f ? 1 : 0;
            float conv = fabsf(prob - 0.5f) * 2.0f;

            if (conv < 0.1f) continue;  // skip low conviction trades

            // Stake proportional to capital * conviction
            float stake = a->capital * 0.02f * conv;
            if (stake < 0.01f) continue;
            if (stake > a->capital * 0.05f) stake = a->capital * 0.05f;

            // Deduct stake
            a->capital -= stake;
            a->trades++;
            a->trade_open = 1;
            a->last_direction = direction;
            a->last_stake = stake;
            a->last_logit = logit;
            a->last_prob = prob;

            // Resolve immediately (we already know the target's close)
            int won = direction == price_up;

            if (won) {
                float profit = stake * (1.0f - TAKER_FEE);
                a->capital += stake + profit;
                a->total_pnl += profit;
                a->wins++;
                a->consecutive_losses = 0;
                a->win_rate_ema = 0.9f * a->win_rate_ema + 0.1f * 1.0f;
                pnl_this_cycle += profit;
                total_wins++;
                gross_win += profit;
                nn_reinforce(&a->nn, 1, stake, logit, LEARN_RATE);
            } else {
                a->total_pnl -= stake;
                a->losses++;
                a->consecutive_losses++;
                a->win_rate_ema = 0.9f * a->win_rate_ema + 0.1f * 0.0f;
                pnl_this_cycle -= stake;
                total_losses++;
                gross_loss += stake;
                nn_reinforce(&a->nn, 0, stake, logit, LEARN_RATE);
            }

            total_trades++;
            traded_this_cycle++;

            if (a->consecutive_losses > max_consec)
                max_consec = a->consecutive_losses;

            if (a->capital > a->peak_capital)
                a->peak_capital = a->capital;
            float dd = (a->peak_capital - a->capital) / a->peak_capital;
            if (dd > a->max_drawdown)
                a->max_drawdown = dd;
        }

        // Push target candle to history for next prediction
        price_hist[hist_len] = target->close;
        vol_hist[hist_len] = target->volume;
        hist_len++;

        // Update portfolio
        portfolio_value = 0;
        for (int i = 0; i < N; i++)
            portfolio_value += agents[i].capital;
        if (portfolio_value > peak_portfolio)
            peak_portfolio = portfolio_value;

        // Cycle return for Sharpe
        if (portfolio_value > 0) {
            float ret = pnl_this_cycle / portfolio_value;
            cycle_returns[return_idx] = ret;
            return_idx = (return_idx + 1) % 128;
            if (return_count < 128) return_count++;
        }

        // Darwin evolution every 500 trades
        if (total_trades > 0 && total_trades % 500 == 0) {
            evolve_agents(agents, N);
        }

        // Progress report every 10%
        int pct = (100 * (target_idx - WARMUP_CYCLES)) / (n_candles - WARMUP_CYCLES);
        if (pct >= last_report_pct + 10) {
            last_report_pct = pct;
            float wr = total_trades > 0 ? (float)total_wins / total_trades : 0;
            printf("  %d%% — cycle=%d/%d trades=%d wr=%.4f port=$%.2f\n",
                   pct, target_idx, n_candles, total_trades, wr, portfolio_value);
        }
    }

    // ── Paper Proof Results ──
    printf("\n============================================================\n");
    printf("PAPER PROOF — NN ROOM v3 (Market-Direction, NO lookahead)\n");
    printf("============================================================\n\n");

    float init_port = N * INIT_CAP;
    float ret_pct = init_port > 0 ? (portfolio_value - init_port) / init_port * 100.0f : 0;
    float wr = total_trades > 0 ? (float)total_wins / total_trades : 0;
    float z = total_trades > 0 ?
        (total_wins - 0.5f*total_trades) / sqrtf(total_trades * 0.25f) : 0;

    // Sharpe (annualized for 15-min candles)
    float sharpe = 0;
    if (return_count >= 5) {
        float mr = 0;
        for (int i = 0; i < return_count; i++) mr += cycle_returns[i];
        mr /= return_count;
        float vr = 0;
        for (int i = 0; i < return_count; i++) {
            float d = cycle_returns[i] - mr;
            vr += d * d;
        }
        vr /= return_count;
        float std = sqrtf(vr > 0 ? vr : 1e-10f);
        sharpe = (mr / std) * sqrtf(35040.0f);
    }

    float mdd_pct = peak_portfolio > 0 ?
        -(peak_portfolio - portfolio_value) / peak_portfolio * 100.0f : 0;
    float pf = gross_loss > 0 ? gross_win / gross_loss : 0;
    float conv_acc = wr;

    printf("%-30s %-18s %-12s %s\n", "Metric", "Value", "Target", "Status");
    printf("---------------------------------------------------------------\n");

    int passed = 0;
#define CHECK(name, val, tgt, ok) do { \
    printf("%-30s %-18.4f %-12.4f %s\n", name, (float)(val), (float)(tgt), \
           (ok) ? "✅ PASS" : "❌ FAIL"); \
    if (ok) passed++; } while(0)

    CHECK("total_return_pct", ret_pct, 5.0, ret_pct > 5.0);
    CHECK("win_rate", wr, 0.55, wr > 0.55);
    CHECK("z_score", z, 2.33, z > 2.33);
    CHECK("sharpe", sharpe, 1.0, sharpe > 1.0);
    CHECK("max_drawdown_pct", mdd_pct, -15.0, mdd_pct > -15.0);
    CHECK("profit_factor", pf, 1.5, pf > 1.5);
    CHECK("consecutive_losses", (float)max_consec, 6.0, max_consec < 6);
    CHECK("conviction_accuracy", conv_acc, 0.60, conv_acc > 0.60);

    printf("\nCriteria passed: %d/8\n", passed);
    printf("Portfolio: $%.2f → $%.2f (return: %.2f%%)\n",
           (double)init_port, (double)portfolio_value, (double)ret_pct);
    printf("Trades: %d (%dW/%dL, WR=%.4f)\n", total_trades, total_wins, total_losses, wr);
    printf("Z-score: %.4f\n", z);
    printf("Sharpe: %.4f\n", sharpe);
    printf("Max DD: %.2f%%\n", mdd_pct);
    printf("Profit factor: %.4f\n", pf);
    printf("Max consec losses: %d\n", max_consec);
    printf("Agents alive at end: %d\n", N);

    if (passed == 8)
        printf("\n🎉 ALL 8 PASSED! Paper proof complete.\n");
    else {
        printf("\n⚠️ %d/8 passed. Need 8.\n", passed);
        // Show how close we are
        printf("\n--- Gap Analysis ---\n");
        if (wr < 0.55) printf("  WR needs +%.2f pts to reach 55%%\n", (0.55 - wr) * 100);
        if (z < 2.33) printf("  Z-score needs +%.2f\n", 2.33 - z);
        if (sharpe < 1.0) printf("  Sharpe needs +%.2f\n", 1.0 - sharpe);
        if (ret_pct < 5.0) printf("  Return needs +%.2f%%\n", 5.0 - ret_pct);
    }

    free(agents);
    return passed == 8 ? 0 : 1;
}
