# Setup Guide

## Prerequisites

```bash
# System packages — C toolchain + libraries
sudo apt update && sudo apt install -y \
  gcc make libcurl4-openssl-dev libjansson-dev libsqlite3-dev \
  curl jq git

# No Python required. Everything is C.
```

## 1. Clone & Build

```bash
git clone https://github.com/waefrebeorn/money-room.git
cd money-room

# Build everything
cd engine
make all       # Build all C binaries (collectors, engine, tools)
make tools     # Build only tools (pipeline_monitor, data_quality, etc.)
make paper     # Build paper-mode engine for testing

# Quick test — run a single cycle
./room_engine --cycles 1 --paper
```

## 2. Set Up Secrets

```bash
# Create secrets file
touch ~/.hermes/secrets.env
chmod 600 ~/.hermes/secrets.env

# Add API keys (one per line, export format):
#   FINNHUB_API_KEY='your_key'
#   POLYGON_API_KEY='your_key'
#   KALSHI_API_KEY='your_key'
```

## 3. Set Up Crontab

The system runs on system crontab. All collectors and tools are C binaries.

```bash
# View current crontab
crontab -l

# The standard setup includes:
#   * * * * *   cycle_all_rooms    — run engine (every minute)
#   * * * * *   collector_runner fast   — fast collectors (every minute)
#   */5 * * * * pipeline_monitor   — system health (every 5 min)
#   */10 * * * * data_quality      — data validation (every 10 min)
#   */15 * * * * collector_runner normal — normal collectors
#   */30 * * * * collector_runner sports — sports collectors
#   0 * * * *   collector_runner slow   — slow collectors (hourly)
#   0 6 * * *   weather_collector  — weather (daily 6am)
#   0 7 * * *   multi_market_trainer — train 17 genomes (daily 7am)
```

To install the default crontab:

```bash
# Pipe the standard cron entries to `crontab`
cat ../scripts/default_crontab.txt | crontab -
```

## 4. Create Data Directories

```bash
mkdir -p ~/.hermes/pm_logs/{c_room,historical}
mkdir -p ~/.hermes/scripts
```

## 5. Populate Historical Database

```bash
# Populate historical BTC 1-min candles from Coinbase CSV
cd engine
./populate_btc_db  # Loads 734K+ candles into pm_logs/historical/historical.db
```

## 6. Verify Everything

```bash
# Check engine is running
ls -la ~/.hermes/pm_logs/c_room/room_engine_paper  # binary, should be running

# Check data files are being written
ls -la ../data/multi_market/market_summary.json     # should exist

# Check pipeline health
cat ../docs/data/pipeline_status.json                # health check output

# Check data quality
cat ../docs/data/data_quality.json                   # validation results
```

## Production Deployment

- **4GB RAM minimum** (8GB recommended for paper training)
- **4+ CPU cores** (for parallel collectors)
- **SSD storage** (historical.db is 3.6GB+)
- **Linux (Ubuntu 22.04+ or WSL2)**

No web server required — data files in `docs/data/` are served via GitHub Pages.

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| No trades generated | Check `market_feed.json` has volume > 0. `cycle_all_rooms` must be running every minute. |
| Darwin not evolving | Need 100+ trades first. Check `--epochs` is ≥3 on multi_market_trainer. |
| Collector not running | Check `~/hermes/pm_logs/<collector>.log` for errors. Verify `FINNHUB_API_KEY` or other keys are set. |
| Engine crashes | Check `c_room/room_engine_paper` binary is current (compiled with same `types.h`). Rebuild with `make clean && make all`. |
| Stale genome | Paper training writes `.bin` files to `data/multi_market/`. Hot-reload picks them up within 1000 cycles. |
