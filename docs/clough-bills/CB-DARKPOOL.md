# CB-DARKPOOL — Dark Pool Trade Tracker

**Priority:** 🟠 P1
**Cell:** P20 (6 points)
**Target:** Clone UW dark pool tools
**Language:** C (FINRA CSV downloader + parser)

## Data Source
- **FINRA OTC Transparency Data** — https://otctransparency.finra.org/
- Free weekly CSV files of off-exchange transactions
- Covers ATS and non-ATS trades, includes: ticker, date, volume, price, venue
- T+2 publishing (2-day lag)

## Pipeline Architecture

```
C binary: darkpool_collector
├── libcurl → GET FINRA weekly CSV
├── Parse CSV fields → trade structs
├── Aggregate by ticker → daily dark pool volume/price
├── Write SQLite → ~/.hermes/darkpool_cache/darkpool.db
└── cron: weekly (Sunday), no_agent=true
```

### C Source Files
- `scripts/c/darkpool_collector.c` — HTTP download, CSV parser, SQLite writer
- Uses built-in `strtok` for CSV parsing (no external CSV lib needed)

### Schema
```sql
CREATE TABLE darkpool_trades (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker TEXT NOT NULL,
    trade_date TEXT NOT NULL,
    volume INTEGER, price REAL,
    total_value REAL,
    venue TEXT,              -- 'ATS' or 'Non-ATS'
    is_block_trade INTEGER,  -- volume > 10,000 shares
    reported_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE darkpool_aggregate (
    ticker TEXT PRIMARY KEY,
    week_ending TEXT,
    total_volume INTEGER,
    block_volume INTEGER,
    avg_trade_size INTEGER,
    avg_premium REAL,
    total_trades INTEGER,
    darkpool_ratio REAL,  -- dark vol / total vol (approximate)
    updated_at TIMESTAMP
);
```

## MCP Tools

### `get_darkpool_recent(limit=50, min_value=0) → list`
Most recent dark pool trades, optionally filtered by trade value

### `get_darkpool_ticker(ticker) → dict`
Aggregate dark pool stats for specific ticker: weekly volume, block activity, darkpool ratio

## Limitations
- T+2 delay (not real-time — FINRA publishes with 2-day lag)
- Weekly aggregation (not per-trade — FINRA gives weekly CSVs)
- No NBBO context (UW has real-time dark pool with NBBO, they pay for it)
- Still useful for: detecting institutional accumulation/distribution over weeks

## Verification
```
./darkpool_collector --ticker SPY  # print dark pool stats for SPY
```
