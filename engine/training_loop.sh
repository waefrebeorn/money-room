#!/bin/bash
# training_loop.sh — Full training pipeline: paper → distill → live → cycle
# Runs every 30 min. Generates paper_state.bin, distills to live, cycles live engine.
set -e
cd /home/wubu2/money-room/engine

# Lock
LOCKFILE="/tmp/training_loop.lock"
[ -f "$LOCKFILE" ] && [ -f /proc/$(cat $LOCKFILE)/status ] && exit 0
echo $$ > "$LOCKFILE"
trap 'rm -f "$LOCKFILE"' EXIT

PAPER_STATE="/home/wubu2/.hermes/pm_logs/c_room/room_state_paper.bin"
LIVE_STATE="/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"
LOGFILE="/home/wubu2/.hermes/pm_logs/training_loop.log"

log() { echo "[$(date -Iseconds)] $*" >> "$LOGFILE"; }

log "=== TRAINING LOOP START ==="

# Phase 1: Paper training (short run)
if [ ! -f "$PAPER_STATE" ] || [ $(stat -c%s "$PAPER_STATE" 2>/dev/null) -lt 1000000 ]; then
    log "Phase 1: Paper training..."
    timeout 60 ./room_engine_paper 2>&1 | tail -3 >> "$LOGFILE"
    log "Paper state: $(stat -c%s $PAPER_STATE 2>/dev/null) bytes"
fi

# Phase 2: Distill to live
if [ -f "$PAPER_STATE" ] && [ $(stat -c%s "$PAPER_STATE") -gt 50000000 ]; then
    log "Phase 2: Genome distillation..."
    ./genome_distiller --backup 2>&1 | tail -3 >> "$LOGFILE"
fi

# Phase 3: Cycle live engine (3 cycles)
log "Phase 3: Live engine cycling..."
for i in 1 2 3; do
    timeout 5 ./room_engine 2>&1 | grep "Shutdown" >> "$LOGFILE"
done

log "=== TRAINING LOOP DONE ==="
