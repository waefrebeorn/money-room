# BATTLESHIP INDEX
## Money Room vs Unusual Whales — Complete Gap Analysis

**Date:** June 1, 2026
**Target:** Clone the Unusual Whales MCP (18 tools, 123+ endpoints, 15 categories)
**Status:** 12/15 fully PORTED, 3 PARTIAL (Market ⚠️, Options/Flow ⚠️, News ⚠️)

---

## ── COVERAGE MAP ──

|| # | Category | UW Tools | Our Coverage | Gap | Priority | Clough Bill |
|---|----------|----------|-------------|-----|----------|-------------|
|| 1 | **Stock** | 8 tools (info, chains, Greeks, IV, max pain, volatility) | ✅ 6/6 PORTED | CLOSED | 🔴 CLOSED | `CB-STOCK` |
|| 2 | **Options** | Contract flow, historic prices, volume profiles | ✅ PCR/IV skew/max pain + large trade flow (P62) + OI PCR (P36) | LOW | 🟢 CLOSED | `CB-FLOW` |
|| 3 | **Flow** | Options flow alerts, full tape, net flow | ✅ CBOE options flow alert system (P62) + PCR (P5) | MEDIUM | 🟡 P4 | `CB-OPTIONS` |
|| 4 | **Dark Pool** | Recent trades, ticker-level filtering | ✅ CBOE/FINRA dark pool feat via dark_pool_feat.c (546 lines C, compiled) | MEDIUM | 🟡 P4 | `CB-DARKPOOL` |
|| 5 | **Congress** | Member trades, late reports, recent activity | ✅ Capitol Trades via congress_trades.c (363 lines C, compiled, cron every 60min) | MEDIUM | 🟡 P3 | `CB-CONGRESS` |
|| 6 | **Politicians** | Portfolios, holdings by ticker *(Premium)* | ✅ Portfolio aggregator via politician_portfolio.c (388 lines C, compiled, cron 240min) | MEDIUM | 🟡 P4 | `CB-POLITICIAN` |
|| 7 | **Insider** | Transactions, sector flow, ticker flow | ✅ SEC EDGAR Form 4 via insider_trades.c (338 lines C, compiled, cron every 60min) | MEDIUM | 🟡 P3 | `CB-INSIDER` |
|| 8 | **Institutions** | 13F filings, holdings, sector exposure | ✅ SEC EDGAR 13F via 13f_holdings.c (338 lines C, compiled) | MEDIUM | 🟡 P3 | `CB-INSTITUTIONS` |
|| 9 | **Market** | Tide, sector tide, economic/FDA calendar, correlations | ✅ Macro events (P8), cross-asset (P9), L/S ratio (P37), ETF flow (P40) | MEDIUM | 🟡 P3 | `CB-MARKET` |
|| 10 | **Earnings** | Premarket/afterhours schedules, historical | ✅ Earnings calendar + density (P32) | MEDIUM | 🟡 P3 | `CB-EARNINGS` |
|| 11 | **ETF** | Holdings, exposure, inflows/outflows, weights | ✅ BTC ETF flow proxy from 7 ETFs (P40) | MEDIUM | 🟡 P4 | `CB-ETF` |
|| 12 | **Shorts** | Short interest, FTDs, short volume, borrow rates | ✅ FINRA short vol + FTD via short_interest_feat.c (727 lines C, compiled) | MEDIUM | 🟡 P3 | `CB-SHORTS` |
|| 13 | **Seasonality** | Monthly performers, yearly patterns | ✅ 5yr SPY history via seasonality.c (203 lines C, compiled, cron 30min) | MEDIUM | 🟡 P4 | `CB-SEASONALITY` |
|| 14 | **Screener** | Stock/Option screener, analyst ratings | ✅ Composite multi-source screener via stock_screener.c (326 lines C, compiled) | MEDIUM | 🟡 P3 | `CB-SCREENER` |
|| 15 | **News** | Market news headlines | ✅ GDELT sentiment (P7) + on-chain/whale (P39) | LOW | ⚪ P5 | `CB-NEWS` |

---

## ── EXISTING VS TARGET ──

### What We Have (24+ tools, 15 categories)

