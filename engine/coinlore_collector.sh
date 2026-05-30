#!/bin/bash
# coinlore_collector.sh — Wrapper for coinlore_collector binary
# Fetches top 100 coins from CoinLore free API every 60m

set -euo pipefail
DIR="/home/wubu2/money-room/engine"
cd "$DIR"
./coinlore_collector -q 2>/dev/null || ./coinlore_collector
