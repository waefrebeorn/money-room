# BATTLESHIP INDEX
## Money Room vs Unusual Whales — Complete Gap Analysis

**Date:** May 28, 2026
**Target:** Clone the Unusual Whales MCP (18 tools, 123+ endpoints, 15 categories)
**Status:** We have 14 data pipelines covering ~7/15 categories. **8 categories to clone.**

---

## ── COVERAGE MAP ──

|| # | Category | UW Tools | Our Coverage | Gap | Priority | Clough Bill |
||---|----------|----------|-------------|-----|----------|-------------|
|| 1 | **Stock** | 8 tools (info, chains, Greeks, IV, max pain, volatility) | ❌ None | FULL | 🔴 P0 | `CB-STOCK` |
|| 2 | **Options** | Contract flow, historic prices, volume profiles | ✅ PCR/IV skew/max pain + large trade flow (P62) + OI PCR (P36) | LOW | 🟢 CLOSED | `CB-FLOW` |
|| 3 | **Flow** | Options flow alerts, full tape, net flow | ✅ CBOE options flow alert system (P62) + PCR (P5) | MEDIUM | 🟡 P4 | `CB-OPTIONS` |
|| 4 | **Dark Pool** | Recent trades, ticker-level filtering | ❌ None | FULL | 🔴 P1 | `CB-DARKPOOL` |
|| 5 | **Congress** | Member trades, late reports, recent activity | ❌ None | FULL | 🟠 P2 | `CB-CONGRESS` |
|| 6 | **Politicians** | Portfolios, holdings by ticker *(Premium)* | ❌ None | FULL | ⚪ P6 | `CB-POLITICIAN` |
|| 7 | **Insider** | Transactions, sector flow, ticker flow | ❌ None | FULL | 🟠 P2 | `CB-INSIDER` |
|| 8 | **Institutions** | 13F filings, holdings, sector exposure | ❌ None | FULL | 🟠 P2 | `CB-INSTITUTIONS` |
|| 9 | **Market** | Tide, sector tide, economic/FDA calendar, correlations | ✅ Macro events (P8), cross-asset (P9), L/S ratio (P37), ETF flow (P40) | MEDIUM | 🟡 P3 | `CB-MARKET` |
|| 10 | **Earnings** | Premarket/afterhours schedules, historical | ✅ Earnings calendar + density (P32) | MEDIUM | 🟡 P3 | `CB-EARNINGS` |
|| 11 | **ETF** | Holdings, exposure, inflows/outflows, weights | ✅ BTC ETF flow proxy from 7 ETFs (P40) | MEDIUM | 🟡 P4 | `CB-ETF` |
|| 12 | **Shorts** | Short interest, FTDs, short volume, borrow rates | ❌ None | FULL | 🟠 P2 | `CB-SHORTS` |
|| 13 | **Seasonality** | Monthly performers, yearly patterns | ❌ None | FULL | ⚪ P5 | `CB-SEASONALITY` |
|| 14 | **Screener** | Stock/Option screener, analyst ratings | ❌ None | FULL | 🟠 P2 | `CB-SCREENER` |
|| 15 | **News** | Market news headlines | ✅ GDELT sentiment (P7) + on-chain/whale (P39) | LOW | ⚪ P5 | `CB-NEWS` |

---

## ── EXISTING VS TARGET ──

### What We Have (10 tools, ~4 categories)

| Tool | Our Source | UW Equivalent | Gap |
|------|-----------|---------------|-----|
| `get_room_state` | C engine mmap | — | Unique (our engine) |
| `get_options_flow` | options_pipeline (CBOE/VIX/PCR) | `get_stock_greek_exposure`, `get_stock_max_pain`, `get_stock_iv_rank` | No Greeks, no chains, no IV term structure |
| `get_gdelt_sentiment` | gdelt_pipeline | `get_news_headlines` | Limited (headlines only, no classification) |
| `get_cross_asset` | cross_asset_pipeline | `get_market_tide` (partial) | No sector tide, no correlation matrix |
| `get_macro_events` | macro_pipeline | `get_market_economic_calendar` | No FDA calendar, no earnings calendar |
| `get_feature_importance` | C engine bridge | — | Unique (our engine) |
| `get_tail_risk` | C engine | — | Unique (our engine) |
| `get_prediction` | C engine | — | Unique (our engine) |
| `get_ecosystem_stats` | C engine | — | Unique (our engine) |
| `get_market_summary` | Aggregate | — | Unique (our aggregate) |

#### Unique Advantages (they can't do):
- C engine with 10K agent population, evolution, trading floor
- Feature importance tracking per dimension
- Tail risk hedging with beam-search gating
- Nested cascade prediction with conviction scoring