| Tool | Our Source | UW Equivalent | Gap |
|------|-----------|---------------|-----|
| `get_room_state` | C engine mmap | — | Unique (our engine) |
| `get_iv_rank` | iv_rank.c (options flow DB, C) | `get_stock_iv_rank` | LOW (needs 52wk data accumulation) |
| `get_volatility` | volatility_calc.c (timeline.db, C) | `get_stock_volatility` | LOW |
| `get_politician_portfolio` | politician_portfolio.c (C) | `get_politician_portfolio` | LOW |
| `get_options_flow` | options_flow.c / options_chain.c (CBOE/VIX/PCR) | `get_stock_greek_exposure`, `get_stock_max_pain`, `get_stock_iv_rank` | No IV term structure |
| `get_dark_pool` | dark_pool_feat.c (FINRA/CBOE, C) | `get_darkpool_trades` | LOW |
| `get_congress_trades` | congress_trades.c (Capitol Trades API) | `get_congress_trades` | LOW |
| `get_insider_trades` | insider_trades.c (SEC EDGAR, C) | `get_insider_transactions` | LOW |
| `get_13f_holdings` | 13f_holdings.c (SEC EDGAR, C) | `get_institution_holdings` | LOW |
| `get_short_interest` | short_interest_feat.c (FINRA, C) | `get_short_interest` | LOW |
| `get_gdelt_sentiment` | gdelt_pipeline | `get_news_headlines` | Limited (headlines only, no classification) |
| `get_cross_asset` | cross_asset_pipeline | `get_market_tide` (partial) | No sector tide, no correlation matrix |
| `get_macro_events` | macro_pipeline | `get_market_economic_calendar` | No FDA calendar, no earnings calendar |
| `get_earnings` | earnings_calendar.c + earnings_cal.c (C) | `get_earnings_calendar` | LOW |
| `get_etf_flow` | etf_flow_feat.c / etf_holdings.c (C) | `get_etf_holdings`, `get_etf_flow` | LOW |
| `get_screener` | stock_screener.c (12 DB join, C) | `get_screener_stocks` | LOW |
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
- Custom HV10/HV30 volatility computation per ticker

### What They Have (18 tools, 123+ endpoints, 15 categories)

| Category | Key Endpoints | Data Source (Theirs) | Our Clone Source |
|----------|--------------|---------------------|------------------|
| **Stock** | info, chains, Greeks, IV rank, max pain, volatility | Paid API (CBOE/OPRA) | **Free:** Yahoo Finance, Finnhub free tier, yahoo_collector |
| **Options Flow** | flow alerts, full tape | Paid OPRA feed ($50/mo) | **Free:** CBOE data shop (delayed), Yahoo options chains |
| **Dark Pool** | trades, ticker filter | Paid ATS/TRF data ($200+/mo) | **Free:** FINRA OTC transparency |
| **Congress** | member, late, recent | Senate STOCK Act filings (free) | **FREE:** Senate.gov, House.gov, capitoltrades.com |
| **Insider** | transactions, flow | SEC EDGAR Form 4 (free) | **FREE:** SEC EDGAR |
| **Institutions** | 13F filings, holdings | SEC EDGAR 13F (free) | **FREE:** SEC EDGAR |
| **Market** | tide, sector, econ cal, FDA | Multiple paid feeds | **FREE:** FRED, BLS, yfinance |
| **Earnings** | calendar, history | Zacks/refinitiv | **FREE:** Yahoo Finance earnings calendar |
| **ETF** | holdings, flows | Paid ETF data ($100+/mo) | **FREE:** Yahoo Finance ETF data |
| **Shorts** | SI, FTDs, borrow | FINRA + paid feeds | **FREE:** FINRA short volume, SEC FTD |
| **Screener** | stock/option/analyst | Multiple paid feeds | **FREE:** Local SQLite (12 sources) |
| **Seasonality** | monthly/yearly patterns | Historical calc | **FREE:** yfinance history → C computation |
| **News** | headlines | News API paid | **FREE:** GDELT (already built) |

---

## ── CLOUGH BILL MAP ──

Each bill below is a self-contained pipeline plan. Free data. No API keys required (except where free tier noted).

