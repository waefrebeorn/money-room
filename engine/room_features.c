/**
 * room_features.c — L2: 13-dim feature vector per tick
 * Combines market data, news sentiment, and agent consensus into features.
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "types.h"

// ── History ring buffers — per-market-type ──
#define FEED_HISTORY 50
#define N_FEED_MARKETS 10  // matches N_MARKET_TYPES
static float price_history[N_FEED_MARKETS][FEED_HISTORY];
static float volume_history[N_FEED_MARKETS][FEED_HISTORY];
static int hist_len[N_FEED_MARKETS];
static int hist_idx[N_FEED_MARKETS];

// ── RSI ──
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

// ── EMA ──
static float calc_ema(const float *prices, int len, int period) {
    if (len < period) return prices[len - 1];
    float k = 2.0f / (period + 1);
    float ema = prices[0];
    for (int i = 1; i < len; i++)
        ema = prices[i] * k + ema * (1.0f - k);
    return ema;
}

// ── MACD ──
static float calc_macd_hist(const float *prices, int len) {
    if (len < 26) return 0;
    float ema12 = calc_ema(prices, len, 12);
    float ema26 = calc_ema(prices, len, 26);
    float macd = ema12 - ema26;
    // Signal line: EMA of MACD (9-period approximation)
    // For 1-min data, signal line = last 9 values of MACD line
    // Simplified: use macd - ema_of_macd
    float signal = calc_ema(prices + (len > 9 ? len - 9 : 0),
                            len > 9 ? 9 : len, 9);
    return macd - signal;
}

// ── Bollinger %B ──
static float calc_bollinger_pct(const float *prices, int len) {
    if (len < 20) return 0.5f;
    float sum = 0, mean = 0;
    int n = len > 20 ? 20 : len;
    const float *p = prices + len - n;
    for (int i = 0; i < n; i++) sum += p[i];
    mean = sum / n;
    float var = 0;
    for (int i = 0; i < n; i++) {
        float d = p[i] - mean;
        var += d * d;
    }
    float std = sqrtf(var / n);
    float last = prices[len - 1];
    float lower = mean - 2 * std;
    float upper = mean + 2 * std;
    if (upper - lower < 0.0001f) return 0.5f;
    return (last - lower) / (upper - lower);
}

// ── Regime detection ──
// 0=range, 1=trend, 2=volatile
static float calc_regime(const float *prices, int len) {
    if (len < 10) return 0;
    float sum = 0, mean = 0;
    for (int i = len - 10; i < len; i++) sum += prices[i];
    mean = sum / 10;
    float var = 0;
    for (int i = len - 10; i < len; i++) {
        float d = prices[i] - mean;
        var += d * d;
    }
    float std = sqrtf(var / 10);
    float range_pct = std / (mean > 0 ? mean : 1);
    
    // Directional movement over 10 periods
    float net = prices[len - 1] - prices[len - 10];
    float gross = 0;
    for (int i = len - 9; i < len; i++)
        gross += fabsf(prices[i] - prices[i - 1]);
    
    float efficiency = gross > 0 ? fabsf(net) / gross : 0;
    
    if (range_pct > 0.005) return 2;       // Volatile
    if (efficiency > 0.6) return 1;         // Trending
    return 0;                                // Ranging
}

// ── P12: GAAD Golden-ratio timeframe features ──
// Compute features at φ-multiplied intervals for multi-scale analysis.
// Derived from GAAD-WuBu-ST paper: φ-based window decomposition.
static void compute_phi_features(const float *px, int len, float *phi_return, float *phi_vol, float *phi_momentum) {
    *phi_return = 0.0f;
    *phi_vol = 0.0f;
    *phi_momentum = 0.0f;
    if (len < 3) return;
    
    // φ-multiplied intervals: 1, φ, φ², φ³, ...
    float intervals[] = {1.0f, PHI, PHI*PHI, PHI*PHI*PHI};
    int n_phi = 4;
    
    // Weighted multi-scale return
    float ret_sum = 0.0f, vol_sum = 0.0f, mom_sum = 0.0f;
    float total_w = 0.0f;
    
    for (int i = 0; i < n_phi; i++) {
        int period = (int)(intervals[i] + 0.5f);
        if (period < 1) period = 1;
        if (period >= len) continue;
        
        float ret = (px[len-1] - px[len-1-period]) / (px[len-1-period] > 0 ? px[len-1-period] : 1.0f);
        float w = 1.0f / intervals[i];  // Higher weight on shorter intervals
        
        ret_sum += ret * w;
        vol_sum += fabsf(ret) * w;
        
        // Momentum at φ-scale: compare φ-interval return to previous φ-interval
        if (len > period * 2) {
            float prev_ret = (px[len-1-period] - px[len-1-period*2]) / (px[len-1-period*2] > 0 ? px[len-1-period*2] : 1.0f);
            mom_sum += (ret - prev_ret) * w;
        }
        
        total_w += w;
    }
    
    if (total_w > 0) {
        *phi_return = ret_sum / total_w;
        *phi_vol = vol_sum / total_w;
        *phi_momentum = mom_sum / total_w;
    }
}

// ── P13: Goertzel DFT — extract dominant frequency ──
// Single-frequency DFT using Goertzel algorithm.
// Finds dominant cycle in price history.
static float compute_dft_dominant(const float *px, int len) {
    if (len < 10) return 0.0f;
    
    // Remove DC component
    float mean = 0;
    for (int i = 0; i < len; i++) mean += px[i];
    mean /= len;
    
    // Search for dominant frequency in range [2, len/2] periods
    float max_mag = 0;
    
    for (int k = 2; k <= len / 2; k++) {
        float omega = TWO_PI * k / len;
        float coeff = 2.0f * cosf(omega);
        float s0 = 0, s1 = 0, s2 = 0;
        
        for (int i = 0; i < len; i++) {
            s0 = (px[i] - mean) + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        
        float real = s1 - s2 * cosf(omega);
        float imag = s2 * sinf(omega);
        float mag = real * real + imag * imag;
        
        if (mag > max_mag) {
            max_mag = mag;
        }
    }
    
    // Normalize: 0 = no dominant cycle, 1 = strong cycle
    float norm = max_mag / (len * len * 0.01f + 1.0f);
    return fminf(norm, 1.0f);
}

// ── P15: Tailslayer tail risk detection ──
// Computes tail risk from excess kurtosis + extreme moves.
// Returns 0-1 where 0=normal gaussian, 1=extreme fat-tail risk.
static float compute_tail_risk(const float *px, int len) {
    if (len < 10) return 0.0f;

    // Compute log returns
    float returns[FEED_HISTORY];
    int n_ret = 0;
    for (int i = 1; i < len; i++) {
        if (px[i - 1] > 0) {
            returns[n_ret++] = logf(px[i] / px[i - 1]);
        }
    }
    if (n_ret < 5) return 0.0f;

    // Mean and std of returns
    float mean = 0.0f;
    for (int i = 0; i < n_ret; i++) mean += returns[i];
    mean /= n_ret;

    float var = 0.0f;
    for (int i = 0; i < n_ret; i++) {
        float d = returns[i] - mean;
        var += d * d;
    }
    float std = sqrtf(var / n_ret);
    if (std < 1e-8f) return 0.0f;

    // ── Kurtosis: E[(X-μ)⁴] / σ⁴ — excess kurtosis > 3 = fat tails
    float m4 = 0.0f;
    float extreme_count = 0.0f;
    for (int i = 0; i < n_ret; i++) {
        float z = (returns[i] - mean) / std;
        float z2 = z * z;
        m4 += z2 * z2;
        // Count extreme moves (>2 sigma)
        if (fabsf(z) > 2.0f) extreme_count += 1.0f;
    }
    float kurtosis = m4 / n_ret;  // Raw kurtosis (excess = kurtosis - 3)
    float extreme_ratio = extreme_count / n_ret;

    // ── Composite score: blend excess kurtosis + extreme move frequency
    // Excess kurtosis: 0=normal, >3=very fat. Normalize via tanh(k/3)
    float kurt_contrib = tanhf(fmaxf(kurtosis - 3.0f, 0.0f) / 3.0f);
    // Extreme ratio: 0-1, expect 0.05 for normal (5% beyond 2σ), scale up
    float extreme_contrib = fminf(extreme_ratio * 10.0f, 1.0f);

    // Blend: 60% kurtosis, 40% extreme frequency
    float score = 0.6f * kurt_contrib + 0.4f * extreme_contrib;
    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;
    return score;
}
RoomError room_features_compute(const MarketTick *tick, FeatureVector *fv, const RoomState *s) {
    memset(fv, 0, sizeof(FeatureVector));
    
    // Determine market type for per-buffer indexing
    int mt = (int)tick->market_type;
    if (mt < 0 || mt >= N_FEED_MARKETS) mt = MARKET_CRYPTO;
    bool is_binary = (tick->market_type == MARKET_SPORTS || tick->market_type == MARKET_WEATHER ||
                      tick->market_type == MARKET_PREDICTION || tick->market_type == MARKET_ELECTION);
    
    // Determine "price" for this market type
    float price_val;
    if (is_binary) {
        // Binary markets: clamp to probability 0-1
        price_val = tick->close;
        if (price_val < 0.0f) price_val = 0.0f;
        if (price_val > 1.0f) price_val = 1.0f;
        if (tick->close > 1000.0f) price_val = 0.5f;  // BTC price, not probability
    } else {
        price_val = tick->close;
    }
    
    // Push into per-market history buffer
    price_history[mt][hist_idx[mt]] = price_val;
    volume_history[mt][hist_idx[mt]] = tick->volume;
    hist_idx[mt] = (hist_idx[mt] + 1) % FEED_HISTORY;
    if (hist_len[mt] < FEED_HISTORY) hist_len[mt]++;
    
    // Need at least 1 data point for initial features
    if (hist_len[mt] < 1) return ERR_NO_DATA;
    
    // Build linear price array (oldest to newest) from per-market buffer
    float px[FEED_HISTORY];
    float vol[FEED_HISTORY];
    for (int i = 0; i < hist_len[mt]; i++) {
        int idx = (hist_idx[mt] - hist_len[mt] + i + FEED_HISTORY) % FEED_HISTORY;
        px[i] = price_history[mt][idx];
        vol[i] = volume_history[mt][idx];
    }
    
    // With only 1 data point, duplicate it for feature computation
    if (hist_len[mt] == 1) {
        px[1] = px[0];
        vol[1] = vol[0];
    }
    
    // F1: Price delta (current vs window open)
    if (tick->open > 0) {
        if (is_binary) {
            fv->price_delta_pct = (tick->close - tick->open) * 100.0f;  // Probability delta
        } else {
            fv->price_delta_pct = (tick->close - tick->open) / tick->open * 100.0f;
        }
    }
    
    // F2: Micro momentum (last 2 closes delta)
    if (hist_len[mt] >= 3)
        fv->micro_momentum = (px[hist_len[mt] - 1] - px[hist_len[mt] - 2]) * (is_binary ? 100.0f : (1.0f / fmax(px[hist_len[mt] - 2], 0.001f)));
    else if (hist_len[mt] >= 2)
        fv->micro_momentum = (px[1] - px[0]) * (is_binary ? 100.0f : (1.0f / fmax(px[0], 0.001f)));
    
    // F3: RSI(7) — meaningful for both price and probability
    fv->rsi_7 = calc_rsi(px, hist_len[mt], 7);
    
    // F4: Volume surge ratio
    if (hist_len[mt] >= 4) {
        float recent = (vol[hist_len[mt] - 1] + vol[hist_len[mt] - 2]) / 2.0f;
        float prior = (vol[hist_len[mt] - 3] + vol[hist_len[mt] - 4]) / 2.0f;
        fv->volume_surge_ratio = prior > 0 ? recent / prior : 1.0f;
    } else {
        fv->volume_surge_ratio = 1.0f;
    }
    
    // F5: EMA fast (3)
    fv->ema_fast = calc_ema(px, hist_len[mt], 3);
    
    // F6: EMA slow (8)
    fv->ema_slow = calc_ema(px, hist_len[mt], 8);
    
    // F7: MACD histogram
    fv->macd_hist = calc_macd_hist(px, hist_len[mt]);
    
    // F8: Bollinger %B
    fv->bollinger_pct = calc_bollinger_pct(px, hist_len[mt]);
    
    // F9: Divergence score (price vs RSI)
    if (hist_len[mt] >= 14) {
        float rsi_now = fv->rsi_7;
        float rsi_prev = calc_rsi(px, hist_len[mt] - 7, 7);
        float px_now = px[hist_len[mt] - 1];
        float px_prev = px[hist_len[mt] - 7];
        float px_dir = px_now > px_prev ? 1.0f : -1.0f;
        float rsi_dir = rsi_now > rsi_prev ? 1.0f : -1.0f;
        fv->divergence_score = (rsi_dir - px_dir) / 2.0f;
    }
    
    // F10: Pump score (from crony-weighted news pipeline)
    fv->pump_score = tick->pump_score;
    
    // F11: Regime indicator
    fv->regime_indicator = calc_regime(px, hist_len[mt]);
    
    // F12: Fear & Greed normalized
    fv->fear_greed_norm = tick->fear_greed / 100.0f;
    
    // F13: Herd consensus (what % of agents voted UP last cycle)
    if (s->vote_count > 0) {
        int up = 0;
        for (int i = 0; i < s->vote_count; i++) {
            if (s->votes[i].direction) up++;
        }
        fv->herd_consensus = (float)up / s->vote_count;
    } else {
        fv->herd_consensus = 0.5f;
    }
    
    // F14-F16: GAAD φ-interval features (P12)
    compute_phi_features(px, hist_len[mt], &fv->phi_return, &fv->phi_vol, &fv->phi_momentum);
    
    // F17: DFT dominant frequency (P13)
    fv->dft_dominant = compute_dft_dominant(px, hist_len[mt]);

    // F20: Tailslayer tail risk score (P15)
    fv->tail_risk_score = compute_tail_risk(px, hist_len[mt]);

    return ERR_OK;
}