### What They Have (18 tools, 123+ endpoints, 15 categories)

| Category | Key Endpoints | Data Source (Theirs) | Our Clone Source |
|----------|--------------|---------------------|------------------|
| **Stock** | info, chains, Greeks, IV rank, max pain, volatility | Paid API (CBOE/OPRA) | **Free alternative:** Yahoo Finance / Alpha Vantage (free tier) / yfinance |
| **Options Flow** | flow alerts, full tape | Paid OPRA feed ($50/mo) | **Free alternative:** CBOE data shop (delayed), Yahoo options chains |
| **Dark Pool** | trades, ticker filter | Paid ATS/TRF data ($200+/mo) | **Free alternative:** FINRA OTC transparency (free), WhaleWisdom |
| **Congress** | member, late, recent | Senate STOCK Act filings (free) | **FREE:** Senate.gov, House.gov, capitoltrades.com |
| **Insider** | transactions, flow | SEC EDGAR Form 4 (free) | **FREE:** SEC EDGAR API, InsiderMonkey free tier |
| **Institutions** | 13F filings, holdings | SEC EDGAR 13F (free) | **FREE:** SEC EDGAR, WhaleWisdom free, Dataroma |
| **Market** | tide, sector, econ cal, FDA | Multiple paid feeds | **FREE:** FRED API, BLS, FDA calendar, yfinance |
| **Earnings** | calendar, history | Zacks/refinitiv | **FREE:** Yahoo Finance earnings calendar, Financial Modeling Prep |
| **ETF** | holdings, flows | Paid ETF data ($100+/mo) | **FREE:** Yahoo Finance ETF data, etfdb.com scrape |
| **Shorts** | SI, FTDs, borrow | FINRA + paid feeds | **FREE:** FINRA short volume, SEC FTD data (free), iborrowdesk |
| **Screener** | stock/option/analyst | Multiple paid feeds | **FREE:** Yahoo Finance screener, Finviz free scrape, MarketBeat |
| **Seasonality** | monthly/yearly patterns | Historical calc | **FREE:** yfinance history → statistical computation |
| **News** | headlines | News API paid | **FREE:** GDELT (already built), NewsAPI free tier |

---

## ── CLOUGH BILL MAP ──

Each bill below is a self-contained pipeline plan. Free data. No API keys required (except where free tier noted).

### 🔴 CB-STOCK (P0) — Stock Fundamentals & Technicals
**Gap:** 8 missing tools — stock info, option chains, Greeks, IV rank, max pain, volatility stats
**Free source:** `yfinance` (Python, MIT, no API key, Yahoo Finance data)
**Pipeline:** `stocks_pipeline.py` → `~/.hermes/stocks_cache/stocks.db`
**Cron:** every 1h (market hours)
**MCP tools:** 
- `get_stock_info` — company profile, PE, market cap, dividend
- `get_stock_option_chains` — available expiry chains for a ticker
- `get_stock_max_pain` — max pain calculation from OI
- `get_stock_iv_rank` — IV percentile over 52 weeks
- `get_stock_volatility` — historical volatility, HV10/HV30
- `get_stock_greeks` — delta, gamma, theta, vega for strikes
**Cost:** Free + yfinance pip install

### 🔴 CB-FLOW (P0) — Options Flow Alerts & Tape
**Gap:** No options flow alerting system
**Free source:** CBOE live options quotes (delayed 15min) + our options pipeline
**Pipeline:** `flow_pipeline.py` → `~/.hermes/flow_cache/flow.db`
**Cron:** every 15min
**MCP tools:**
- `get_flow_alerts` — unusual options activity (volume > 2x avg OI)
- `get_flow_net_premium` — net premium flow by ticker
- `get_flow_sector` — sector-level options flow aggregation
**Cost:** Free (CBOE delayed data)

### 🟠 CB-DARKPOOL (P1) — Dark Pool Trade Tracking
**Gap:** No dark pool data at all
**Free source:** FINRA OTC Transparency Data (free CSV download, weekly)
**Pipeline:** `darkpool_pipeline.py` → `~/.hermes/darkpool_cache/darkpool.db`
**Cron:** weekly (FINRA publishes T+2)
**MCP tools:**
- `get_darkpool_recent` — recent off-exchange trades
- `get_darkpool_ticker` — dark pool activity for specific ticker
**Cost:** Free (FINRA public data)

