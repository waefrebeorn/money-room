# ⚔️ DA AUDIT: All Market Data Sources
> The timeline is the vault for time data.
> Every tradable market → timeline.db. Every timestamp → one row.
> Compiled: 2026-05-28 | Rows: 9,745,477 | Sources: 15,441 | Span: 2012-01-01 → Present

---

## Phase 1: CLAIM — "We have comprehensive market data"

We operate 9.7M rows across 15K+ sources spanning 14 years.
Data flows into timeline.db from 12+ data categories.
All tradable markets should be represented.

## Phase 2: VERIFY — What We Actually Have

### ✅ LIVE DATA (currently flowing into timeline.db)

| # | Market | Sources | Rows | Years | Resolution | C Collector? |
|---|--------|---------|:----:|:-----:|:----------:|:-----------:|
| 1 | **Bitcoin (BTC)** | bitstamp_1min | 7,569,588 | 2012-2026 | **1-min** | ✅ engine |
| 2 | **FRED Macro** | 10 series | 1,944,455 | 2015-2026 | Daily | ✅ engine |
| 3 | **Polymarket** | 15,244 markets | 159,391 | May 2026 | Per-event | ✅ `polymarket_collector.c` |
| 4 | **Stocks** | 10 (VIX, gold, DOW, NASDAQ, etc.) | 26,326 | 2011-2026 | Daily | ✅ engine |
| 5 | **Kraken Crypto** | 4 pairs (BTC, ETH, SOL, XRP) | 21,258 | May 2026 | 1-min | ✅ engine |
| 6 | **DeFiLlama** | 36 chains/DEXes | 8,520 | May 2026 | Snapshot | ✅ engine |
| 7 | **Forex** | 3 pairs | 3,901 | 2021-2026 | Daily | ✅ engine |
| 8 | **Fear & Greed** | 3 variants | 3,684 | 2018-2026 | Daily | ✅ engine |
| 9 | **CoinGecko** | 10 crypto prices | 3,124 | May 2026 | Snapshot | ✅ engine |
| 10 | **Weather** | 5 cities | 2,840 | May 2026 | Hourly | ✅ engine |
| 11 | **Engine Trades** | 2 (5min/fivemin) | 2,131 | May 2026 | Per-trade | ✅ engine |
| 12 | **GDELT News** | 1 | 145 | Mar-May 2026 | Per-event | ✅ engine |
| 13 | **Kalshi** | 100 markets | 100 | May-Jun 2026 | Snapshot | ✅ `kalshi_collector.c` |

### ⏳ LEGACY DATA — On Our Radar

| # | Market | What's Available | Where | Cost | Priority |
|---|--------|-----------------|-------|:----:|:--------:|
| 14 | **Polymarket pre-2025** | 43,840 events, 8.1M prices, 792K orderbooks | Kaggle + manja316/github | **Free** | 🔴 HIGH |
| 15 | **Polymarket tick data** | Trades, order books, quotes from Oct 2025 | telonex.io/datasets | **Free** | 🔴 HIGH |
| 16 | **Polymarket historical** | Free CSV + BigQuery | deltabase.tech | **Free** | 🔴 HIGH |
| 17 | **Kalshi historical** | Settled markets, completed trades | docs.kalshi.com + deltabase | **Free** | 🔴 HIGH |
| 18 | **Sports odds (MLB 2009-2023)** | Moneylines, O/U, scores | Princeton DSS | **Free** (academic) | 🟡 MED |
| 19 | **Sports odds (2020+)** | 10-min snapshots, 350+ bookmakers | the-odds-api.com | **Free tier** | 🟡 MED |
| 20 | **Sports betting data** | Match results, odds, bookmakers | Kaggle SportsBet 2025 | **Free** | 🟡 MED |
| 21 | **Historical sports data** | NFL, NBA, MLB, NHL, soccer | api-sports.io (free 100/d) | **Free tier** | 🟡 MED |
| 22 | **Options data** | VIX term structure, put/call ratios | CBOE free | **Free** | 🟡 MED |
| 23 | **SEC EDGAR** | Insider trading, 13F filings | sec.gov/edgar | **Free** | 🟡 MED |
| 24 | **On-chain data** | Exchange flows, miner revenue | CoinMetrics free tier | **Free tier** | 🟢 LOW |

## Phase 3: RISK — Data Gaps Assessment