### ✅ CB-STOCK (P0) — Stock Fundamentals & Technicals — PORTED
**Status:** ✅ PORTED — stock_collector.c (393 lines C) + options_chain.c + iv_rank.c + volatility_calc.c
**Free source:** Finnhub free API (300 req/day) + Yahoo Finance v7 chart (via yahoo_collector.c)
**Cron:** every 4h (via collector_runner SLOW)
**MCP tools:**
- `get_stock_info` ✅ (stock_collector.c)
- `get_stock_option_chains` ✅ (options_chain.c)
- `get_stock_max_pain` ✅ (options_chain.c)
- `get_stock_iv_rank` ✅ (iv_rank.c)
- `get_stock_volatility` — HV10/HV30 ✅ (volatility_calc.c)
- `get_stock_greeks` ✅ (options_flow.c)

### ✅ CB-FLOW (P0) — Options Flow Alerts — PORTED
**Status:** ✅ PORTED — options_flow.c (667 lines C), collector_runner wired every 30min
**Free source:** CBOE live options quotes (delayed 15min) via cdn.cboe.com
**MCP tools:**
- `get_flow_alerts` — unusual options activity ✅
- `get_flow_net_premium` — net premium flow by ticker ✅
- `get_flow_sector` — sector-level options flow aggregation ✅

### ✅ CB-DARKPOOL (P1) — Dark Pool Trade Tracking — PORTED
**Status:** ✅ PORTED — dark_pool_feat.c (546 lines C) compiled, collector_runner wired every 60min
**Free source:** FINRA OTC Transparency
**MCP tools:**
- `get_darkpool_recent` — recent off-exchange trades ✅
- `get_darkpool_ticker` — dark pool activity for specific ticker ✅

### ✅ CB-CONGRESS (P2) — Congressional Trading Tracker — PORTED
**Status:** ✅ PORTED — congress_trades.c (363 lines C), collector_runner wired every 60min
**Free source:** capitoltrades.com API (free tier, 100 req/day)
**MCP tools:**
- `get_congress_recent` ✅
- `get_congress_trader` ✅
- `get_congress_ticker` ✅

### ✅ CB-INSIDER (P2) — Insider Transaction Tracker — PORTED
**Status:** ✅ PORTED — insider_trades.c (338 lines C), collector_runner wired every 60min
**Free source:** SEC EDGAR Form 4
**MCP tools:**
- `get_insider_recent` ✅
- `get_insider_ticker` ✅
- `get_insider_sector` ✅

### ✅ CB-INSTITUTIONS (P2) — Institutional Holdings — PORTED
**Status:** ✅ PORTED — 13f_holdings.c (338 lines C, compiled)
**Free source:** SEC EDGAR 13F filings
**MCP tools:**
- `get_institution_holdings` ✅
- `get_institution_sector` ✅
- `get_institution_changes` ✅

### ✅ CB-SCREENER (P2) — Stock & Options Screener — PORTED
**Status:** ✅ PORTED — stock_screener.c (326 lines C), collector_runner wired every 60min
**Free source:** Local SQLite DBs (joins 12 data sources)
**MCP tools:**
- `get_screener_stocks` ✅
- `get_screener_options` ✅
- `get_screener_analyst` ✅

### ✅ CB-SHORTS (P2) — Short Interest & FTD Tracker — PORTED
**Status:** ✅ PORTED — short_interest_feat.c (727 lines C, compiled)
**Free source:** FINRA short volume, SEC FTD
**MCP tools:**
- `get_short_interest` ✅
- `get_short_volume` ✅
- `get_ftd_data` ✅
- `get_borrow_rate` ✅

### ⚠️ CB-MARKET (P3) — Enhanced Market Data — PARTIAL
**Gap:** Market tide, sector tide, FDA calendar
**What we have:** Macro events (P8), cross-asset (P9), L/S ratio (P37), ETF flow (P40)
**Enhance:** Upgrade `get_market_tide` with yfinance sector ETF data
**Free source:** FRED API (free tier), FDA.gov calendar, BLS

### ✅ CB-EARNINGS (P3) — Earnings Calendar & History — PORTED
**Status:** ✅ PORTED — earnings_calendar.c (251 lines C) + earnings_cal.c (159 lines C, both compiled)
**Free source:** Yahoo Finance earnings calendar
**Cron:** daily

### ✅ CB-ETF (P4) — ETF Holdings & Flow — PORTED
**Status:** ✅ PORTED — etf_flow_feat.c (174 lines C) + etf_holdings.c (151 lines C, both compiled)
**Free source:** Yahoo Finance ETF data via libcurl
**Cron:** every 30min (via collector_runner NORMAL)

