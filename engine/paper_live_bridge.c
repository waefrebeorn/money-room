/**
 * paper_live_bridge.c — Daytime paper trading with real-time stats
 * 
 * Reads live market_feed.json (same as engine), runs paper-mode 
 * trading, outputs stats every 60s to docs/data/paper_stats.json.
 * 
 * Compile:
 *   gcc -O2 -o paper_live_bridge paper_live_bridge.c -ljansson -lm -I.
 * 
 * Usage:
 *   ./paper_live_bridge              # Single cycle
 *   ./paper_live_bridge --continuous # Run continuously (60s cycles)
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <jansson.h>
#include "types.h"

#define FEED_PATH       "/home/wubu2/.hermes/pm_logs/c_room/market_feed.json"
#define STATS_OUT       "/home/wubu2/money-room/docs/data/paper_stats.json"
#define LEADERBOARD_OUT "/home/wubu2/money-room/data/paper_leaderboard.json"
#define GENOME_PATH     "/home/wubu2/money-room/data/multi_market/BTC.bin"
#define N_AGENTS        2500
#define SEED_CAPITAL     50.0f

// ── Agent state ──
typedef struct {
    bool    alive;
    float   capital;
    float   peak_capital;
    int     trades;
    int     wins;
    int     losses;
    float   total_pnl;
    float   win_rate;
    Genome  genome;
} PaperAgent;

static PaperAgent g_agents[N_AGENTS];
static int g_cycle = 0;
static int g_total_trades = 0;
static int g_total_wins = 0;
static int g_active = 0;
static int g_warmup_cycles = 0;
const float MAX_POSITION_FRACTION = 0.01f;  // Max 1% per trade
const float MAX_CAPITAL = 1000.0f;           // Reset agents above $1000 (unrealistic)

// ── Load evolved genome or use random ──
static int load_trained_genome(Genome *out) {
    FILE *f = fopen(GENOME_PATH, "rb");
    if (!f) { printf("[PAPER] No trained genome found at %s, using random\n", GENOME_PATH); return -1; }
    if (fread(out, sizeof(Genome), 1, f) != 1) {
        printf("[PAPER] Failed to read genome from %s\n", GENOME_PATH);
        fclose(f); return -1;
    }
    fclose(f);
    printf("[PAPER] Loaded trained genome from %s\n", GENOME_PATH);
    return 0;
}

// ── Initialize agents ──
static void init_agents(void) {
    Genome trained;
    int has_trained = load_trained_genome(&trained);
    
    for (int i = 0; i < N_AGENTS; i++) {
        g_agents[i].alive = true;
        g_agents[i].capital = SEED_CAPITAL;
        g_agents[i].peak_capital = SEED_CAPITAL;
        g_agents[i].trades = 0;
        g_agents[i].wins = 0;
        g_agents[i].losses = 0;
        g_agents[i].total_pnl = 0.0f;
        g_agents[i].win_rate = 0.5f;
        
        if (has_trained == 0) {
            // Start from trained genome + small noise for diversity
            g_agents[i].genome = trained;
            g_agents[i].genome.bias += ((float)rand() / RAND_MAX - 0.5f) * 0.02f;
            g_agents[i].genome.position_size *= (0.8f + (float)rand() / RAND_MAX * 0.4f);
            for (int w = 0; w < N_FEATURES; w++)
                g_agents[i].genome.feat_weight[w] += ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
        } else {
            // Conservative random genome
            g_agents[i].genome.bias = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
            g_agents[i].genome.position_size = 0.005f + (float)rand() / RAND_MAX * 0.02f;
            g_agents[i].genome.conviction_threshold = 0.15f + (float)rand() / RAND_MAX * 0.35f;
            g_agents[i].genome.stop_loss_pct = 0.05f + (float)rand() / RAND_MAX * 0.15f;
            g_agents[i].genome.take_profit_pct = 0.05f + (float)rand() / RAND_MAX * 0.25f;
            for (int w = 0; w < N_FEATURES; w++)
                g_agents[i].genome.feat_weight[w] = ((float)rand() / RAND_MAX - 0.5f) * 0.5f;
        }
    }
    printf("[PAPER] Initialized %d agents (source: %s)\n", N_AGENTS, has_trained == 0 ? "trained" : "random");
}

// ── Parse market_feed.json ──
static int read_feed(MarketTick *tick) {
    FILE *f = fopen(FEED_PATH, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f);
    if (sz < 10) { fclose(f); return -1; }
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = '\0';
    
    json_error_t err;
    json_t *root = json_loads(buf, 0, &err);
    free(buf);
    if (!root) return -1;
    
    memset(tick, 0, sizeof(MarketTick));
    
    json_t *jts = json_object_get(root, "window_ts");
    json_t *jclose = json_object_get(root, "close");
    json_t *jopen = json_object_get(root, "open");
    json_t *jhigh = json_object_get(root, "high");
    json_t *jlow = json_object_get(root, "low");
    json_t *jvol = json_object_get(root, "volume");
    json_t *jfng = json_object_get(root, "fear_greed");
    
    if (jts) tick->window_ts = json_integer_value(jts);
    if (jclose) tick->close = (float)json_number_value(jclose);
    if (jopen) tick->open = (float)json_number_value(jopen);
    if (jhigh) tick->high = (float)json_number_value(jhigh);
    if (jlow) tick->low = (float)json_number_value(jlow);
    if (jvol) tick->volume = (float)json_number_value(jvol);
    if (jfng) tick->fear_greed = (float)json_number_value(jfng);
    tick->market_type = MARKET_CRYPTO;
    strncpy(tick->asset, "BTC", sizeof(tick->asset) - 1);
    
    json_decref(root);
    return (tick->close > 0) ? 0 : -1;
}

// ── Simple features from tick ──
static void compute_features(const MarketTick *tick, float *feats) {
    for (int i = 0; i < N_FEATURES; i++) feats[i] = 0.5f;
    feats[0] = (tick->open > 0) ? (tick->close - tick->open) / tick->open * 100.0f : 0;
    feats[1] = 0;  // micro_momentum placeholder (would need history)
    feats[10] = 0.5f;  // regime
    feats[11] = tick->fear_greed / 100.0f;  // fear_greed_norm
    feats[12] = 0.5f;  // herd_consensus (not tracked here)
}

// ── Agent vote ──
static int agent_vote(const PaperAgent *a, const float *feats, bool *dir, float *conv) {
    double score = a->genome.bias;
    for (int w = 0; w < N_FEATURES; w++) score += (feats[w] - 0.5f) * a->genome.feat_weight[w];
    double sig = 1.0 / (1.0 + exp(-score));
    *conv = (float)fabs(sig - 0.5) * 2.0f;
    if (*conv < a->genome.conviction_threshold) return 0;
    *dir = sig > 0.5;
    return 1;
}

// ── Write stats JSON ──
static void write_stats(void) {
    // Compute aggregate stats
    float total_cap = 0, total_pnl = 0;
    int alive = 0;
    float best_cap = 0, worst_cap = 1e9;
    int best_idx = 0, worst_idx = 0;
    float wr_sum = 0;
    
    for (int i = 0; i < N_AGENTS; i++) {
        if (!g_agents[i].alive) continue;
        alive++;
        total_cap += g_agents[i].capital;
        total_pnl += g_agents[i].total_pnl;
        wr_sum += g_agents[i].win_rate;
        if (g_agents[i].capital > best_cap) { best_cap = g_agents[i].capital; best_idx = i; }
        if (g_agents[i].capital < worst_cap) { worst_cap = g_agents[i].capital; worst_idx = i; }
    }
    
    float avg_cap = alive > 0 ? total_cap / alive : 0;
    float avg_wr = alive > 0 ? wr_sum / alive : 0;
    float room_pnl = (total_cap - SEED_CAPITAL * alive) / (SEED_CAPITAL * alive) * 100.0f;
    g_active = alive;
    
    json_t *stats = json_object();
    json_object_set_new(stats, "timestamp", json_integer((int64_t)time(NULL)));
    json_object_set_new(stats, "cycle", json_integer(g_cycle));
    json_object_set_new(stats, "active_agents", json_integer(alive));
    json_object_set_new(stats, "total_agents", json_integer(N_AGENTS));
    json_object_set_new(stats, "avg_capital", json_real(avg_cap));
    json_object_set_new(stats, "total_trades", json_integer(g_total_trades));
    json_object_set_new(stats, "total_wins", json_integer(g_total_wins));
    json_object_set_new(stats, "avg_win_rate", json_real(avg_wr));
    json_object_set_new(stats, "room_pnl_pct", json_real(room_pnl));
    json_object_set_new(stats, "sharpe_ratio", json_real(0.0));  // placeholder
    json_object_set_new(stats, "best_capital", json_real(best_cap));
    json_object_set_new(stats, "best_agent", json_integer(best_idx));
    json_object_set_new(stats, "worst_capital", json_real(worst_cap));
    json_object_set_new(stats, "worst_agent", json_integer(worst_idx));
    
    // Top 10 leaderboard
    json_t *top10 = json_array();
    // Simple selection sort for top 10
    int sorted[N_AGENTS];
    for (int i = 0; i < N_AGENTS; i++) sorted[i] = i;
    for (int i = 0; i < N_AGENTS - 1 && i < 50; i++) {
        for (int j = i + 1; j < N_AGENTS; j++) {
            if (g_agents[sorted[j]].capital > g_agents[sorted[i]].capital) {
                int t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
            }
        }
    }
    for (int i = 0; i < 10 && i < N_AGENTS; i++) {
        int id = sorted[i];
        json_t *entry = json_object();
        json_object_set_new(entry, "id", json_integer(id));
        json_object_set_new(entry, "capital", json_real(g_agents[id].capital));
        json_object_set_new(entry, "trades", json_integer(g_agents[id].trades));
        json_object_set_new(entry, "wins", json_integer(g_agents[id].wins));
        json_object_set_new(entry, "win_rate", json_real(g_agents[id].win_rate));
        json_object_set_new(entry, "total_pnl", json_real(g_agents[id].total_pnl));
        json_array_append_new(top10, entry);
    }
    json_object_set_new(stats, "leaderboard", top10);
    
    // Distribution (capital deciles)
    json_t *dist = json_array();
    // Count agents in capital ranges
    float ranges[] = {0, 10, 25, 50, 75, 100, 150, 200, 500, 1000, 1e9};
    for (int r = 0; r < 10; r++) {
        int cnt = 0;
        for (int i = 0; i < N_AGENTS; i++) {
            if (g_agents[i].alive && g_agents[i].capital >= ranges[r] && g_agents[i].capital < ranges[r+1])
                cnt++;
        }
        json_array_append_new(dist, json_integer(cnt));
    }
    json_object_set_new(stats, "capital_distribution", dist);
    
    // Write
    char *out = json_dumps(stats, JSON_INDENT(2));
    if (out) {
        FILE *f = fopen(STATS_OUT, "w");
        if (f) { fputs(out, f); fclose(f); }
        free(out);
    }
    json_decref(stats);
}

int main(int argc, char **argv) {
    int continuous = (argc > 1 && strcmp(argv[1], "--continuous") == 0);
    
    setbuf(stdout, NULL);  // Unbuffer stdout for real-time log
    
    srand((unsigned)time(NULL));
    init_agents();
    
    int64_t last_ts = 0;
    int run_cycles = continuous ? 1000000 : 1;
    
    printf("[PAPER] Starting paper live bridge (%s mode)\n",
           continuous ? "continuous" : "single-cycle");
    
    // Debug: verify first 3 agents
    printf("[PAPER] Debug: agent[0] cap=%.2f pos=%.4f bias=%.4f\n",
           g_agents[0].capital, g_agents[0].genome.position_size, g_agents[0].genome.bias);
    printf("[PAPER] Debug: agent[1] cap=%.2f pos=%.4f\n",
           g_agents[1].capital, g_agents[1].genome.position_size);
    printf("[PAPER] Debug: agent[2] cap=%.2f pos=%.4f\n",
           g_agents[2].capital, g_agents[2].genome.position_size);
    
    for (int c = 0; c < run_cycles; c++) {
        MarketTick tick;
        if (read_feed(&tick) != 0) {
            if (c == 0) { printf("[PAPER] No feed yet\n"); }
            if (continuous) sleep(5);
            continue;
        }
        
        // ── Warmup: bypass duplicate-check during warmup so we always get past it ──
        g_warmup_cycles++;
        if (g_warmup_cycles < 2) {
            last_ts = tick.window_ts;
            write_stats();
            printf("[PAPER] warmup=%d\n", g_warmup_cycles);
            if (continuous) { sleep(1); continue; } else break;
        }
        
        // Skip duplicate timestamps but STILL write stats to keep website current
        if (tick.window_ts == last_ts) {
            if (continuous) {
                if (c % 60 == 0) write_stats();
                sleep(1);
            }
            continue;
        }
        last_ts = tick.window_ts;
        
        g_cycle++;
        
        // Write stats immediately on first cycle for debug
        if (g_cycle == 1) write_stats();
        
        // Compute features
        float feats[N_FEATURES];
        compute_features(&tick, feats);
        
        // Run vote
        int voted = 0, up = 0, down = 0;
        float total_conv = 0;
        float room_drawdown = 0;
        
        for (int i = 0; i < N_AGENTS; i++) {
            if (!g_agents[i].alive) continue;
            
            // ── Circuit breaker: reset agents with absurd capital ──
            if (g_agents[i].capital > MAX_CAPITAL) {
                printf("[PAPER] Agent %d reset: cap=$%.0f exceeded limit\n", i, g_agents[i].capital);
                g_agents[i].capital = SEED_CAPITAL;
                g_agents[i].peak_capital = SEED_CAPITAL;
                g_agents[i].trades = 0;
                g_agents[i].wins = 0;
                g_agents[i].losses = 0;
                g_agents[i].total_pnl = 0;
            }
            
            // ── Stop loss: death at 50% drawdown ──
            float dd = (g_agents[i].peak_capital - g_agents[i].capital) / fmax(g_agents[i].peak_capital, 0.01f);
            if (dd > 0.5f) { g_agents[i].alive = false; continue; }
            
            bool dir; float conv;
            if (!agent_vote(&g_agents[i], feats, &dir, &conv)) continue;
            
            voted++;
            if (dir) up++; else down++;
            total_conv += conv;
            
            // Resolve trade: predict direction vs actual
            bool was_up = tick.close >= tick.open;
            bool won = (dir == was_up);
            
            // Fixed position sizing: cap at 1% of capital
            float pos = fmin(g_agents[i].genome.position_size, MAX_POSITION_FRACTION) * g_agents[i].capital;
            float payout = won ? pos * 0.95f : -pos;
            g_agents[i].capital += payout;
            g_agents[i].total_pnl += payout;
            g_agents[i].trades++;
            if (won) { g_agents[i].wins++; g_total_wins++; }
            else { g_agents[i].losses++; }
            g_total_trades++;
            
            // Update win rate
            g_agents[i].win_rate = g_agents[i].trades > 0 ? (float)g_agents[i].wins / g_agents[i].trades : 0.5f;
            
            // Track peak
            if (g_agents[i].capital > g_agents[i].peak_capital)
                g_agents[i].peak_capital = g_agents[i].capital;
        }
        
        // Write stats every 60 cycles (or every cycle in non-continuous)
        if (c % 60 == 0 || !continuous) {
            write_stats();
            printf("[PAPER] cycle=%d agents=%d trades=%d wr=%.1f%% cap=$%.2f\n",
                   g_cycle, g_active, g_total_trades,
                   g_total_trades > 0 ? (float)g_total_wins / g_total_trades * 100 : 0,
                   g_active > 0 ? g_agents[0].capital : 0);
        }
        
        if (!continuous) break;
        
        sleep(1);  // 1s cycle to match live engine
    }
    
    printf("[PAPER] Done. %d cycles, %d trades, %d active agents\n",
           g_cycle, g_total_trades, g_active);
    return 0;
}
