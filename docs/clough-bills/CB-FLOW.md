# CB-FLOW — Options Flow Alerts Pipeline

**Priority:** 🔴 P0
**Cell:** P19 (5 points)
**Target:** Clone UW options flow alerting
**Language:** C (yfinance options chain diff + volume surge detection)

## Data Source
- Yahoo Finance options chains (same API as CB-STOCK)
- Diff OI and volume between consecutive fetches → detect unusual activity
- CBOE delayed quotes (free, 15-min delayed)

## Pipeline Architecture

```
C binary: flow_collector
├── Every 15min: fetch option chains for top 200 tickers
├── Compare current OI vs last-seen OI → OI change signal
├── Volume vs avg_volume(5d) → volume surge detection
├── Write flow events → ~/.hermes/flow_cache/flow.db
└── cron: every 15min, no_agent=true
```

### C Source Files
- `scripts/c/flow_collector.c` — OI/volume diff engine, alert generation
- `scripts/c/include/flow_types.h` — flow_event, alert structs
- Reuses `stock_collector.c` option chain fetcher as shared module

### Schema
```sql
CREATE TABLE flow_alerts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker TEXT NOT NULL,
    expiry TEXT, strike REAL,
    option_type TEXT,
    volume INTEGER, avg_volume_5d INTEGER,
    volume_ratio REAL,
    oi_change INTEGER,
    premium_est REAL,
    alert_type TEXT,  -- 'volume_surge', 'oi_spike', 'unusual_flow'
    severity TEXT,    -- 'low', 'medium', 'high'
    detected_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE flow_snapshot (
    ticker TEXT, expiry TEXT, strike REAL,
    option_type TEXT,
    volume INTEGER, open_interest INTEGER,
    snapshot_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (ticker, expiry, strike, option_type, snapshot_at)
);
```

### Alert Detection Logic (in C)
1. **Volume surge**: current_volume > 3× avg_volume_5d AND volume > 100
2. **OI spike**: OI_change > 2× avg_oi AND abs(change) > 200
3. **Unusual flow**: premium > $100K (volume × mid_price)
4. **Sector aggregation**: sum alerts by sector group (mag7, semis, banks, energy)

## MCP Tools

### `get_flow_alerts(ticker=None, min_severity='medium') → list`
Return active flow alerts, sorted by severity and premium

### `get_flow_net_premium(ticker) → dict`
Net premium (calls - puts) for last 24h

### `get_flow_sector(sector=None) → dict`
Sector-level flow aggregation

## Verification
```
./flow_collector SPY QQQ AAPL TSLA NVDA  # fetch 5 tickers, print alerts
```
