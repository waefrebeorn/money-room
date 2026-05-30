# CB-SHORTS — Short Interest & FTD Tracker

**Priority:** 🟠 P2
**Cell:** P27 (5 points)
**Language:** C

## Data Sources
1. **FINRA Short Volume** (daily): `https://regsho.finra.org/regsho-issue/FNYXshvol.csv`
2. **SEC FTD Data** (biweekly): `https://www.sec.gov/files/data/fails-deliver-data/`
3. **iborrowdesk** (free): borrow rate + availability for top tickers

## Pipeline (C binary)
```
./shorts_collector
├── libcurl → GET FINRA daily short volume CSV
├── Parse → ticker, short_volume, total_volume, short_ratio
├── libcurl → GET SEC FTD CSV
├── Parse → ticker, fails, settlement_date
├── libcurl → GET iborrowdesk scrape
├── Extract borrow rate, availability score
├── SQLite → ~/.hermes/shorts_cache/shorts.db
└── cron: daily
```

### Schema
```sql
CREATE TABLE short_volume (
    ticker TEXT, date TEXT,
    short_volume INTEGER, total_volume INTEGER,
    short_ratio REAL,  -- short_vol / total_vol
    PRIMARY KEY (ticker, date)
);

CREATE TABLE ftd_data (
    ticker, settlement_date TEXT,
    fails INTEGER, dollar_value REAL,
    price REAL, PRIMARY KEY (ticker, settlement_date)
);

CREATE TABLE borrow_rates (
    ticker TEXT PRIMARY KEY,
    borrow_rate REAL,
    availability TEXT,  -- 'easy', 'medium', 'hard'
    updated_at TIMESTAMP
);
```

## MCP Tools
- `get_short_interest(ticker)` — current short volume ratio + trend (5d/30d)
- `get_ftd_data(ticker)` — failure-to-deliver history, squeeze flags
- `get_borrow_rate(ticker)` — stock borrow cost + availability
- `get_squeeze_candidates(min_short_ratio=0.3, min_ftd_increase=2.0)` — squeeze screen

### C Engine Integration
Short data feeds directly into the C engine as F22 (short_ratio), F23 (borrow_rate), F24 (ftd_spike). Short squeeze = short_ratio > 40% + borrow hard + FTD spiking = high squeeze probability.

### Pitfalls
- FINRA short volume is daily, FTD is biweekly (2-week delay)
- Borrow rates change intraday — daily snapshot only
- FINRA CSVs are large (~50MB) — stream parse, don't load entirely
