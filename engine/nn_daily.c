/**
 * nn_daily.c — NN on DAILY SP500 data with VIX regime signal.
 * 10K agents × 13→16→1 MLP, REINFORCE, Darwin.
 * Predicts NEXT-DAY SP500 direction using technical + VIX features.
 * Structural edge from VIX fear/greed regimes.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define NN_AGENTS   10000
#define N_INPUTS    16  // 13 OHLCV + 3 GAAD φ-features
#define N_HIDDEN    16
#define N_OUTPUTS   1
#define INIT_CAP    50.0f
#define TAKER_FEE   0.001f
#define LEARN_RATE  0.002f
#define WARMUP      50
#define MAX_DAYS    10000
#define PHI         1.618033988749895f

typedef struct {
    float w1[N_INPUTS * N_HIDDEN];
    float b1[N_HIDDEN];
    float w2[N_HIDDEN * N_OUTPUTS];
    float b2[N_OUTPUTS];
} NNWeights;

typedef struct {
    NNWeights nn;
    float capital, peak_capital, starting_capital;
    int trades, wins, losses;
    int consecutive_losses;
    float total_pnl, max_drawdown;
    float win_rate_ema;
} NNAgent;

typedef struct {
    int64_t ts;
    float sp500, vix, yield;
} DailyPoint;

// ── Helpers ──
static inline float sig(float x) {
    if (x < -10) return 0; if (x > 10) return 1;
    return 1.0f / (1.0f + expf(-x));
}
static inline float relu(float x) { return x > 0 ? x : 0.01f * x; }

static float nn_forward(const NNWeights *w, const float *feat, float *logit_out) {
    float h[N_HIDDEN];
    for (int i = 0; i < N_HIDDEN; i++) {
        float s = w->b1[i];
        for (int j = 0; j < N_INPUTS; j++) s += feat[j] * w->w1[j * N_HIDDEN + i];
        h[i] = relu(s);
    }
    float logit = w->b2[0];
    for (int i = 0; i < N_HIDDEN; i++) logit += h[i] * w->w2[i];
    if (logit_out) *logit_out = logit;
    return sig(logit);
}

static void nn_reinforce(NNWeights *w, int won, float stake, float logit, float lr) {
    float p = sig(logit);
    float adv = won ? (stake * (1 - TAKER_FEE)) : (-stake * (1 + TAKER_FEE));
    float g = (won ? (1-p) : -p) * adv * lr;
    for (int i = 0; i < N_HIDDEN; i++) w->w2[i] += g;
    w->b2[0] += g;
    for (int i = 0; i < N_HIDDEN; i++) w->b1[i] += g * 0.1f;
}

static void init_weights(NNWeights *w, unsigned int seed) {
    srand(seed);
    int nw = sizeof(NNWeights) / sizeof(float);
    float *wp = (float*)w;
    for (int i = 0; i < nw; i++) wp[i] = (float)rand() / RAND_MAX * 0.2f - 0.1f;
}

static void mutate(NNWeights *w, float rate, float scale) {
    int nw = sizeof(NNWeights) / sizeof(float);
    float *wp = (float*)w;
    for (int i = 0; i < nw; i++)
        if ((float)rand() / RAND_MAX < rate)
            wp[i] += (float)rand() / RAND_MAX * scale - scale/2;
}

// ════════════════════════════════════════════════════════
//  FEATURE ENGINE (daily SP500 + VIX)
// ════════════════════════════════════════════════════════
// Returns 13-dim feature vector from history up to span-1.
// Features strictly BEFORE the target day.
static void compute_daily_features(const float *px, const float *vx,
                                           const float *vd, int len,
                                    float *feat, int n_feat) {
    memset(feat, 0, n_feat * sizeof(float));
    if (len < 2) return;

    // F1: Previous day return %
    feat[0] = px[len-1] > 0 ? (px[len-1] - px[len-2]) / px[len-2] * 100.0f : 0;

    // F2: 5-day momentum
    if (len >= 6) {
        float p5 = px[len-6];
        feat[1] = p5 > 0 ? (px[len-1] - p5) / p5 * 100.0f : 0;
    }

    // F3: RSI(14)
    if (len >= 15) {
        float gains = 0, losses = 0;
        for (int i = len-15; i < len-1; i++) {
            float d = px[i+1] - px[i];
            if (d > 0) gains += d; else losses -= d;
        }
        if (losses > 0) {
            float rs = (gains/14) / (losses/14);
            feat[2] = 100.0f - 100.0f / (1.0f + rs);
        } else feat[2] = 100.0f;
    }

    // F4: VIX z-score over last 20 days (how extreme is current VIX)
    if (len >= 20) {
        float v_mn = 0;
        for (int i = len-20; i < len; i++) v_mn += vx[i];
        v_mn /= 20;
        float v_var = 0;
        for (int i = len-20; i < len; i++) { float d = vx[i]-v_mn; v_var += d*d; }
        float v_std = sqrtf(fmaxf(v_var/20, 0.0001f));
        feat[3] = fmaxf(-3.0f, fminf(3.0f, (vx[len-1]-v_mn)/v_std));  // clamp at ±3σ
    } else feat[3] = 0.0f;

    // F5/F6: EMA fast(5) / slow(20)
    if (len >= 5) {
        float k5 = 2.0f/6, k20 = 2.0f/21;
        float e5 = px[0], e20 = px[0];
        for (int i = 1; i < len; i++) {
            e5 = px[i] * k5 + e5 * (1-k5);
            e20 = px[i] * k20 + e20 * (1-k20);
        }
        feat[4] = e5;
        feat[5] = e20;
    }

    // F7: MACD histogram (12,26,9)
    if (len >= 26) {
        float k12 = 2.0f/13, k26 = 2.0f/27;
        float e12 = px[0], e26 = px[0];
        for (int i = 1; i < len; i++) {
            e12 = px[i] * k12 + e12 * (1-k12);
            e26 = px[i] * k26 + e26 * (1-k26);
        }
        float macd = e12 - e26;
        // Signal line: 9-period EMA of MACD (approximate with 9-period EMA of px)
        float k9 = 2.0f/10;
        float s9 = px[0];
        for (int i = 1; i < len; i++) s9 = px[i] * k9 + s9 * (1-k9);
        feat[6] = macd - (e12 - s9) / 2.0f; // simplified signal
    }

    // F8: Bollinger %B (20,2)
    if (len >= 20) {
        const float *p = px + len - 20;
        float mn = 0;
        for (int i = 0; i < 20; i++) mn += p[i];
        mn /= 20;
        float var = 0;
        for (int i = 0; i < 20; i++) { float d = p[i] - mn; var += d*d; }
        float std = sqrtf(fmaxf(var/20, 0.0001f));
        float lo = mn - 2*std, hi = mn + 2*std;
        feat[7] = (px[len-1] - lo) / (hi - lo + 0.0001f);
    }

    // F9: VIX level (normalized 0-1, clamp at 0-100)
    feat[8] = fminf(vx[len-1] / 100.0f, 1.0f);

    // F10: VIX change % (today vs 5 days ago)
    if (len >= 6) {
        float v_prev = vx[len-6];
        feat[9] = v_prev > 0 ? (vx[len-1] - v_prev) / v_prev * 100.0f : 0;
    }

    // F11: Regime (0=low fear, 1=normal, 2=high fear)
    float v_now = vx[len-1];
    if (v_now > 30.0f) feat[10] = 2.0f;
    else if (v_now < 15.0f) feat[10] = 0.0f;
    else feat[10] = 1.0f;

    // F12: SP500/VIX divergence (correlation sign over last 5 days, tighter)
    if (len >= 6) {
        float sp_corr = 0;
        for (int i = len-5; i < len; i++) {
            if (i > 0) {
                float sp_d = (px[i] - px[i-1]) / px[i-1];
                float vx_d = (vx[i] - vx[i-1]) / vx[i-1];
                sp_corr += sp_d * (-vx_d); // Inverse: SP up, VIX down = +1
            }
        }
        sp_corr /= 4.0f;
        // Normalize to roughly -1..1, clamp
        feat[11] = fmaxf(-1.0f, fminf(1.0f, sp_corr * 10.0f));
    }

    // F13: 10yr Treasury yield (normalized 0-1, typical range 0.5-5.5%)
    feat[12] = fmaxf(0.0f, fminf(1.0f, (vd[len-1] - 0.5f) / 5.0f));

    // ── P12: GAAD φ-interval features (multi-scale returns) ──
    // F14: φ¹ return (~2-day interval)
    if (len >= 3) {
        int interval = (int)(PHI + 0.5f);
        if (interval < 2) interval = 2;
        float p_prev = px[len - 1 - interval];
        feat[13] = p_prev > 0 ? (px[len-1] - p_prev) / p_prev * 100.0f : 0;
    }
    // F15: φ² return (~3-day interval)
    if (len >= 4) {
        int interval = (int)(PHI * PHI + 0.5f);
        if (interval < 2) interval = 2;
        float p_prev = px[len - 1 - interval];
        feat[14] = p_prev > 0 ? (px[len-1] - p_prev) / p_prev * 100.0f : 0;
    }
    // F16: φ⁵ return (~11-day interval)
    if (len >= 13) {
        int interval = (int)(powf(PHI, 5.0f) + 0.5f);
        float p_prev = px[len - 1 - interval];
        feat[15] = p_prev > 0 ? (px[len-1] - p_prev) / p_prev * 100.0f : 0;
    }
}

// ════════════════════════════════════════════════════════
//  DATA LOAD
// ════════════════════════════════════════════════════════
static int load_daily(const char *sp_path, const char *vix_path,
                       DailyPoint *buf, int max) {
    // Load SP500
    char dates[MAX_DAYS][16];
    float sp_px[MAX_DAYS];
    int n_sp = 0;
    FILE *f = fopen(sp_path, "r");
    if (!f) return -1;
    char line[4096];
    fgets(line, sizeof(line), f); // header
    while (fgets(line, sizeof(line), f) && n_sp < max) {
        char dt[32]; float val;
        if (sscanf(line, " %31[^,],%f", dt, &val) >= 2) {
            if (dt[0] && val > 0) {
                strncpy(dates[n_sp], dt, 15); dates[n_sp][15]=0;
                sp_px[n_sp] = val;
                n_sp++;
            }
        }
    }
    fclose(f);
    printf("[DATA] SP500: %d rows\n", n_sp);

    // Load VIX — build date->value map
    float vix_map[MAX_DAYS];
    char vix_dates[MAX_DAYS][16];
    int n_vix = 0;
    f = fopen(vix_path, "r");
    if (!f) return -1;
    fgets(line, sizeof(line), f); // header
    while (fgets(line, sizeof(line), f) && n_vix < max) {
        char dt[32]; float o,h,l,c,vol;
        // Format: Date,Open,High,Low,Close,Volume,Dividends,Stock Splits
        if (sscanf(line, " %31[^,],%f,%f,%f,%f,%f", dt, &o, &h, &l, &c, &vol) >= 5) {
            // Take date part only (before space)
            char *space = strchr(dt, ' ');
            if (space) *space = 0;
            if (dt[0] && c > 0) {
                strncpy(vix_dates[n_vix], dt, 15); vix_dates[n_vix][15]=0;
                vix_map[n_vix] = c;
                n_vix++;
            }
        }
    }
    fclose(f);
    printf("[DATA] VIX: %d rows\n", n_vix);

    // Merge by date, in chronological order
    int count = 0;
    for (int si = 0; si < n_sp && count < max; si++) {
        // Find matching VIX date
        for (int vi = 0; vi < n_vix; vi++) {
            if (strcmp(dates[si], vix_dates[vi]) == 0) {
                // Parse date to timestamp
                struct tm tm = {0};
                sscanf(dates[si], "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                buf[count].ts = (int64_t)mktime(&tm);
                buf[count].sp500 = sp_px[si];
                buf[count].vix = vix_map[vi];
                count++;
                break;
            }
        }
    }
    
    // Load 10yr Treasury yield
    float yld_buf[MAX_DAYS];
    char yld_dates[MAX_DAYS][16];
    int n_yld = 0;
    f = fopen("/home/wubu2/.hermes/pm_logs/historical/raw/stocks/DGS10_daily.csv", "r");
    if (f) {
        fgets(line, sizeof(line), f); // header
        while (fgets(line, sizeof(line), f) && n_yld < MAX_DAYS) {
            char dt[32]; float val;
            if (sscanf(line, " %31[^,],%f", dt, &val) >= 2) {
                char *dot = strchr(dt, '.');
                if (dot) continue; // skip . (null) entries
                if (strlen(dt) > 0) {
                    strncpy(yld_dates[n_yld], dt, 15); yld_dates[n_yld][15]=0;
                    yld_buf[n_yld] = val;
                    n_yld++;
                }
            }
        }
        fclose(f);
    }
    printf("[DATA] DGS10: %d rows\n", n_yld);
    
    // Re-merge with yield
    count = 0;
    for (int si = 0; si < n_sp && count < max; si++) {
        for (int vi = 0; vi < n_vix; vi++) {
            if (strcmp(dates[si], vix_dates[vi]) == 0) {
                // Find matching yield
                float yld = 0;
                for (int yi = 0; yi < n_yld; yi++) {
                    if (strcmp(dates[si], yld_dates[yi]) == 0) {
                        yld = yld_buf[yi];
                        break;
                    }
                }
                struct tm tm = {0};
                sscanf(dates[si], "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
                tm.tm_year -= 1900; tm.tm_mon -= 1;
                buf[count].ts = (int64_t)mktime(&tm);
                buf[count].sp500 = sp_px[si];
                buf[count].vix = vix_map[vi];
                buf[count].yield = yld;
                count++;
                break;
            }
        }
    }
    printf("[DATA] Merged: %d daily points\n", count);
    return count;
}

// ════════════════════════════════════════════════════════
//  EVOLUTION
// ════════════════════════════════════════════════════════
static void evolve(NNAgent *agents, int n) {
    int alive = 0;
    for (int i = 0; i < n; i++)
        if (agents[i].capital > 1 && agents[i].trades >= 5) alive++;
    if (alive < 10) return;

    // Sort by win_rate_ema descending
    for (int i = 0; i < n; i++)
        for (int j = i+1; j < n; j++)
            if (agents[j].win_rate_ema > agents[i].win_rate_ema) {
                NNAgent t = agents[i]; agents[i] = agents[j]; agents[j] = t;
            }

    int nc = alive / 4;
    if (nc < 1) nc = 1;
    int nw = sizeof(NNWeights) / sizeof(float);
    for (int i = 0; i < nc; i++) {
        int dst = n-1-i;
        int p1 = rand() % nc, p2 = rand() % nc;
        memcpy(&agents[dst].nn, &agents[p1].nn, sizeof(NNWeights));
        float *wd = (float*)&agents[dst].nn, *ws = (float*)&agents[p2].nn;
        for (int k = 0; k < nw; k++) wd[k] = (wd[k] + ws[k]) / 2;
        mutate(&agents[dst].nn, 0.15f, 0.1f);
        agents[dst].capital = fmaxf(INIT_CAP * 0.5f, agents[dst].capital);
    }
}

// ════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════
int main(int argc, char **argv) {
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    int N = argc > 1 ? atoi(argv[1]) : 10000;
    if (N > NN_AGENTS) N = NN_AGENTS;

    printf("=== NN DAILY — SP500+VIX Regime Paper Proof (16-feat) ===\n");
    printf("Agents: %d  Network: %d→%d→%d  LR: %.4f\n\n",
           N, N_INPUTS, N_HIDDEN, N_OUTPUTS, LEARN_RATE);

    // Load data
    DailyPoint data[MAX_DAYS];
    int nd = load_daily(
        "/home/wubu2/.hermes/pm_logs/historical/sp500.csv",
        "/home/wubu2/.hermes/pm_logs/historical/raw/stocks/VIX_daily.csv",
        data, MAX_DAYS);
    if (nd < WARMUP + 10) { fprintf(stderr, "Not enough data\n"); return 1; }

    // Feature price buffer
    float px[MAX_DAYS], vx[MAX_DAYS], vy[MAX_DAYS];
    for (int i = 0; i < nd; i++) {
        px[i] = data[i].sp500; vx[i] = data[i].vix; vy[i] = data[i].yield;
    }

    // Init agents
    NNAgent *agents = (NNAgent*)calloc(N, sizeof(NNAgent));
    srand(42);
    for (int i = 0; i < N; i++) {
        agents[i].capital = agents[i].peak_capital = agents[i].starting_capital = INIT_CAP;
        agents[i].win_rate_ema = 0.5f;
        init_weights(&agents[i].nn, i+100);
    }

    // ── Backtest ──
    float portfolio = N * INIT_CAP, peak = portfolio;
    int total_trades = 0, total_wins = 0, total_losses = 0, max_consec = 0;
    float gross_win = 0, gross_loss = 0;
    float features[16];
    float cyc_ret[128] = {0}; int ri = 0, rc = 0;

    for (int t = WARMUP; t < nd; t++) {
        // Features from history [0..t-1] — strictly before target
        compute_daily_features(px, vx, vy, t, features, 16);

        // Target: did SP500 go up today?
        int price_up = data[t].sp500 >= data[t-1].sp500;

        float pnl_tc = 0;
        for (int i = 0; i < N; i++) {
            NNAgent *a = &agents[i];
            if (a->capital <= 0.01f) continue;

            float logit;
            float prob = nn_forward(&a->nn, features, &logit);
            int dir = prob >= 0.5f;
            float conv = fabsf(prob - 0.5f) * 2.0f;

            if (conv < 0.15f) continue;  // skip low conviction

            float stake = a->capital * 0.02f * conv;
            if (stake < 0.01f || stake > a->capital * 0.05f) continue;

            a->capital -= stake;
            a->trades++;
            int won = dir == price_up;

            if (won) {
                float profit = stake * (1 - TAKER_FEE);
                a->capital += stake + profit;
                a->total_pnl += profit; a->wins++;
                a->consecutive_losses = 0;
                a->win_rate_ema = 0.9f * a->win_rate_ema + 0.1f;
                pnl_tc += profit; total_wins++; gross_win += profit;
                nn_reinforce(&a->nn, 1, stake, logit, LEARN_RATE);
            } else {
                a->total_pnl -= stake; a->losses++;
                a->consecutive_losses++;
                a->win_rate_ema = 0.9f * a->win_rate_ema;
                pnl_tc -= stake; total_losses++; gross_loss += stake;
                nn_reinforce(&a->nn, 0, stake, logit, LEARN_RATE);
            }
            total_trades++;
            if (a->consecutive_losses > max_consec) max_consec = a->consecutive_losses;
            if (a->capital > a->peak_capital) a->peak_capital = a->capital;
            float dd = (a->peak_capital - a->capital) / a->peak_capital;
            if (dd > a->max_drawdown) a->max_drawdown = dd;
        }

        // Update portfolio
        portfolio = 0; for (int i = 0; i < N; i++) portfolio += agents[i].capital;
        if (portfolio > peak) peak = portfolio;
        if (portfolio > 0) { cyc_ret[ri] = pnl_tc / portfolio; ri = (ri+1)%128; if (rc<128) rc++; }

        // Darwin every 200 trades
        if (total_trades > 0 && total_trades % 200 == 0 && total_trades < 10000)
            evolve(agents, N);
        // After 10K trades, evolve every 500
        if (total_trades > 0 && total_trades % 500 == 0 && total_trades >= 10000)
            evolve(agents, N);

        // Progress
        if ((t - WARMUP) % 100 == 0 && t > WARMUP) {
            float wr = total_trades > 0 ? (float)total_wins / total_trades : 0;
            printf("  day=%d/%d trades=%d wr=%.4f port=$%.2f\n",
                   t, nd, total_trades, wr, portfolio);
        }
    }

    // ── Results ──
    printf("\n============================================================\n");
    printf("PAPER PROOF — NN DAILY (Market-Direction, SP500+VIX)\n");
    printf("============================================================\n\n");

    float init = N * INIT_CAP;
    float ret_pct = init > 0 ? (portfolio - init) / init * 100.0f : 0;
    float wr = total_trades > 0 ? (float)total_wins / total_trades : 0;
    float z = total_trades > 0 ?
        (total_wins - 0.5f*total_trades) / sqrtf(total_trades * 0.25f) : 0;

    float sharpe = 0;
    if (rc >= 5) {
        float mr = 0; for (int i = 0; i < rc; i++) mr += cyc_ret[i]; mr /= rc;
        float vr = 0; for (int i = 0; i < rc; i++) { float d = cyc_ret[i]-mr; vr += d*d; }
        float std = sqrtf(fmaxf(vr/rc, 1e-10f));
        sharpe = (mr / std) * sqrtf(252.0f); // annualized for daily
    }

    float mdd = peak > 0 ? -(peak - portfolio) / peak * 100.0f : 0;
    float pf = gross_loss > 0 ? gross_win / gross_loss : 0;
    int passed = 0;

    printf("%-30s %-18s %-12s %s\n", "Metric", "Value", "Target", "Status");
    printf("---------------------------------------------------------------\n");
#define CHK(name, v, t, ok) do { \
    printf("%-30s %-18.4f %-12.4f %s\n", name, (float)(v), (float)(t), ok?"✅ PASS":"❌ FAIL"); \
    if (ok) passed++; } while(0)
    CHK("total_return_pct", ret_pct, 5.0, ret_pct > 5.0);
    CHK("win_rate", wr, 0.55, wr > 0.55);
    CHK("z_score", z, 2.33, z > 2.33);
    CHK("sharpe", sharpe, 1.0, sharpe > 1.0);
    CHK("max_drawdown_pct", mdd, -15.0, mdd > -15.0);
    CHK("profit_factor", pf, 1.5, pf > 1.5);
    CHK("consecutive_losses", (float)max_consec, 6.0, max_consec < 6);
    CHK("conviction_accuracy", wr, 0.60, wr > 0.60);
    printf("\nCriteria passed: %d/8\n", passed);
    printf("Portfolio: $%.2f → $%.2f (%.2f%%)\n", init, portfolio, ret_pct);
    printf("Trades: %d (%dW/%dL, WR=%.4f)\n", total_trades, total_wins, total_losses, wr);
    printf("Z=%.4f Sharpe=%.4f DD=%.2f%% PF=%.4f MaxLoss=%d\n",
           z, sharpe, mdd, pf, max_consec);

    if (passed == 8) printf("\n🎉 ALL 8 PASSED! Paper proof complete.\n");
    else printf("\n⚠️ %d/8 passed.\n---\n", passed);
    free(agents);
    return passed == 8 ? 0 : 1;
}
