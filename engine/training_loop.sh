#!/bin/bash
# training_loop.sh — Multi-market training pipeline (delegates to multi_training_loop.sh)
# Runs every 60 min across 17 markets. Overrides old BTC-only training.
set -e
exec /home/wubu2/money-room/engine/multi_training_loop.sh
