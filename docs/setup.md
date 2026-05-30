# Setup Guide

## Prerequisites

```bash
# System packages
sudo apt update && sudo apt install -y \
  gcc make python3 python3-pip python3-numpy \
  curl jq

# Python dependencies
pip install numpy
```

## 1. Clone & Build

```bash
git clone https://github.com/waefrebeorn/money-room.git
cd money-room

# Build C engine (paper mode for testing)
cd engine
make paper
./room_engine     # Run a single cycle
```

## 2. Run Ecosystem

```bash
cd ..

# Create data directories
mkdir -p ~/.hermes/pm_logs/{eco,c_room,teachers}
mkdir -p ~/.hermes/infra/heartbeats

# Start 10 teacher daemons
python3 ecosystem/pm_teachers.py start

# Run money loop
python3 ecosystem/pm_money_loop.py --count 100

# Check teacher status
python3 ecosystem/pm_teachers.py status
```

## 3. Wire Market Feed

```bash
# Create market feed bridge script (see scripts/run.sh)
# Then add to cron:
# * * * * * python3 /path/to/room_feed_bridge.py
```

## 4. Verify Everything

```bash
# Check ecosystem PnL
cat ~/.hermes/pm_logs/eco/minute_log.jsonl | tail -3

# Check teacher portfolios
ls -la ~/.hermes/pm_logs/teachers/

# Check heartbeats
cat ~/.hermes/infra/heartbeats/pm-money-loop.heartbeat
```

## Production Deployment

See `scripts/deploy.sh` for one-command deploy. Recommended:
- 4GB RAM minimum
- 4+ CPU cores
- SSD storage
- Linux (Ubuntu 22.04+ or WSL2)

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| No trades generated | Check market_feed.json has volume > 0 |
| Darwin not evolving | Need 100+ trades first |
| Teachers crash | `pm_teachers.py restart` or watchdog auto-restarts |
| Q-controller stuck | Verify room_feed_bridge.py is running (cron every 60s) |
| Room_state.bin corruption | `rm -f room_state.bin room_log.csv` then restart |
