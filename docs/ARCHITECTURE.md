# Money Room — Architecture Deep Dive

## System Overview

The Money Room is a **10,000-agent evolutionary trading ecosystem** built **entirely in C** (engine, collectors, dashboard, all production tools). It runs as a set of cron-driven C binaries on a single host, processing live BTC market data and generating paper trades through competing genome strategies.

**Python is dead in production.** All collectors, the dashboard, and data pipelines are standalone C binaries (libcurl, jansson, sqlite3). 76 C source files in engine/. Ecosystem management (teachers, money loop) is Python-backed but all data paths are C.

## Processing Pipeline

### Layer 1: Data Collection (C binaries)
- `kraken_collector.c` — Kraken OHLCV API → historical.db (SQLite, 1-min BTC candles)
- `economic_collector.c` — FRED API → timeline.db (SP500, VIX)
- `gdelt_sentiment.c` — GDELT news → pump_score calculation
- `onchain_feat.c` — CoinGecko → market cap, BTC dominance
- `market_tide.c` — Yahoo Finance → sector ETF breadth
- + 30+ more C pipelines covering options, funding, OI, liquidations, whales, ETF, hashrate, dark pool, congress, insider, 13F, shorts

### Layer 2: Feed Bridge (`feed_bridge.c` — C binary)
- Runs every 60s via system cron
- Reads latest candle from historical.db
- Enriches with fear/greed, pump score, BTC 30d stats
- Runs MarketDynamicsEngine for PID signals + Q-controller
- Writes `market_feed.json` consumed by all downstream components

### Layer 3: C Room Engine (`room_engine.c`)
- 10,000 agents (2,500 in paper mode)
- Each agent has:
  - 11 genome parameters (evolved via Darwin)
  - 17 learned feature weights (trained via REINFORCE SGD)
  - 4-float hidden state (RNN-like memory)
- Per cycle:
  1. Read market_feed.json
  2. Compute 17-dim feature vector (F1-F17)
  3. Each agent votes: UP/DOWN with conviction
  4. P2P matching: YES votes vs NO votes
  5. Capital transfer between winners/losers
  6. Nested HT cascade inference for trade bias
  7. Darwin evolution every 100 trades

### Layer 4: C Dashboard (`dashboard.c` — 44KB ELF, 1.1MB RAM)
- Raw HTTP server on port 9090
- SHA256 auth, session cookies, SQLite visitor tracking
- Routes: / /login /logout /tracking /api/rooms /api/consensus /api/stats /api/tracking
- Shows 16 rooms, timeline stats, 8-topic consensus, visitor data
- Started via systemd (money-room-dashboard.service) — 7ms startup

### Layer 5: Teacher Strategies (Python, C-backed)
- 10 independent daemon processes
- Each has a unique strategy profile (ultra conservative → degenerate)
- Each reads market_feed.json independently
- Auto-restarted by teacher_watchdog.c (C binary) every 5min

### Layer 6: Q-Controller (`market_controller.c` — C binary)
- Tabular Q-learning (ε-greedy, lr=0.1, γ=0.9)
- State: regime × volatility_bucket × sentiment_bucket
- Actions: 5 strategy weight distributions
- Reward: price movement alignment with predicted direction
- Reward applied every 60s via feed_bridge.c

## File Layout

