# CB-STOCK — Stock Fundamentals & Technicals Pipeline

**Priority:** 🔴 P0
**Cell:** P17 (5 points)
**Target:** Clone 8 Unusual Whales stock tools
**Language:** C (yfinance via libcurl + JSON parsing)

## Data Source
- **yfinance API** (Yahoo Finance HTTP endpoints, reverse-engineered)
- Covers: OHLC, dividends, splits, fundamentals, options chains, institutional holders
- No API key — free HTTP queries

## Pipeline Architecture

```
C binary: stock_collector
├── libcurl → GET https://query1.finance.yahoo.com/v8/finance/chart/SPY
├── libcurl → GET https://query1.finance.yahoo.com/v7/finance/options/SPY
├── cJSON parse → extract fields
├── sqlite3 INSERT → ~/.hermes/stocks_cache/stocks.db
└── cron: every 1h market hours, no_agent=true
```

### C Source Files
- `scripts/c/stock_collector.c` — HTTP fetcher + JSON parser + SQLite writer
- `scripts/c/include/stock_types.h` — struct definitions
- Build: `gcc -o stock_collector stock_collector.c -lcurl -lcjson -lsqlite3 -lm`

### Schema
```sql
CREATE TABLE stock_info (
    ticker TEXT PRIMARY KEY,
    name TEXT, sector TEXT, industry TEXT,
    market_cap REAL, pe_ratio REAL, eps REAL,
    dividend_yield REAL, beta REAL,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE option_chains (
    ticker TEXT, expiry TEXT, strike REAL,
    option_type TEXT,  -- 'call' or 'put'
    last_price REAL, bid REAL, ask REAL,
    volume INTEGER, open_interest INTEGER,
    implied_vol REAL, delta REAL, gamma REAL,
    theta REAL, vega REAL,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE iv_rank (
    ticker TEXT PRIMARY KEY,
    current_iv REAL, iv_52w_high REAL, iv_52w_low REAL,
    iv_rank REAL, iv_percentile REAL,
    updated_at TIMESTAMP
);

CREATE TABLE max_pain (
    ticker TEXT, expiry TEXT,
    max_pain_strike REAL,
    total_oi_calls INTEGER, total_oi_puts INTEGER,
    updated_at TIMESTAMP,
    PRIMARY KEY (ticker, expiry)
);
```

## MCP Tools (Python MCP server reads SQLite → returns JSON)

### `get_stock_info(ticker) → dict`
Query stock_info table, return name/sector/PE/mcap/etc.

### `get_stock_option_chains(ticker, expiry) → dict`
Query option_chains table, return strikes with IV/Greeks/OI

### `get_stock_max_pain(ticker, expiry) → dict`
Query max_pain table, compute from OI distribution

### `get_stock_iv_rank(ticker) → dict`
Query iv_rank, return current + 52w range + percentile

### `get_stock_volatility(ticker) → dict`
Compute HV from historical close prices (stddev of log returns)

## Build Dependencies
```
apt install libcurl4-openssl-dev libcjson-dev libsqlite3-dev
```

## Key Pitfalls
- Yahoo Finance rate limits ~2000 req/hr/IP; stagger symbol lists
- Options chains only for actively traded names (SPY, QQQ, AAPL, etc.)
- Greeks are model-derived from Yahoo's side — trust but verify
- Need user-agent rotation to avoid 404 from Yahoo
