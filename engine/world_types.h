/**
 * world_types.h — World Trainer Simulation-Specific Types
 * 
 * Extends the shared types.h with simulation-specific fields
 * for the world_trainer.c curriculum training engine.
 */
#ifndef WORLD_TYPES_H
#define WORLD_TYPES_H

#include "types.h"

// ── Curriculum phases ──
#define CURR_NOISE  0
#define CURR_TREND  1
#define CURR_REGIME 2
#define CURR_FULL   3
#define CURR_COUNT  4

// ── Agent archetypes ──
#define ARCH_TRADER      0
#define ARCH_BETTOR      1
#define ARCH_SPECULATOR  2
#define ARCH_HEDGER      3
#define ARCH_NOISE       4
#define ARCH_COUNT       5

#endif // WORLD_TYPES_H