| Risk | Severity | Details |
|------|:--------:|---------|
| **BTC over-dominance** | 🔴 HIGH | 77.7% of all data is ONE source (Bitstamp BTC). ETH, SOL have <0.2% |
| **Polymarket only live** | 🟡 MED | Only last 4 days of data. Need pre-2025 backfill for meaningful training |
| **Kalshi just started** | 🟡 MED | Only 100 markets, 0 volume on most. Needs daily collection |
| **No sports data yet** | 🔴 HIGH | Sports room designed but zero data flowing. Need Odds API + historical |
| **No options data** | 🟡 MED | VIX is there but no options chain, IV skew, put/call |
| **No order book data** | 🟡 MED | Level 2 data completely missing across all sources |
| **Macro only daily** | 🟢 LOW | Daily is fine for macro alignment, but limits 1-min feature engineering |
| **No SEC/insider data** | 🟢 LOW | Valuable for stock prediction but lower priority than sports/poly |

## Phase 4: MITIGATE — Action Plan

### IMMEDIATE (This sprint)
| Action | Status |
|--------|--------|
| ✅ Polymarket Gamma collector built | **DONE** — 5,000 events, 159K rows |
| ✅ Kalshi collector built | **DONE** — 100 markets |
| 🔲 Download Polymarket pre-2025 dataset | **Kaggle 43K events + manja316 GitHub** |
| 🔲 Download Kalshi historical from DeltaBase | **Free CSV** |
| 🔲 Build sports odds collector (The Odds API) | **Free tier, historical back to 2020** |

### NEXT SPRINT
| Action | Target |
|--------|--------|
| Backfill Polymarket to 2020 | Kaggle + GitHub dataset |
| Backfill Kalshi to 2022 (inception) | DeltaBase + Kalshi API |
| Pull Princeton sports odds (MLB 2009-2023) | Academic free data |
| Start daily The Odds API collection | Free tier (1000 req/mo) |

### FUTURE
| Action | Target |
|--------|--------|
| SEC EDGAR insider trading pipeline | C binary |
| On-chain data (exchange flows) | CoinMetrics free tier |
| Options chain data | CBOE free |

---

## LAUNDRY LIST: Legacy Data to Find

> Timeline is the vault. Every timepoint goes in. Here's what we're hunting:

### 1. Prediction Markets (Pre-2025)
- 🔴 **Kaggle: Polymarket Prediction Markets** — 43,840 events, free
- 🔴 **manja316/polymarket-historical-data** — 9,550 markets, 30 days 15-min snapshots
- 🔴 **DeltaBase** — Free CSV of all Polymarket + Kalshi history
- 🔴 **Telonex** — Polymarket tick data from Oct 2025, order books, trades

### 2. Sports Betting (Historical)
- 🟡 **Princeton DSS** — MLB 2009-2023: opening/closing moneylines, O/U, scores
- 🟡 **The Odds API** — Historical snapshots from June 2020, 10-min intervals, free tier
- 🟡 **Kaggle SportsBet 2025** — Match results, odds, bookmakers across leagues
- 🟡 **OddsPapi** — Free historical odds CSV export

### 3. Financial Data (Gaps)
- 🟡 **CBOE free data** — VIX term structure, put/call ratios
- 🟡 **SEC EDGAR** — Insider trading (Form 4), institutional holdings (13F)
- 🟢 **CoinMetrics** — On-chain BTC data (exchange flows, miner revenue)
- 🟢 **FRED additional series** — Housing starts, consumer confidence, manufacturing

---

## Data Flow Architecture

```
External Sources (live)
  ├── Polymarket Gamma API ──→ polymarket_collector.c ──→ timeline.db
  ├── Kalshi API ────────────→ kalshi_collector.c ──────→ timeline.db
  ├── Crypto exchanges ──────→ room_feeds.c ────────────→ timeline.db
  └── FRED / Macro ──────────→ data collectors ────────→ timeline.db

Historical/Legacy (batch)
  ├── Kaggle CSVs ──────────→ timeline_ingest.c ──────→ timeline.db
  ├── DeltaBase CSVs ───────→ timeline_ingest.c ──────→ timeline.db
  ├── Princeton sports ─────→ timeline_ingest.c ──────→ timeline.db
  └── The Odds API ─────────→ odds_collector.c ───────→ timeline.db

Consumers
  ├── training_pile.c ──────→ unified training tensor for engine
  ├── room_engine.c ────────→ 10K traders on market data
  ├── sports_room.c ────────→ 10K fans on sports data
  ├── timeline_analyzer.c ──→ real-time market thermometer
  └── future: polymarket_room.c, kalshi_room.c
```

---

## Summary

| Metric | Value |
|--------|:-----:|
| **Total rows** | 9,745,477 |
| **Unique sources** | 15,441 |
| **Time span** | 2012-01-01 → Present (14 years) |
| **Live collectors** | 13 (C: 11, Python: 2) |
| **Legacy data to ingest** | **7 high-priority datasets** |
| **Missing categories** | Options, order books, on-chain, SEC |

*NOT FINANCIAL ADVICE. Timeline is a vault of public market data. Algorithmic analysis only.*
