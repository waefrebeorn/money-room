#!/bin/bash
# tvl_collector.sh — Wrapper for tvl_collector binary
# Fetches DefiLlama TVL by chain data every 60m

set -euo pipefail
DIR="/home/wubu2/money-room/engine"
cd "$DIR"
./tvl_collector -q 2>/dev/null || ./tvl_collector
