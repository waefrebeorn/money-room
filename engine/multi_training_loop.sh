#!/bin/bash
# multi_training_loop.sh — Full multi-market training pipeline
# Runs every 60 min. Refreshes all market data → trains across 17 markets → distills
set -euo pipefail
cd /home/wubu2/money-room/engine

LOCKFILE="/tmp/multi_training_loop.lock"
[ -f "$LOCKFILE" ] && [ -f /proc/$(cat $LOCKFILE)/status ] && { echo "[MULTI-TRAIN] Already running"; exit 0; }
echo $$ > "$LOCKFILE"
trap 'rm -f "$LOCKFILE"' EXIT

PAPER_STATE="/home/wubu2/.hermes/pm_logs/c_room/room_state_paper.bin"
LIVE_STATE="/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"
LOGFILE="/home/wubu2/.hermes/pm_logs/multi_training_loop.log"
ENGINE_DIR="/home/wubu2/money-room/engine"

log() { echo "[$(date -Iseconds)] $*" >> "$LOGFILE"; echo "$*"; }

log "═══ MULTI-MARKET TRAINING LOOP START ═══"

# Phase 0: Quick data refresh (BTC CSV is always live via 15-min cron)
log "Phase 0: Quick data checks..."
# Ensure weather and sports data exist
[ -f "/home/wubu2/money-room/data/multi_market/weather_data.json" ] || ./weather_collector --days 5 2>&1 | tail -1 >> "$LOGFILE"
[ -f "/home/wubu2/money-room/data/multi_market/sports_data.json" ] || ./sports_collector --days 15 2>&1 | tail -1 >> "$LOGFILE"
log "Phase 0 done."

# Phase 1: Multi-market training (500 agents, 10 epochs — proper evolution)
log "Phase 1: Multi-market training (17 markets, 10 epochs)..."
timeout 600 ./multi_market_trainer --agents 500 --epochs 10 2>&1 | tail -50 >> "$LOGFILE"

log "Phase 1 complete. Checking genome outputs..."
ls -la /home/wubu2/money-room/data/multi_market/*.bin >> "$LOGFILE" 2>&1

# Phase 2: Run paper engine with fresh genomes for proper evolution (not stubs)
log "Phase 2: Paper engine evolution (2500 agents, 10000 cycles)..."
rm -f "$PAPER_STATE" 2>/dev/null
timeout 120 ./room_engine_paper 2>&1 | tail -20 >> "$LOGFILE"
log "Phase 2 complete."

# Phase 3: Distill evolved genomes back
log "Phase 3: Distilling evolved genomes..."
if [ -f "$PAPER_STATE" ] && [ -x ./genome_distiller ]; then
    cp "$PAPER_STATE" "${PAPER_STATE}.trained_$(date +%Y%m%d_%H%M%S)" 2>/dev/null || true
    ./genome_distiller --backup 2>&1 | tail -10 >> "$LOGFILE"
    log "Phase 3: Genomes distilled."
fi

# Phase 4: Report
log "═══ MULTI-MARKET TRAINING REPORT ═══"
echo ""
echo "Multi-Market Training Complete"
echo "  17 markets across 7 market types"
echo "  500 agents × 10 epochs"
echo "  Paper engine: 2500 agents × 10000 cycles"
echo "  BTC CSV: $(wc -l < /home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv) candles"
echo ""

log "═══ MULTI-MARKET TRAINING LOOP DONE ═══"
