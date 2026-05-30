# CB-CONGRESS — Congressional Trading Tracker

**Priority:** 🟠 P2
**Cell:** P21 (5 points)
**Target:** Clone UW congressional trading tools
**Language:** C (HTTP fetch + HTML/JSON parse)

## Data Source
- **capitoltrades.com** — Free API (100 req/day, no key needed for basic queries)
- **Senate.gov** — STOCK Act filings (XML, public)
- **House.gov** — House financial disclosures (XML, public)
- All free, public records

## Pipeline Architecture

```
C binary: congress_collector
├── libcurl → GET capitoltrades.com/trades?ticker=NVDA
├── libcurl → GET senate.gov/stock-act-feed.xml
├── cJSON parse → extract trade records
├── Write SQLite → ~/.hermes/congress_cache/congress.db
└── cron: daily, no_agent=true
```

### C Source Files
- `scripts/c/congress_collector.c` — multi-source aggregator
- Channel-based: fetch from all sources, merge, dedupe by trade_id

### Schema
```sql
CREATE TABLE congress_trades (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker TEXT NOT NULL,
    member_name TEXT NOT NULL,
    member_chamber TEXT,  -- 'Senate' or 'House'
    member_party TEXT,     -- 'D', 'R', 'I'
    member_state TEXT,
    trade_date TEXT,
    report_date TEXT,
    transaction_type TEXT, -- 'buy' or 'sell'
    amount_range TEXT,     -- '$1K-$15K', '$15K-$50K', etc.
    amount_min REAL,
    amount_max REAL,
    is_late_report INTEGER,
    source TEXT,           -- 'capitoltrades', 'senate_gov', 'house_gov'
    detected_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

## MCP Tools

### `get_congress_recent(limit=20) → list`
Most recent congressional trades across all members

### `get_congress_trader(name='Pelosi', ticker=None) → list`
Trades by a specific member, optionally filtered by ticker

### `get_congress_ticker(ticker) → list`
All congressional trades for a specific ticker, with party breakdown

## Performance Note
Congressional trading has predictable high-alpha tickers: NVDA, AAPL, MSFT, TSLA, AMZN, GOOGL, CRWD, PLTR, MRNA — preload these.

## Verification
```
./congress_collector --ticker NVDA  # NVDA congressional trades, last 30 days
```