### 🟠 CB-CONGRESS (P2) — Congressional Trading Tracker
**Gap:** No congressional trading data
**Free source:** Senate.gov STOCK Act filings + capitoltrades.com API (free tier, 100 req/day)
**Pipeline:** `congress_pipeline.py` → `~/.hermes/congress_cache/congress.db`
**Cron:** daily
**MCP tools:**
- `get_congress_recent` — recent congressional trades
- `get_congress_trader` — trades by member name
- `get_congress_ticker` — trades by ticker
**Cost:** Free (public filings)

### 🟠 CB-INSIDER (P2) — Insider Transaction Tracker
**Gap:** No insider trading data
**Free source:** SEC EDGAR Form 4 (XML, free API), InsiderMonkey free tier scrape
**Pipeline:** `insider_pipeline.py` → `~/.hermes/insider_cache/insider.db`
**Cron:** daily
**MCP tools:**
- `get_insider_recent` — recent insider trades
- `get_insider_ticker` — insider activity by ticker
- `get_insider_sector` — sector-level insider flow
**Cost:** Free (SEC EDGAR)

### 🟠 CB-INSTITUTIONS (P2) — Institutional Holdings (13F)
**Gap:** No institutional data
**Free source:** SEC EDGAR 13F filings (XML, free), Dataroma (free scrape)
**Pipeline:** `institutions_pipeline.py` → `~/.hermes/inst_cache/institutions.db`
**Cron:** quarterly (13F filing season)
**MCP tools:**
- `get_institution_holdings` — institutional holdings for ticker
- `get_institution_sector` — sector-level institutional exposure
- `get_institution_changes` — top buys/sells last quarter
**Cost:** Free (SEC EDGAR)

### 🟠 CB-SCREENER (P2) — Stock & Options Screener
**Gap:** No screening capability
**Free source:** Yahoo Finance screener API + yfinance scanning + Finviz scrape
**Pipeline:** `screener_pipeline.py` → `~/.hermes/screener_cache/screener.db`
**Cron:** every 1h
**MCP tools:**
- `get_screener_stocks` — stock screener (volume, price, RS, sector filters)
- `get_screener_options` — highest IV, unusual OI options chains
- `get_screener_analyst` — analyst ratings aggregation
**Cost:** Free (scrape)

### 🟠 CB-SHORTS (P2) — Short Interest & FTD Tracker
**Gap:** No short data
**Free source:** FINRA short volume (daily CSV), SEC FTD data (biweekly), iborrowdesk
**Pipeline:** `shorts_pipeline.py` → `~/.hermes/shorts_cache/shorts.db`
**Cron:** daily
**MCP tools:**
- `get_short_interest` — SI by ticker
- `get_short_volume` — short volume ratio
- `get_ftd_data` — failure-to-deliver data
- `get_borrow_rate` — stock borrow rate + availability
**Cost:** Free (FINRA/SEC public data)

### 🟡 CB-MARKET (P3) — Enhanced Market Data
**Gap:** Market tide, sector tide, FDA calendar (partial — we have economic events)
**Enhance:** Upgrade `get_market_tide` with yfinance sector ETF data
**Free source:** FRED API (free tier), FDA.gov calendar, BLS
**MCP tools:**
- `get_market_tide` — broad market sentiment from sector ETFs
- `get_sector_tide` — per-sector momentum score
- `get_fda_calendar` — FDA decision dates
**Cost:** Free (FRED API key = free)

### 🟡 CB-EARNINGS (P3) — Earnings Calendar & History
**Gap:** No earnings data
**Free source:** Yahoo Finance earnings calendar, Financial Modeling Prep (free tier 250/day)
**Pipeline:** `earnings_pipeline.py` → `~/.hermes/earnings_cache/earnings.db`
**Cron:** daily
**MCP tools:**
- `get_earnings_calendar` — upcoming earnings
- `get_earnings_history` — historical EPS surprises
- `get_earnings_ticker` — earnings data for specific ticker
**Cost:** Free

### 🟡 CB-ETF (P4) — ETF Holdings & Flow
**Gap:** No ETF data
**Free source:** Yahoo Finance ETF data, etfdb.com scrape
**Pipeline:** `etf_pipeline.py` → `~/.hermes/etf_cache/etf.db`
**Cron:** daily
**MCP tools:**
- `get_etf_holdings` — top holdings for an ETF
- `get_etf_sector` — sector breakdown
- `get_etf_flow` — aggregated ETF flow signal
**Cost:** Free

### 🟡 CB-OPTIONS (P4) — Enhanced Options Chain Data
**Gap:** We have PCR/IV/max pain but no chain-level data
**Enhance:** Add yfinance options chain extraction to existing options pipeline
**MCP tools:**
- `get_option_chain` — full chain for strike/expiry
- `get_option_volume` — volume profile by strike
- `get_option_oi` — open interest by strike
**Cost:** Free (yfinance)

