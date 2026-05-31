#!/bin/bash
# multi_training_loop.sh — Full multi-market training pipeline
# Runs every 60 min. Refreshes all market data → trains across 17 markets → distills → live engine cycle
set -e
cd /home/wubu2/money-room/engine

LOCKFILE="/tmp/multi_training_loop.lock"
[ -f "$LOCKFILE" ] && [ -f /proc/$(cat $LOCKFILE)/status ] && { echo "[MULTI-TRAIN] Already running"; exit 0; }
echo $$ > "$LOCKFILE"
trap 'rm -f "$LOCKFILE"' EXIT

PAPER_STATE="/home/wubu2/.hermes/pm_logs/c_room/room_state_paper.bin"
LIVE_STATE="/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"
LOGFILE="/home/wubu2/.hermes/pm_logs/multi_training_loop.log"

log() { echo "[$(date -Iseconds)] $*" >> "$LOGFILE"; echo "$*"; }

log "═══ MULTI-MARKET TRAINING LOOP START ═══"

# Phase 0: Refresh data collectors
log "Phase 0: Refreshing market data..."

# Weather (8 cities, 365 days)
./weather_collector --days 365 2>&1 | tail -1 >> "$LOGFILE"

# Sports (7 leagues, 15 days of games)
./sports_collector --days 15 2>&1 | tail -1 >> "$LOGFILE"

log "Phase 0 complete."

# Phase 1: Multi-market training (500 agents, 3 epochs → 17 markets)
log "Phase 1: Multi-market training (17 markets)..."
timeout 300 ./multi_market_trainer --agents 500 --epochs 3 2>&1 | tail -30 >> "$LOGFILE"

# Count total trades
TOTAL_TRADES=$(grep "Total:" "$LOGFILE" | tail -1 | grep -oP '\d+' | tail -1)
log "Phase 1 complete: $TOTAL_TRADES trades across 17 markets."

# Phase 2: Distill best multi-market genomes to paper state
if [ -f "/home/wubu2/money-room/data/multi_market/BTC.bin" ]; then
    log "Phase 2: Genome distillation..."
    # Copy best genomes from each market into engine-compatible format
    # The genome_distiller reads these and creates a merged population
    
    # First, run the existing BTC paper training for a quick cycle
    if [ ! -f "$PAPER_STATE" ] || [ $(stat -c%s "$PAPER_STATE" 2>/dev/null) -lt 1000000 ]; then
        log "Paper training kickstart..."
        timeout 60 ./room_engine_paper 2>&1 | tail -3 >> "$LOGFILE"
    fi
    
    # Then distill multi-market genomes into engine
    ./genome_distiller --multi-market 2>&1 | tail -5 >> "$LOGFILE"
fi

# Phase 3: Cycle live engine (5 cycles)
log "Phase 3: Live engine cycling..."
for i in 1 2 3 4 5; do
    timeout 5 ./room_engine 2>&1 | grep "Shutdown" >> "$LOGFILE" || true
done

# Phase 4: Report
log "═══ MULTI-MARKET TRAINING REPORT ═══"
echo ""
echo "Multi-Market Training Complete"
echo "  17 markets across 7 market types"
echo "  $(grep -c 'Best:' "$LOGFILE" | head -1) agents evolved"
echo "  $TOTAL_TRADES total trades"
echo ""
echo "Best agents by market:"
grep "Best:" "$LOGFILE" | tail -20
echo ""

log "═══ MULTI-MARKET TRAINING LOOP DONE ═══"
