#!/bin/bash
# coinbase_live.sh — Wrapper for coinbase_live binary
# Fetches latest BTC-USD 1-min candles from Coinbase Exchange every 15m

set -euo pipefail
DIR="/home/wubu2/money-room/engine"
cd "$DIR"
./coinbase_live -q 2>/dev/null || ./coinbase_live
