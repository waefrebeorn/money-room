/**
 * paper_mode_sync.c — Sync evolved paper genomes to live engine
 * 
 * Reads paper engine's state file, extracts best genomes,
 * writes them as multi-market .bin files for live engine.
 * 
 * Compile: gcc -O2 -o paper_mode_sync paper_mode_sync.c -lm -I.
 * Usage:   ./paper_mode_sync [--force]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include "types.h"

#define PAPER_STATE  "/home/wubu2/.hermes/pm_logs/c_room/room_state_paper.bin"
#define GENOME_DIR   "/home/wubu2/money-room/data/multi_market"
#define BACKUP_DIR   "/home/wubu2/.hermes/pm_logs/genome_backups"
#define N_AGENTS     2500

int main(int argc, char **argv) {
    int force = (argc > 1 && strcmp(argv[1], "--force") == 0);
    
    // Read paper state
    FILE *f = fopen(PAPER_STATE, "rb");
    if (!f) { fprintf(stderr, "[SYNC] No paper state at %s\n", PAPER_STATE); return 1; }
    
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    
    // Read entire state
    void *buf = malloc(sz);
    if (!buf) { fclose(f); return 1; }
    size_t nread = fread(buf, 1, sz, f);
    fclose(f);
    
    if (nread != (size_t)sz) { free(buf); return 1; }
    
    // The RoomState starts with cycle (int), then expanded_cycle_log, then votes, then agents...
    // Extract cycle number from start of state
    int cycle = 0;
    if (sz >= 4) cycle = *(int*)buf;
    
    printf("[SYNC] Paper state: %ld bytes, cycle=%d\n", sz, cycle);
    
    // Find agents array — after votes[MAX_AGENTS]
    // votes is at offset after cycle+expanded fields, agents at a fixed offset
    // We need RoomState layout, but we can estimate: first 4 bytes = cycle
    // Read all Genome structs from the state file by scanning for valid patterns
    
    // Each AgentState is ~1192 bytes (Genome=712 + other fields). 
    // MAX_AGENTS=2500, so agents array is at some offset.
    // Simplest approach: allocate RoomState and cast
    RoomState *state = (RoomState*)buf;
    
    // Count alive
    int alive = 0;
    int total_trades = 0, total_wins = 0;
    float total_cap = 0;
    int scored = 0;
    
    // Score each agent by win_rate * trades_penalty
    typedef struct { int idx; float score; float wr; int trades; float cap; } AgentScore;
    AgentScore scores[N_AGENTS];
    
    for (int i = 0; i < N_AGENTS; i++) {
        const AgentState *a = &state->agents[i];
        scores[i].idx = i;
        scores[i].cap = a->capital;
        scores[i].trades = a->trades;
        scores[i].wr = a->trades > 0 ? (float)a->wins / a->trades : 0.5f;
        float dd_penalty = 1.0f / (1.0f + a->max_drawdown * 2.0f);
        scores[i].score = a->alive ? scores[i].wr * dd_penalty * logf(1.0f + a->trades) : -1;
        
        if (a->alive) {
            alive++;
            total_trades += a->trades;
            total_wins += a->wins;
            total_cap += a->capital;
            scored++;
        }
    }
    // Check if state looks valid
    if (alive < 10) {
        fprintf(stderr, "[SYNC] Only %d alive agents — state may be corrupted or training not started\n", alive);
        if (!force) { free(buf); return 1; }
    }
    
    float avg_cap = alive > 0 ? total_cap / alive : 0;
    float wr = total_trades > 0 ? (float)total_wins / total_trades * 100 : 0;
    printf("[SYNC] Paper result: %d alive, avg cap=$%.2f, WR=%.1f%%\n", alive, avg_cap, wr);
    
    // Sort by score descending
    for (int i = 0; i < N_AGENTS - 1; i++) {
        for (int j = i + 1; j < N_AGENTS; j++) {
            if (scores[j].score > scores[i].score) {
                AgentScore t = scores[i]; scores[i] = scores[j]; scores[j] = t;
            }
        }
    }
    
    // Mkdir for backups
    mkdir(GENOME_DIR, 0755);
    mkdir(BACKUP_DIR, 0755);
    
    // Print top 10
    printf("[SYNC] Top 10 agents:\n");
    for (int i = 0; i < 10 && i < N_AGENTS; i++) {
        if (scores[i].score <= 0) break;
        int idx = scores[i].idx;
        printf("  #%d: cap=$%.2f trades=%d WR=%.1f%% score=%.3f\n",
               idx, scores[i].cap, scores[i].trades, scores[i].wr * 100, scores[i].score);
    }
    
    // Average top agents per market group (since we don't have agent_market in state,
    // we average all top agents into each market genome)
    int top_n = scored / 10;
    if (top_n < 5) top_n = 5;
    if (top_n > 50) top_n = 50;
    
    printf("[SYNC] Averaging top %d agents for genome sync\n", top_n);
    
    // Market list (17 markets)
    const char *market_names[] = {
        "BTC", "SP500", "DOW", "NASDAQ", "FTSE100", "NIKKEI",
        "EURUSD", "GBPUSD", "USDJPY",
        "GOLD", "SILVER", "CRUDE_OIL",
        "DGS10", "VIX",
        "PMARKET", "SPORTS", "WEATHER"
    };
    int n_markets = 17;
    
    mkdir(GENOME_DIR, 0755);
    int synced = 0;
    
    for (int m = 0; m < n_markets; m++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.bin", GENOME_DIR, market_names[m]);
        
        // Backup existing
        char backup[512];
        snprintf(backup, sizeof(backup), "%s/%s_%ld.bin", BACKUP_DIR, market_names[m], (long)time(NULL));
        FILE *ef = fopen(path, "rb");
        if (ef) {
            FILE *bf = fopen(backup, "wb");
            if (bf) {
                char tmp[4096]; size_t n;
                while ((n = fread(tmp, 1, sizeof(tmp), ef)) > 0) fwrite(tmp, 1, n, bf);
                fclose(bf);
            }
            fclose(ef);
        }
        
        // Average top_n genomes
        Genome avg;
        memset(&avg, 0, sizeof(Genome));
        
        for (int k = 0; k < top_n; k++) {
            int idx = scores[k].idx;
            if (scores[k].score <= 0) continue;
            const Genome *g = &state->agents[idx].genome;
            avg.bias += g->bias;
            avg.position_size += g->position_size;
            avg.conviction_threshold += g->conviction_threshold;
            avg.risk_tolerance += g->risk_tolerance;
            avg.lie_sensitivity += g->lie_sensitivity;
            avg.herd_antipathy += g->herd_antipathy;
            avg.stop_loss_pct += g->stop_loss_pct;
            avg.take_profit_pct += g->take_profit_pct;
            avg.min_edge_pct += g->min_edge_pct;
            avg.time_horizon += g->time_horizon;
            avg.mean_reversion_bias += g->mean_reversion_bias;
            avg.learning_rate += g->learning_rate;
            for (int w = 0; w < N_FEATURES; w++) avg.feat_weight[w] += g->feat_weight[w];
            for (int r = 0; r < N_REGS; r++) {
                avg.regime_bias[r] += g->regime_bias[r];
                for (int w = 0; w < N_FEATURES; w++) avg.regime_weight[r][w] += g->regime_weight[r][w];
            }
        }
        
        // Normalize
        int count = 0;
        for (int k = 0; k < top_n; k++) if (scores[k].score > 0) count++;
        if (count < 1) count = 1;
        
        avg.bias /= count;
        avg.position_size /= count;
        avg.conviction_threshold /= count;
        avg.risk_tolerance /= count;
        avg.lie_sensitivity /= count;
        avg.herd_antipathy /= count;
        avg.stop_loss_pct /= count;
        avg.take_profit_pct /= count;
        avg.min_edge_pct /= count;
        avg.time_horizon /= count;
        avg.mean_reversion_bias /= count;
        avg.learning_rate /= count;
        for (int w = 0; w < N_FEATURES; w++) avg.feat_weight[w] /= count;
        for (int r = 0; r < N_REGS; r++) {
            avg.regime_bias[r] /= count;
            for (int w = 0; w < N_FEATURES; w++) avg.regime_weight[r][w] /= count;
        }
        
        // Clamp values to sane ranges
        if (avg.position_size < 0.01f) avg.position_size = 0.01f;
        if (avg.position_size > 0.5f) avg.position_size = 0.5f;
        if (avg.conviction_threshold < 0.01f) avg.conviction_threshold = 0.01f;
        if (avg.conviction_threshold > 0.5f) avg.conviction_threshold = 0.5f;
        if (avg.bias < -1.0f) avg.bias = -1.0f;
        if (avg.bias > 1.0f) avg.bias = 1.0f;
        for (int w = 0; w < N_FEATURES; w++) {
            if (avg.feat_weight[w] < -2.0f) avg.feat_weight[w] = -2.0f;
            if (avg.feat_weight[w] > 2.0f) avg.feat_weight[w] = 2.0f;
        }
        
        // Write
        FILE *of = fopen(path, "wb");
        if (!of) { fprintf(stderr, "[SYNC] Cannot write %s\n", path); continue; }
        fwrite(&avg, sizeof(Genome), 1, of);
        fclose(of);
        
        printf("[SYNC] %s.bin: %d agents averaged, WR range %.1f%%-%.1f%%\n",
               market_names[m], count,
               scores[count-1].wr * 100, scores[0].wr * 100);
        synced++;
    }
    
    printf("[SYNC] Synced %d/%d market genomes. Top WR=%.1f%%\n",
           synced, n_markets, scores[0].wr * 100);
    
    free(buf);
    return 0;
}
