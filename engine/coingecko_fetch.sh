#!/bin/bash
# CoinGecko BTC daily OHLC collector — uses absolute path
timeout 30 /home/wubu2/money-room/engine/coingecko_collector 2>&1 || echo "[CG] timed out"