### ⚪ CB-SEASONALITY (P5) — Market Seasonality
**Gap:** No seasonal patterns
**Free source:** yfinance historical data → statistical computation
**Pipeline:** `seasonality_pipeline.py` → `~/.hermes/seasonality_cache/seasonality.db`
**Cron:** weekly
**MCP tools:**
- `get_seasonality_monthly` — best/worst months per ticker
- `get_seasonality_quarterly` — quarterly patterns
- `get_seasonality_holiday` — holiday/week effect
**Cost:** Free (computed)

### ⚪ CB-POLITICIAN (P6) — Politician Portfolio Tracker
**Gap:** Politician-level portfolio analysis
**Note:** This is Premium-tier for UW. We can do it free via congress data + holdings aggregation
**Pipeline:** Extends congress_pipeline.py
**MCP tools:**
- `get_politician_portfolio` — full portfolio for a politician
- `get_politician_recent` — most recent trades by politician
**Cost:** Free (extends existing pipeline)

### ⚪ CB-NEWS (P5) — Enhanced News Pipeline
**Gap:** We have GDELT but no market news headlines
**Enhance:** Add NewsAPI free tier (100 req/day) + RSS feeds (Yahoo Finance, MarketWatch)
**MCP tools:**
- `get_news_headlines` — financial news for ticker or market
- `get_news_sentiment` — per-ticker news sentiment score
**Cost:** Free (GDELT + NewsAPI free tier + RSS)

---

## ── GRID CELL MAPPING ──

Each Clough bill maps to a new cell in the 300-cell grid (added after the existing P60):

| Cell | Bill | Points | Priority | Description |
|------|------|--------|----------|-------------|
| **P61** | CB-STOCK | 5 | P0 | Stock fundamentals pipeline |
| **P62** | CB-FLOW | 5 | P0 | Options flow volume alerts |
| **P63** | CB-DARKPOOL | 6 | P1 | Dark pool trade tracker |
| **P64** | CB-CONGRESS | 5 | P2 | Congressional trade tracker |
| **P65** | CB-INSIDER | 5 | P2 | Insider transaction pipeline |
| **P66** | CB-INSTITUTIONS | 5 | P2 | 13F filing processor |
| **P67** | CB-SHORTS | 5 | P2 | Short interest + FTD tracker |
| **P68** | CB-SCREENER | 5 | P2 | Stock/option screener |
| **P69** | CB-MARKET | 4 | P3 | Market tide + FDA calendar |
| **P70** | CB-EARNINGS | 4 | P3 | Earnings calendar + history |
| **P71** | CB-ETF | 4 | P4 | ETF holdings + flow |
| **P72** | CB-OPTIONS | 5 | P4 | Full option chain extraction |
| **P73** | CB-SEASONALITY | 3 | P5 | Seasonal pattern computation |
| **P74** | CB-NEWS | 3 | P5 | Financial news RSS pipeline |
| **P75** | CB-POLITICIAN | 3 | P6 | Politician portfolio aggregator |

**Total grid impact:** +15 new cells (P61–P75), 36 total MCP tools

---

## ── EXECUTION ORDER ──

```
Phase 1 (now):     P61 CB-STOCK → P62 CB-FLOW → P63 CB-DARKPOOL    [C binaries, 3 tools]
Phase 2 (next):    P64 CB-CONGRESS → P65 CB-INSIDER → P67 CB-SHORTS [C binaries, 3 tools]
Phase 3 (next):    P66 CB-INSTITUTIONS → P68 CB-SCREENER            [C binaries, 2 tools]
Phase 4 (next):    P69 CB-MARKET → P70 CB-EARNINGS → P71 CB-ETF     [C binaries, 3 tools]
Phase 5 (next):    P72 CB-OPTIONS → P73 CB-SEASONALITY → P74 CB-NEWS → P75 CB-POLITICIAN [C binaries, 4 tools]

Total: 15 new C pipelines, +15 grid cells (P61-P75), 36 total MCP tools
```

---

## ── BOTTOM LINE ──

**They have:** 18 tools, 123+ endpoints, API key required ($50-$200/mo)
**We will have:** 25+ tools, 55+ endpoints, completely free
**Our edge:** C engine (10K agents, feature importance, tail risk, nested cascade) — they can't touch that
**Their edge:** Real-time OPRA options flow, dark pool — they pay for exchange data
**Our strategy:** Clone their free/free-tierable categories (12/15). Accept the paid categories (Options Flow real-time, Dark Pool real-time) as "nice to have." Win on breadth + unique engine.

**The repo is the banner. The banner is the walk.**