```
money-room/
├── engine/                    # 76 C source files (compiled binaries)
│   ├── dashboard.c           # C HTTP server (44KB ELF, port 9090)
│   ├── room_engine.c         # Main loop (~600 LOC)
│   ├── room_features.c       # 17-dim feature computation (φ, DFT, MACD, RSI, etc.)
│   ├── room_capital.c        # P2P matching + capital transfer + SGD update
│   ├── room_vote.c           # Agent voting + sigmoid activation
│   ├── room_darwin.c         # Darwin evolution + diversity metrics
│   ├── room_feeds.c          # Market data ingestion
│   ├── room_bridge.c         # mmap → JSON snapshot
│   ├── nested_ht_infer.h     # Nested HT cascade inference (header-only)
│   ├── kraken_collector.c    # Kraken OHLCV data
│   ├── gdelt_sentiment.c     # GDELT news sentiment
│   ├── db_prune.c            # Database pruning
│   ├── basin_sweep.c         # Feature baseline sweep
│   ├── polygon_monitor.c     # Blockchain monitor
│   ├── options_flow.c        # CBOE options chain
│   ├── market_tide.c         # Sector ETF breadth
│   ├── earnings_cal.c        # Earnings calendar features
│   ├── funding_feat.c        # Perpetual funding rate
│   ├── onchain_feat.c        # On-chain BTC features
│   ├── hashrate_feat.c       # Hash rate & mining floor
│   ├── dark_pool_feat.c      # Dark pool / ATS volume
│   ├── congress_trades.c     # Congressional trading
│   ├── insider_trades.c      # SEC Form 4 insider tracking
│   ├── 13f_holdings.c        # Institutional 13F filings
│   ├── macro_pipeline.c      # Macro events
│   ├── feed_bridge.c         # Feed bridge
│   ├── market_controller.c   # Q-controller
│   ├── money_loop.c          # Ecosystem money loop
│   └── ... (+ 50+ more .c files)
│   └── Makefile              # Build targets: production, paper, market, dashboard
├── ecosystem/                 # Python ecosystem (C-backed data paths)
│   ├── pm_money_loop.py      # 10K genome trading loop
│   ├── pm_teachers.py        # 10 teacher daemons
│   └── pm_market_controller.py # Q-controller + market dynamics
├── scripts/                  # Deployment and ops
│   ├── deploy.sh             # One-command deploy
│   └── setup.sh              # Initial setup
├── docs/                     # Documentation
│   ├── ARCHITECTURE.md       # This file
│   ├── setup.md              # Getting started
│   ├── genome-params.md      # Genome parameter reference
│   ├── paper-proof.md        # Paper proof methodology
│   └── enterprise.md         # Enterprise features
└── README.md                 # Main entry point
```

## Feature Engineering (17-dim)

### Standard Technical Features (F1-F13)
All standard TA features computed from price history ring buffer (50 elements):
- F1-F2: Price delta and momentum
- F3: RSI(7) 
- F4: Volume surge ratio
- F5-F6: EMA cross (fast/slow)
- F7: MACD histogram
- F8: Bollinger %B
- F9: Price-RSI divergence
- F10: News pump score
- F11: Regime detection (range/trend/volatile)
- F12: Fear & Greed index
- F13: Herd consensus

### Money Room Exclusive (F14-F17)
**GAAD Golden-Ratio Timeframes (F14-F16):**
- Weighted multi-scale analysis at φ, φ², φ³ intervals
- Higher weight on shorter intervals
- Captures fractal structure of market moves

**Goertzel DFT (F17):**
- Single-frequency DFT using Goertzel algorithm
- Extracts dominant cycle length from price history
- Normalized to [0, 1] for use as feature

## Architecture Diagram

```
                         ┌──────────────────────────────────┐
                         │   C Collectors (30+ binaries)     │
                         │   Kraken / FRED / GDELT / CBOE   │
                         │   CoinGecko / OKX / Yahoo v8     │
                         └────────────┬─────────────────────┘
                                      │
                         ┌────────────▼─────────────────────┐
                         │      feed_bridge.c (C)            │
                         │      (every 60s via cron)          │
                         │      + Q-controller reward         │
                         └────────────┬─────────────────────┘
                                      │
              ┌───────────────────────┼───────────────────────┐
              │                       │                       │
              ▼                       ▼                       ▼
   ┌──────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
   │  C Room Engine    │   │  C Dashboard (9090)  │   │  Python Ecosystem   │
   │  (C11, ~600 LOC)  │   │  44KB ELF, 1.1MB RAM │   │  (C-backed data)     │
   │                   │   │  SHA256 auth          │   │                       │
   │ • 4 rooms cycling │   │  Visitor tracking     │   │ • 10K genomes         │
   │ • 10K agents      │   │  6 routes + 5 APIs   │   │ • 10 teachers         │
   │ • Darwin evolve   │   │  SQLite visits DB    │   │ • Trade loop          │
   │ • Feature import. │   └─────────────────────┘   └─────────────────────┘
   │ • Tail risk hedge │
   └────────┬─────────┘
            │
            ▼
   ┌─────────────────────┐
   │  C Housekeeping     │
   │  db_prune / basin   │
   │  sweep / monitor    │
   └─────────────────────┘
```

## Performance

| Metric | Value |
|--------|-------|
| Cycle time (10K agents) | 0.3-0.9ms |
| Cycle time (2.5K paper) | 0.1-0.3ms |
| Memory per agent | ~200 bytes |
| Total memory (10K) | ~6MB + trade buffer |
| Trade buffer | 1M trades ~ 64MB |
| Dashboard RAM | **1.1MB** (was 22.7MB with Flask) |
| Dashboard startup | **7ms** (was 986ms with Flask) |
| C source files | **76 .c** files, ~50K LOC |
