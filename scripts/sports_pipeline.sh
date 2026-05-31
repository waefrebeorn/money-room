#!/bin/bash
# Sports data pipeline — runs all sports collectors in order
ENGINE_DIR="/home/wubu2/money-room/engine"

echo "[SPORTS-PIPELINE] $(date -u '+%Y-%m-%d %H:%M:%S UTC')"

# Step 1: Collect sports outcomes (30 days back)
echo "=== Step 1: Sports Collector ==="
"$ENGINE_DIR/sports_collector" --days 30 2>&1

# Step 2: Team stats from DB
echo "=== Step 2: Team Stats ==="
"$ENGINE_DIR/team_stats_calculator" 2>&1

# Step 3: Game timing
echo "=== Step 3: Game Timing ==="
"$ENGINE_DIR/game_timing_collector" 2>&1

# Step 4: Head-to-head
echo "=== Step 4: H2H ==="
"$ENGINE_DIR/head2head_analyzer" 2>&1

# Step 5: Injuries
echo "=== Step 5: Injuries ==="
"$ENGINE_DIR/injury_collector" 2>&1

# Step 6: Schedule (3 days ahead)
echo "=== Step 6: Schedule ==="
"$ENGINE_DIR/schedule_collector" --days 3 2>&1

# Step 7: News + Sentiment
echo "=== Step 7: Sports News ==="
"$ENGINE_DIR/sports_news_collector" 2>&1

# Step 8: Generate training features
echo "=== Step 8: Training Features ==="
"$ENGINE_DIR/sports_feature_generator" 2>&1

echo "[SPORTS-PIPELINE] Complete — $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