### ⚠️ CB-OPTIONS (P4) — Enhanced Options Chain Data — PARTIAL
**Status:** PCR/IV/max pain functional (options_flow.c). Chain-level extraction still needed.
**Free source:** CBOE data shop, Yahoo options chains
**MCP tools needed:**
- `get_option_chain` — full chain for strike/expiry ❌
- `get_option_volume` — volume profile by strike ❌
- `get_option_oi` — open interest by strike ❌

### ✅ CB-SEASONALITY (P5) — Market Seasonality — PORTED
**Status:** ✅ PORTED — seasonality.c (203 lines C, compiled, cron 30min)
**Free source:** yfinance historical data → C computation

### ⚠️ CB-NEWS (P5) — Enhanced News Pipeline — PARTIAL
**Status:** GDELT sentiment (P7) + on-chain/whale (P39) functional. Market news headlines classification still needed.
**Enhance:** Add NewsAPI free tier (100 req/day) + RSS feeds
**MCP tools needed:**
- `get_news_headlines` — financial news per ticker ❌
- `get_news_sentiment` — per-ticker sentiment score ❌

### ✅ CB-POLITICIAN (P6) — Politician Portfolio Tracker — PORTED
**Status:** ✅ PORTED — politician_portfolio.c (388 lines C, compiled, cron 240min)
**Free source:** Extends congress data + holdings aggregation
**MCP tools:**
- `get_politician_portfolio` ✅
- `get_politician_recent` ✅

---

## ── GRID CELL MAPPING ──

Each Clough bill maps to a new cell in the 300-cell grid (added after the existing P60):

| Cell | Bill | Points | Priority | Description | Status |
|------|------|--------|----------|-------------|--------|
| **P61** | CB-STOCK | 5 | P0 | Stock fundamentals pipeline | ✅ DONE |
| **P62** | CB-FLOW | 5 | P0 | Options flow volume alerts | ✅ DONE |
| **P63** | CB-DARKPOOL | 6 | P1 | Dark pool trade tracker | ✅ DONE |
| **P64** | CB-CONGRESS | 5 | P2 | Congressional trade tracker | ✅ DONE |
| **P65** | CB-INSIDER | 5 | P2 | Insider transaction pipeline | ✅ DONE |
| **P66** | CB-INSTITUTIONS | 5 | P2 | 13F filing processor | ✅ DONE |
| **P67** | CB-SHORTS | 5 | P2 | Short interest + FTD tracker | ✅ DONE |
| **P68** | CB-SCREENER | 5 | P2 | Stock/option screener | ✅ DONE |
| **P69** | CB-MARKET | 4 | P3 | Market tide + FDA calendar | ⚠️ PARTIAL |
| **P70** | CB-EARNINGS | 4 | P3 | Earnings calendar + history | ✅ DONE |
| **P71** | CB-ETF | 4 | P4 | ETF holdings + flow | ✅ DONE |
| **P72** | CB-OPTIONS | 5 | P4 | Full option chain extraction | ⚠️ PARTIAL |
| **P73** | CB-SEASONALITY | 3 | P5 | Seasonal pattern computation | ✅ DONE |
| **P74** | CB-NEWS | 3 | P5 | Financial news RSS pipeline | ⚠️ PARTIAL |
| **P75** | CB-POLITICIAN | 3 | P6 | Politician portfolio aggregator | ✅ DONE |

**Total grid impact:** +15 new cells (P61–P75), 36 total MCP tools
**Completed:** 12/15, **Remaining partials:** P69 (Market), P72 (Options), P74 (News)

---

## ── BOTTOM LINE ──

**They have:** 18 tools, 123+ endpoints, API key required ($50-$200/mo)
**We have:** 24+ tools, 55+ endpoints, completely free (12 PORTED, 3 PARTIAL)
**Our edge:** C engine (10K agents, feature importance, tail risk, nested cascade, custom volatility) — they can't touch that
**Their edge:** Real-time OPRA options flow, dark pool — they pay for exchange data
**Our strategy:** Clone their free/free-tierable categories (15/15). Accept the paid categories (real-time Options Flow, real-time Dark Pool) as "nice to have." Win on breadth + unique engine.

**The repo is the banner. The banner is the walk.**
