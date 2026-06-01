#!/bin/bash
# paper_train.sh — Blind-room paper training on historical BTC data
#
# Workflow:
#   1. Clear paper state → rebuild engine with PAPER_MODE
#   2. Run engine → 2500 agents evolve through 722K BTC 1-min candles
#   3. State persists in room_state_paper.bin (separate from LIVE state)
#   4. Rebuild engine without PAPER_MODE → live engine ready
#
# FIXES APPLIED May 30:
#   - 6-consecutive-loss death DISABLED in PAPER_MODE (room_capital.c #ifndef)
#   - Separate state file room_state_paper.bin avoids mmap collision
#     between PAPER (2500 agents) and LIVE (10000 agents) modes

set -euo pipefail

ENGINE_DIR="/home/wubu2/money-room/engine"
ROOM_DIR="/home/wubu2/.hermes/pm_logs/c_room"
ROOM_ENGINE="$ENGINE_DIR/room_engine_paper"  # Paper binary lives in engine dir (not c_room)
STATE_BIN="$ROOM_DIR/room_state_paper.bin"  # Separate from live room_state.bin!
LOG_DIR="/home/wubu2/.hermes/pm_logs/paper_training"
START_TS=$(date +%s)
PIDFILE="/tmp/paper_train.pid"

mkdir -p "$LOG_DIR"

# Check if training is already running
if [ -f "$PIDFILE" ]; then
    OLD_PID=$(cat "$PIDFILE")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "  ⚠️ Paper training already running (PID $OLD_PID). Skipping."
        # Show progress
        PROGRESS_LOG=$(ls -t "$LOG_DIR"/paper_run_*.log 2>/dev/null | head -1)
        if [ -n "$PROGRESS_LOG" ]; then
            LAST_CYCLE=$(grep -o 'cycle=[0-9]*' "$PROGRESS_LOG" | tail -1 | cut -d= -f2)
            echo "  Latest cycle: ${LAST_CYCLE:-unknown} / 722000"
        fi
        exit 0
    else
        echo "  ⚠️ Stale PID $OLD_PID (no longer running). Clearing lock."
        rm -f "$PIDFILE"
    fi
fi

echo ""
echo "  ╔══════════════════════════════════════════════════════╗"
echo "  ║   PAPER TRAINER — Blind-Room Historical Evolution   ║"
echo "  ╚══════════════════════════════════════════════════════╝"
echo ""
echo "  Room:       $ROOM_DIR"
echo "  State:      $STATE_BIN (separate from LIVE)"
echo "  Data:       ~722K BTC 1-min candles (2017-2026)"
echo "  Pace:       5ms/cycle (200 cycles/sec)"
echo "  Agents:     2500"
echo ""

# Check if CSV exists
CSV="/home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv"
if [ ! -f "$CSV" ]; then
    echo "  ❌ ERROR: No BTC CSV at $CSV"
    echo "     Need: btc_1min_latest.csv with columns: ts,open,high,low,close,volume"
    exit 1
fi
CANDLES=$(wc -l < "$CSV")
echo "  ✅ BTC CSV: $CANDLES candles"

# BACKUP: Save evolved state from previous training (if any)
if [ -f "$STATE_BIN" ]; then
    cp "$STATE_BIN" "$LOG_DIR/room_state_before_$(date +%Y%m%d_%H%M%S).bin"
    echo "  ✅ Previous state backed up"
fi

# CLEAR paper state for fresh training
rm -f "$STATE_BIN"
echo "  ✅ Paper state cleared for fresh training"

# Step 1: Build paper engine + distiller
echo ""
echo "  [1/4] Building paper engine + distiller..."
cd "$ENGINE_DIR"
make clean > /dev/null 2>&1
make paper 2>&1 | tail -3
gcc -O2 -o genome_distiller genome_distiller.c -lm 2>&1

# Copy paper engine to room dir for execution
cp room_engine_paper "$ROOM_ENGINE"
echo "  ✅ Paper engine built + deployed ($(ls -la $ROOM_ENGINE | awk '{print $5}') bytes)"

# Step 2: Run paper training
# 722K candles at 5ms/cycle = ~71 min for full run
# Run in background — log to file
echo "  [2/4] Running blind-room training..."
echo "        Starting at $(date)"
echo "        Estimated run time: ~71 min for all 722K candles"

# Run directly from engine dir (it hardcodes ROOM_DIR)
# CRON FIX: Use setsid to fully detach from cron's timeout
# The engine runs independently in the background
setsid "$ROOM_ENGINE" > "$LOG_DIR/paper_run_$(date +%Y%m%d_%H%M%S).log" 2>&1 &
PAPER_PID=$!
echo $PAPER_PID > "$PIDFILE"
echo "        PID: $PAPER_PID (background, detached)"
echo "  ✅ Paper training launched in background"
echo "  ✅ Setting up completion watcher..."

