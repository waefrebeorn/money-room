/**
 * survival_stats.c — P26: Survival Analysis for Darwin-Evolving Genomes
 * 
 * Reads RoomState from mmap'd file, computes survival statistics:
 * - Agent age (cycles since last cull)
 * - Generation tracking (epoch cycle stamps)
 * - Survival rate per epoch
 * - Win-rate persistence (WR variance across epochs)
 * 
 * Compile: gcc -O2 -o survival_stats survival_stats.c -lm
 * Run: ./survival_stats path/to/room_state.bin
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "types.h"

static RoomState *map_state(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return NULL; }
    size_t sz = sizeof(RoomState);
    RoomState *s = (RoomState*)mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (s == MAP_FAILED) { perror("mmap"); return NULL; }
    return s;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "room_state.bin";
    RoomState *state = map_state(path);
    if (!state) return 1;

    printf("═══ SURVIVAL ANALYSIS ═══\n");
    printf("Cycle: %d  Trade count: %d  Live agents: %d/%d\n\n",
           state->cycle, state->trade_count,
           state->stats.active_agents, MAX_AGENTS);

    // ── Agent age distribution ──
    int age_buckets[10] = {0};  // 0-9 trades, 10-19, ..., 90+
    int bankrupt = 0, never_traded = 0;
    float wr_sum = 0, wr_sq_sum = 0;
    int wr_count = 0;
    float max_wr = 0, min_wr = 1;

    for (int i = 0; i < MAX_AGENTS; i++) {
        AgentState *a = &state->agents[i];
        if (!a->alive) continue;
        int trades = a->trades;
        if (trades == 0) { never_traded++; continue; }
        if (a->capital <= 0) { bankrupt++; continue; }

        int bucket = trades / 10;
        if (bucket >= 10) bucket = 9;
        age_buckets[bucket]++;

        float wr = (float)a->wins / trades;
        wr_sum += wr;
        wr_sq_sum += wr * wr;
        wr_count++;
        if (wr > max_wr) max_wr = wr;
        if (wr < min_wr) min_wr = wr;
    }

    printf("─ Agent Age Distribution (by trades executed) ─\n");
    printf("  0-9:    %5d\n", age_buckets[0]);
    printf("  10-19:  %5d\n", age_buckets[1]);
    printf("  20-29:  %5d\n", age_buckets[2]);
    printf("  30-39:  %5d\n", age_buckets[3]);
    printf("  40-49:  %5d\n", age_buckets[4]);
    printf("  50-59:  %5d\n", age_buckets[5]);
    printf("  60-69:  %5d\n", age_buckets[6]);
    printf("  70-79:  %5d\n", age_buckets[7]);
    printf("  80-89:  %5d\n", age_buckets[8]);
    printf("  90+:    %5d\n", age_buckets[9]);
    printf("  Never traded: %d  Bankrupt: %d\n\n", never_traded, bankrupt);

    if (wr_count > 0) {
        float mean_wr = wr_sum / wr_count;
        float var_wr = wr_sq_sum / wr_count - mean_wr * mean_wr;
        if (var_wr < 0) var_wr = 0;
        printf("─ Win Rate Distribution ─\n");
        printf("  Mean:    %.2f%%\n", mean_wr * 100);
        printf("  Stddev:  %.2f%%\n", sqrtf(var_wr) * 100);
        printf("  Max:     %.2f%%\n", max_wr * 100);
        printf("  Min:     %.2f%%\n", min_wr * 100);
        printf("  Range:   %.2f%%\n", (max_wr - min_wr) * 100);
    }

    // ── Darwin epoch stats ──
    printf("\n─ Darwin Evolution ─\n");
    printf("  Epoch:       %d\n", state->darwin.epoch);
    printf("  Culled:      %d\n", state->darwin.culled);
    printf("  Cloned:      %d\n", state->darwin.cloned);
    printf("  Mut rate:    %.4f\n", state->darwin.mutation_rate);

    // ── Capital distribution ──
    float cap_sum = 0, cap_sq = 0;
    int cap_count = 0;
    float max_cap = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!state->agents[i].alive) continue;
        float c = state->agents[i].capital;
        cap_sum += c;
        cap_sq += c * c;
        cap_count++;
        if (c > max_cap) max_cap = c;
    }
    if (cap_count > 0) {
        float mean_cap = cap_sum / cap_count;
        float var_cap = cap_sq / cap_count - mean_cap * mean_cap;
        if (var_cap < 0) var_cap = 0;
        printf("\n─ Capital Distribution ─\n");
        printf("  Mean:     $%.2f\n", mean_cap);
        printf("  Stddev:   $%.2f\n", sqrtf(var_cap));
        printf("  Max:      $%.2f\n", max_cap);
        printf("  Total:    $%.2f\n", cap_sum);
    }

    printf("\n═══ P26 VERDICT ═══\n");
    printf("Agent survival analysis: live=%d/%d dead=%d\n",
           state->stats.active_agents, MAX_AGENTS, MAX_AGENTS - state->stats.active_agents);
    printf("Survival rate: %.1f%%\n",
           100.0f * state->stats.active_agents / MAX_AGENTS);
    printf("Bankruptcy rate: %.1f%%\n",
           100.0f * bankrupt / MAX_AGENTS);

    munmap(state, sizeof(RoomState));
    return 0;
}
