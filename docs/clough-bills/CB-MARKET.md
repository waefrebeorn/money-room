# CB-MARKET — Enhanced Market Data (Tide + FDA Calendar)

**Priority:** 🟡 P3
**Cell:** P24 (4 points)
**Language:** C

## Data Sources
1. **FRED API** (free tier): Economic indicators (CPI, GDP, unemployment)
2. **FDA.gov** (free calendar): PDUFA decision dates, advisory committee meetings
3. **BLS.gov** (free): Employment data, jobless claims
4. **yfinance sector ETFs**: SPY, QQQ, IWM, DIA, XLF, XLE, XLV, XLK, XLI, XLP, XLU, XLB, XLY, XLC

## Pipeline (C binary)
```
./market_collector
├── libcurl → GET FRED CPI/GDP/Unemployment series
├── Parse JSON from FRED API
├── libcurl → GET FDA calendar (HTML scrape or RSS)
├── Parse FDA event calendar
├── libcurl → GET sector ETF daily data (via yfinance HTTP API)
├── Compute sector momentum scores
├── SQLite → ~/.hermes/market_cache/market.db
└── cron: every 6h (daily for FDA/FRED)
```

### Schema
```sql
CREATE TABLE sector_tide (
    sector TEXT PRIMARY KEY,
    etf_ticker TEXT,
    momentum_5d REAL, momentum_20d REAL,
    relative_strength REAL,  -- vs SPY
    flow_signal TEXT,        -- 'leading', 'neutral', 'lagging'
    updated_at TIMESTAMP
);

CREATE TABLE market_indicators (
    indicator TEXT PRIMARY KEY,
    value REAL, previous REAL,
    change_pct REAL,
    period TEXT, -- 'monthly', 'quarterly', 'weekly'
    source TEXT,
    updated_at TIMESTAMP
);

CREATE TABLE fda_calendar (
    event_date TEXT, ticker TEXT,
    drug_name TEXT, indication TEXT,
    event_type TEXT,  -- 'PDUFA', 'AdCom', 'Clinical Trial'
    outcome TEXT,
    source TEXT,
    updated_at TIMESTAMP
);
```

## MCP Tools
- `get_market_tide()` — overall market sentiment from sector ETF momentum
- `get_sector_tide(sector=None)` — per-sector momentum with leading/lagging labels
- `get_fda_calendar(limit=10)` — upcoming FDA decision dates
- `get_economic_calendar(limit=10)` — upcoming economic data releases (enhances existing macro pipeline)

### Sector Tide Logic
```
sector_momentum = (price_5d_return * 0.4 + price_20d_return * 0.3 + price_63d_return * 0.3)
relative = sector_momentum - SPY_momentum
if relative > 0.02 → 'leading'
elif relative < -0.02 → 'lagging'
else → 'neutral'
```

### Pitfalls
- FRED requires free API key (register at fred.stlouisfed.org)
- FDA calendar scraping — HTML structure changes occasionally
- Sector ETFs don't capture sub-sector nuance
