# CB-ETF — ETF Holdings & Flow

**Priority:** 🟡 P4
**Cell:** P26 (4 points)
**Language:** C

## Data Source
- **Yahoo Finance ETF data** (HTTP API, free): holdings, sector breakdown
- **etfdb.com** scrape: ETF inflows/outflows, category data

## Pipeline (C binary)
```
./etf_collector
├── libcurl → GET yahoo ETF holdings for top 50 ETFs
├── Parse holding: ticker, shares, value, weight_pct
├── Compute sector/thematic aggregation
├── SQLite → ~/.hermes/etf_cache/etf.db
└── cron: daily
```

### Schema
```sql
CREATE TABLE etf_holdings (
    etf_ticker TEXT, holding_ticker TEXT,
    shares INTEGER, market_value REAL,
    weight_pct REAL,
    sector TEXT,
    as_of_date TEXT,
    PRIMARY KEY (etf_ticker, holding_ticker)
);

CREATE TABLE etf_info (
    etf_ticker TEXT PRIMARY KEY,
    name TEXT, category TEXT,
    aum REAL, expense_ratio REAL,
    holdings_count INTEGER,
    top_sector TEXT,
    dividend_yield REAL,
    updated_at TIMESTAMP
);

CREATE TABLE etf_aggregate_flow (
    etf_ticker TEXT PRIMARY KEY,
    flow_1d REAL, flow_5d REAL, flow_20d REAL,
    flow_signal TEXT, -- 'inflow', 'outflow', 'neutral'
    updated_at TIMESTAMP
);
```

## MCP Tools
- `get_etf_holdings(etf_ticker)` — top holdings for an ETF with weights
- `get_etf_info(etf_ticker)` — AUM, expense ratio, category, sector breakdown
- `get_etf_sector(exposure_ticker)` — which ETFs hold the most of a given ticker
- `get_etf_flow(etf_ticker)` — flow direction and magnitude

### Key Insight
ETFs reveal smart money flow. When SPY sees massive inflows, the market is bullish. When QQQ outflows while IWM inflows, it's a rotation signal. Track this as F28 (etf_flow_signal) in the C engine.

### Pitfalls
- Yahoo ETF holdings data quality varies per fund
- etfdb.com scraping is fragile (HTML structure changes)
- Only track major ETFs (SPY, QQQ, IWM, DIA, XLF, XLK, XLE, XLV, etc.) — ~50 is enough
- Flow data computed from AUM changes, not actual creation/redemption data
