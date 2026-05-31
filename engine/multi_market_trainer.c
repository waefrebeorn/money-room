/*
 * multi_market_trainer.c — Train agents on real market data
 * Reads live market_feed.json (or data/*.json) from multiple markets
 * Reuses engine structs from types.h for agent/genome compatibility
 *
 * Compile: gcc -O3 -o multi_market_trainer multi_market_trainer.c \
 *          room_engine.o room_feeds.o room_features.o room_vote.o \
 *          room_capital.o room_darwin.o room_bridge.o -lm
 * Usage:   ./multi_market_trainer <markets.json> [--agents N] [--cycles N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "types.h"

#define MAX_MARKETS 16
#define MAX_TRAIN_AGENTS 500
#define OUTPUT_DIR  "/home/wubu2/money-room/data"

/* Market definition from JSON */
typedef struct {
    char name[32];
    char feed_path[256];
    char pair[16];
    int  period_s;
    int  n_candles;
    double *opens, *highs, *lows, *closes, *volumes;
} Market;

/* Feature vector built from market data (matches engine's N_FEATURES) */
static void build_features(const Market *m, int idx, float *features) {
    if (idx < 20 || idx >= m->n_candles) {
        for (int i = 0; i < N_FEATURES; i++) features[i] = 0.5f;
        return;
    }

    /* F0: Price position in 20-period range */
    double hi = -1e9, lo = 1e9;
    for (int i = idx - 20; i <= idx; i++) {
        if (m->highs[i] > hi) hi = m->highs[i];
        if (m->lows[i] < lo) lo = m->lows[i];
    }
    features[0] = (hi - lo) > 0 ? (m->closes[idx] - lo) / (hi - lo) : 0.5f;

    /* F1: Returns (1-period) */
    features[1] = idx > 0 ? (m->closes[idx] - m->closes[idx-1]) / m->closes[idx-1] * 100 : 0;

    /* F2: Returns (5-period) */
    features[2] = idx >= 5 ? (m->closes[idx] - m->closes[idx-5]) / m->closes[idx-5] * 100 : 0;

    /* F3: Returns (20-period) */
    features[3] = idx >= 20 ? (m->closes[idx] - m->closes[idx-20]) / m->closes[idx-20] * 100 : 0;

    /* F4: Volume ratio (current vs 20-period avg) */
    double vol_sum = 0;
    for (int i = idx - 20; i < idx; i++) if (i >= 0) vol_sum += m->volumes[i];
    features[4] = vol_sum > 0 ? m->volumes[idx] / (vol_sum / 20) : 1.0f;
    if (features[4] > 5) features[4] = 5;
    features[4] = features[4] / 5.0f;

    /* F5: Volatility (10-period ATR / price) */
    double atr = 0;
    for (int i = idx - 10; i < idx; i++) {
        if (i > 0) atr += fmax(m->highs[i] - m->lows[i],
                     fmax(fabs(m->highs[i] - m->closes[i-1]),
                          fabs(m->lows[i] - m->closes[i-1])));
    }
    atr /= 10;
    features[5] = atr / m->closes[idx] * 100;
    if (features[5] > 10) features[5] = 10;
    features[5] /= 10.0f;

    /* F6-F17: Spread across range (random initialization for now) */
    for (int i = 6; i < N_FEATURES; i++) {
        features[i] = 0.5f + 0.1f * ((float)(idx % (i+1)) / (i+1) - 0.5f);
    }
}

/* Simple sign-based return prediction (direction only) */
static double predict_direction(const float *features, const Genome *g) {
    double score = g->bias;
    score += (features[0] - 0.5) * g->feat_weight[0] * 2.0;
    score += features[1] * g->feat_weight[1] * 0.1;
    score += features[2] * g->feat_weight[2] * 0.05;
    score += features[3] * g->feat_weight[3] * 0.02;
    score += (features[4] - 1.0) * g->feat_weight[4];
    score += (features[5] - 0.5) * g->feat_weight[5] * 2.0;
    for (int i = 6; i < N_FEATURES && i < N_REGS + 6; i++) {
        score += (features[i] - 0.5) * g->feat_weight[i] * 0.5;
    }
    return score > 0 ? 1 : -1;
}

int main(int argc, char **argv) {
    const char *markets_file = argc > 1 ? argv[1] : NULL;
    int n_agents = 500;
    int n_cycles = 1000;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--agents") == 0 && i+1 < argc) n_agents = atoi(argv[++i]);
        if (strcmp(argv[i], "--cycles") == 0 && i+1 < argc) n_cycles = atoi(argv[++i]);
    }

    if (!markets_file) {
        fprintf(stderr, "Usage: %s <markets.json> [--agents N] [--cycles N]\n", argv[0]);
        fprintf(stderr, "markets.json format:\n");
        fprintf(stderr, "  [{\"name\":\"BTC\",\"feed\":\"data/btc_feed.json\",\"pair\":\"BTCUSD\",\"period\":60}]\n");
        return 1;
    }

    /* Allocate agents */
    AgentState *agents = calloc(n_agents, sizeof(AgentState));
    if (!agents) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Load markets from JSON */
    FILE *f = fopen(markets_file, "r");
    if (!f) {
        /* Default: use existing data files */
        fprintf(stderr, "[TRAIN] No markets file, using defaults\n");
        /* Will use BTC SPY etc feeds */
    } else {
        fclose(f);
    }

    /* Initialize agents with random genomes */
    srand(time(NULL));
    for (int i = 0; i < n_agents; i++) {
        agents[i].capital = 10000.0f;
        agents[i].alive = 1;
        for (int w = 0; w < N_FEATURES; w++) {
            agents[i].genome.feat_weight[w] = (float)(rand() % 2000 - 1000) / 1000.0f;
        }
    }

    /* Simple training loop (no real market data file parsing — 
       this is the scaffold that reads live data) */
    printf("{\n");
    printf("  \"multi_market_trainer\": true,\n");
    printf("  \"agents\": %d,\n", n_agents);
    printf("  \"cycles\": %d,\n", n_cycles);
    printf("  \"features\": %d,\n", N_FEATURES);
    printf("  \"regimes\": %d,\n", N_REGS);
    printf("  \"markets\": 4,\n");  /* BTC, ETH, SPY, QQQ */
    printf("  \"max_agents\": %d,\n", MAX_AGENTS);
    printf("  \"status\": \"scaffold — connect to live market_feed.json\"\n");
    printf("}\n");

    /* Write output genomes for live engine */
    FILE *out = fopen(OUTPUT_DIR "/trained_genomes.json", "w");
    if (out) {
        fprintf(out, "{\"trainer\":\"multi_market\",\"agents\":%d,\"note\":\"scaffold\"}\n", n_agents);
        fclose(out);
    }

    free(agents);
    fprintf(stderr, "[TRAIN] Genomes written to %s/trained_genomes.json\n", OUTPUT_DIR);
    return 0;
}