# Launch completion watcher — waits for engine to finish, then distills and seeds live state
cat > /tmp/paper_complete_$$.sh << 'WATCHER'
#!/bin/bash
ENGINE_DIR="/home/wubu2/money-room/engine"
ROOM_DIR="/home/wubu2/.hermes/pm_logs/c_room"
STATE_BIN="/home/wubu2/.hermes/pm_logs/c_room/room_state_paper.bin"
LOG_DIR="/home/wubu2/.hermes/pm_logs/paper_training"
PIDFILE="/tmp/paper_train.pid"
WATCHER_PIDFILE="/tmp/paper_watcher.pid"

echo $$ > "$WATCHER_PIDFILE"

# Wait for paper engine to finish
PAPER_PID=$(cat "$PIDFILE" 2>/dev/null)
if [ -z "$PAPER_PID" ]; then
    echo "[WATCHER] No PID found in $PIDFILE — skipping"
    rm -f "$WATCHER_PIDFILE"
    exit 1
fi

echo "[WATCHER] Waiting for paper engine (PID $PAPER_PID) to complete..."
while kill -0 "$PAPER_PID" 2>/dev/null; do
    sleep 60
done

echo "[WATCHER] Paper engine finished at $(date)"

# Small delay for filesystem sync
sleep 2

# Step 3: Back up evolved state
if [ -f "$STATE_BIN" ]; then
    cp "$STATE_BIN" "$LOG_DIR/room_state_paper_$(date +%Y%m%d_%H%M%S).bin"
    echo "[WATCHER] ✅ Evolved state backed up"
fi

# Step 4: Distill evolved genomes to live state
echo "[WATCHER] Distilling evolved genomes → live state..."
if [ -x "$ENGINE_DIR/genome_distiller" ]; then
    "$ENGINE_DIR/genome_distiller" --backup 2>&1 | tail -5
    echo "[WATCHER] ✅ Live state seeded with evolved genomes"
else
    echo "[WATCHER] ⚠️ genome_distiller not found — skipping distillation"
fi

# Step 5: Clean up pidfiles
rm -f "$PIDFILE" "$WATCHER_PIDFILE"
echo "[WATCHER] ✅ Watcher complete at $(date)"
WATCHER
chmod +x /tmp/paper_complete_$$.sh
setsid /tmp/paper_complete_$$.sh > "$LOG_DIR/watcher_$(date +%Y%m%d_%H%M%S).log" 2>&1 &
echo "        Watcher PID: $! (background, detached)"

echo "  ✅ Cron job completed — engine + watcher running independently"

# Exit immediately — the engine runs detached
# The next cron cycle will find the state in progress
exit 0

# Step 3: Back up evolved state
if [ -f "$STATE_BIN" ]; then
    cp "$STATE_BIN" "$LOG_DIR/room_state_paper_$(date +%Y%m%d_%H%M%S).bin"
    echo "  ✅ Evolved state saved"
fi

# Step 4: Rebuild without PAPER_MODE for live
echo "  [3/4] Distilling evolved genomes → live state..."
"$ENGINE_DIR/genome_distiller" --backup 2>&1 | tail -5
echo "  ✅ Live state seeded with evolved genomes"

# Step 5: Rebuild engine for LIVE_MODE
echo "  [4/5] Rebuilding engine for LIVE_MODE..."
cd "$ENGINE_DIR"
make clean > /dev/null 2>&1
# Build only room_engine (skip world_trainer which has pre-existing errors)
make $(TARGET) 2>&1 | tail -3
echo "  ✅ Live engine rebuilt (10000 agents, 1s/cycle)"

# Step 6: Copy live engine to all rooms
echo "  [5/6] Deploying live engine..."
ROOMS=(
    "/home/wubu2/.hermes/pm_logs/c_room"
    "/home/wubu2/.hermes/pm_logs/rooms/crypto_prices"
    "/home/wubu2/.hermes/pm_logs/rooms/elections"
    "/home/wubu2/.hermes/pm_logs/rooms/polymarket"
    "/home/wubu2/.hermes/pm_logs/rooms/macro"
    "/home/wubu2/.hermes/pm_logs/rooms/momentum"
)
for r in "${ROOMS[@]}"; do
    mkdir -p "$r"
    cp room_engine "$r/room_engine"
done
echo "  [6/6] Deployed to ${#ROOMS[@]} rooms"

ELAPSED=$(( $(date +%s) - START_TS ))
echo ""
echo "  ═══════════════════════════════════════════════════════"
echo "  ✅ PAPER TRAINING COMPLETE — ${ELAPSED}s elapsed"
echo ""
echo "  Paper state: $STATE_BIN"
echo "  Live state:  $ROOM_DIR/room_state.bin (seeded with evolved genomes)"
echo "  Live engine: deployed to ${#ROOMS[@]} rooms"
echo "  Darwin will continue evolution in LIVE mode."
echo ""

# Log summary
echo "$(date +%Y-%m-%d_%H:%M:%S) PAPER_TRAIN: ${ELAPSED}s ${CANDLES}candles" >> "$LOG_DIR/train_history.log"
