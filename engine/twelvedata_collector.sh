#!/bin/bash
# Wrapper for twelvedata_collector — sources TWELVEDATA_API_KEY
set -a; source "$HOME/.hermes/secrets.env"; set +a
export PATH="$HOME/money-room/engine:$PATH"
cd "$HOME/money-room/engine"
timeout 20 ./twelvedata_collector 2>&1 || echo "[TD] timed out or failed"
