1|# Money Room — Architecture Deep Dive
2|
3|## System Overview
4|
5|The Money Room is a **10,000-agent evolutionary trading ecosystem** built **entirely in C** (engine, collectors, dashboard, all production tools). It runs as a set of cron-driven C binaries on a single host, processing live BTC market data and generating paper trades through competing genome strategies.
6|
7|**Python is zero in production.** All collectors, data pipelines, risk analysis, and monitoring are standalone C binaries (libcurl, jansson, sqlite3). 209 C source files in engine/. No Python ecosystems remain.
8|
9|## Processing Pipeline
10|
11|### Layer 1: Data Collection (C binaries)
12|- `kraken_collector.c` — Kraken OHLCV API → historical.db (SQLite, 1-min BTC candles)
13|- `economic_collector.c` — FRED API → timeline.db (SP500, VIX)
14|- `gdelt_sentiment.c` — GDELT news → pump_score calculation
15|- `onchain_feat.c` — CoinGecko → market cap, BTC dominance
16|- `market_tide.c` — Yahoo Finance → sector ETF breadth
17|- + 30+ more C pipelines covering options, funding, OI, liquidations, whales, ETF, hashrate, dark pool, congress, insider, 13F, shorts
18|
19|### Layer 2: Feed Bridge (`feed_bridge.c` — C binary)
20|- Runs every 60s via system cron
21|- Reads latest candle from historical.db
22|- Enriches with fear/greed, pump score, BTC 30d stats
23|- Runs MarketDynamicsEngine for PID signals + Q-controller
24|- Writes `market_feed.json` consumed by all downstream components
25|
26|### Layer 3: C Room Engine (`room_engine.c`)
27|- 10,000 agents (2,500 in paper mode)
28|- Each agent has:
29|  - 11 genome parameters (evolved via Darwin)
30|  - 17 learned feature weights (trained via REINFORCE SGD)
31|  - 4-float hidden state (RNN-like memory)
32|- Per cycle:
33|  1. Read market_feed.json
34|  2. Compute 18-dim feature vector (F1-F18)
35|  3. Each agent votes: UP/DOWN with conviction
36|  4. P2P matching: YES votes vs NO votes
37|  5. Capital transfer between winners/losers
38|  6. Nested HT cascade inference for trade bias
39|  7. Darwin evolution every 100 trades
40|
41|### Layer 4: C Dashboard (`data_server.c` — 22KB ELF)
42|- Static file server on port 9090
43|- Serves `docs/data/` JSON files (pipeline_status, paper_stats, health, etc.)
44|- CORS for browser access
45|- GET /lists files, GET /data/<file> serves JSON, POST /register handles signups
46|- Runs via systemd (money-room-dashboard.service) — fork-per-connection
47|
48|### Layer 5: Teacher Strategies (Python, C-backed)
49|- 10 independent daemon processes
50|- Each has a unique strategy profile (ultra conservative → degenerate)
51|- Each reads market_feed.json independently
52|- Auto-restarted by teacher_watchdog.c (C binary) every 5min
53|
54|### Layer 6: Q-Controller (`market_controller.c` — C binary)
55|- Tabular Q-learning (ε-greedy, lr=0.1, γ=0.9)
56|- State: regime × volatility_bucket × sentiment_bucket
57|- Actions: 5 strategy weight distributions
58|- Reward: price movement alignment with predicted direction
59|- Reward applied every 60s via feed_bridge.c
60|
61|## File Layout
62|
63|```
64|money-room/
65|├── engine/                    # 209 C source files (compiled binaries)
66|│   ├── data_server.c         # C static file server (22KB ELF, port 9090)
67|│   ├── room_engine.c         # Main loop (~600 LOC)
68|│   ├── room_features.c       # 18-dim feature computation (RSI, MACD, Bollinger, tail risk, regime)
69|│   ├── room_capital.c        # P2P matching + capital transfer + SGD update
70|│   ├── room_vote.c           # Agent voting + sigmoid activation
71|│   ├── room_darwin.c         # Darwin evolution + diversity metrics
72|│   ├── room_feeds.c          # Market data ingestion
73|│   ├── room_bridge.c         # mmap → JSON snapshot
74|│   ├── nested_ht_infer.h     # Nested HT cascade inference (header-only)
75|│   ├── kraken_collector.c    # Kraken OHLCV data
76|│   ├── gdelt_sentiment.c     # GDELT news sentiment
77|│   ├── db_prune.c            # Database pruning
78|│   ├── basin_sweep.c         # Feature baseline sweep
79|│   ├── polygon_monitor.c     # Blockchain monitor
80|│   ├── options_flow.c        # CBOE options chain
81|│   ├── market_tide.c         # Sector ETF breadth
82|│   ├── earnings_cal.c        # Earnings calendar features
83|│   ├── funding_feat.c        # Perpetual funding rate
84|│   ├── onchain_feat.c        # On-chain BTC features
85|│   ├── hashrate_feat.c       # Hash rate & mining floor
86|│   ├── dark_pool_feat.c      # Dark pool / ATS volume
87|│   ├── congress_trades.c     # Congressional trading
88|│   ├── insider_trades.c      # SEC Form 4 insider tracking
89|│   ├── 13f_holdings.c        # Institutional 13F filings
90|│   ├── macro_pipeline.c      # Macro events
91|│   ├── feed_bridge.c         # Feed bridge
92|│   ├── market_controller.c   # Q-controller
93|│   ├── money_loop.c          # Ecosystem money loop
94|│   └── ... (+ 50+ more .c files)
95|│   └── Makefile              # Build targets: production, paper, market, dashboard
96|├── ecosystem/                 # Python ecosystem (C-backed data paths)
97|│   ├── pm_money_loop.py      # 10K genome trading loop
98|│   ├── pm_teachers.py        # 10 teacher daemons
99|│   └── pm_market_controller.py # Q-controller + market dynamics
100|├── scripts/                  # Deployment and ops
101|│   ├── deploy.sh             # One-command deploy
102|│   └── setup.sh              # Initial setup
103|├── docs/                     # Documentation
104|│   ├── ARCHITECTURE.md       # This file
105|│   ├── setup.md              # Getting started
106|│   ├── genome-params.md      # Genome parameter reference
107|│   ├── paper-proof.md        # Paper proof methodology
108|│   └── enterprise.md         # Enterprise features
109|└── README.md                 # Main entry point
110|```
111|
112|## Feature Engineering (18-dim)
113|
114|### Standard Technical Features (F1-F14)
115|All standard TA features computed from price history ring buffer (50 elements):
116|- F1-F2: Price delta and momentum
117|- F3: RSI(7) 
118|- F4: Volume surge ratio
119|- F5-F6: EMA cross (fast/slow)
120|- F7: MACD histogram
121|- F8: Bollinger %B
122|- F9: Price-RSI divergence
123|- F14: Tail risk score (beam-search gating of worst 5% scenarios)
124|- F15: News pump score
125|- F16: Regime detection (range/trend/volatile)
126|- F17: Fear & Greed index
127|- F18: Herd consensus
128|
129|### Multi-Market Features (F15-F18)
130|**Tail risk score (F15):**
131|- Beam-search gating of worst 5% scenario outcomes
132|- Weighted by conviction of hedging agents
133|- Normalized to [0, 1]
134|
135|**News pump score (F16):**
136|- GDELT global sentiment aggregated over 4h windows
137|- Positive/negative article ratio
138|- Decayed recency weighting
139|
140|**Regime detection (F17):**
141|- Range/trend/volatile classification via ADX + volatility ratio
142|- Per-market regime state in room_features
143|
144|**Fear & Greed index (F18):**
145|- Composite from 7 sub-indices (volatility, momentum, put/call, etc.)
146|- Fetched every 15min from fear_greed_collector
147|
148|## Architecture Diagram
149|
150|```
151|                         ┌──────────────────────────────────┐
152|                         │   C Collectors (80+ binaries)     │
153|                         │   Kraken / FRED / GDELT / CBOE   │
154|                         │   CoinGecko / OKX / Yahoo v8     │
155|                         └────────────┬─────────────────────┘
156|                                      │
157|                         ┌────────────▼─────────────────────┐
158|                         │      feed_bridge.c (C)            │
159|                         │      (every 60s via cron)          │
160|                         │      + Q-controller reward         │
161|                         └────────────┬─────────────────────┘
162|                                      │
163|              ┌───────────────────────┼───────────────────────┐
164|              │                       │                       │
165|              ▼                       ▼                       ▼
166|   ┌──────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
167|   │  C Room Engine    │   │  C Dashboard (9090)  │   │  Python Ecosystem   │
168|   │  (C11, ~600 LOC)  │   │  44KB ELF, 1.1MB RAM │   │  (C-backed data)     │
169|   │                   │   │  SHA256 auth          │   │                       │
170|   │ • 17 markets      │   │  Visitor tracking     │   │ • 10K genomes         │
171|   │ • 10K agents      │   │  6 routes + 5 APIs   │   │ • 10 teachers         │
172|   │ • Darwin evolve   │   │  SQLite visits DB    │   │ • Trade loop          │
173|   │ • Feature import. │   └─────────────────────┘   └─────────────────────┘
174|   │ • Tail risk hedge │
175|   └────────┬─────────┘
176|            │
177|            ▼
178|   ┌─────────────────────┐
179|   │  C Housekeeping     │
180|   │  db_prune / basin   │
181|   │  sweep / monitor    │
182|   └─────────────────────┘
183|```
184|
185|## Performance
186|
187|| Metric | Value |
188||--------|-------|
189|| Cycle time (10K agents) | 0.3-0.9ms |
190|| Cycle time (2.5K paper) | 0.1-0.3ms |
191|| Memory per agent | ~200 bytes |
192|| Total memory (10K) | ~6MB + trade buffer |
193|| Trade buffer | 1M trades ~ 64MB |
194|| Dashboard RAM | **1.1MB** (was 22.7MB with Flask) |
195|| Dashboard startup | **7ms** (was 986ms with Flask) |
196|| C source files | **209 .c** files, ~57K LOC |
197|