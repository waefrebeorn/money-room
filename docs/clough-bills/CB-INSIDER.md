# CB-INSIDER — Insider Transaction Tracker

**Priority:** 🟠 P2
**Cell:** P22 (5 points)
**Language:** C

## Data Source
- SEC EDGAR Form 4 XML (free API, no key): `https://www.sec.gov/cgi-bin/browse-edgar`
- SEC EDGAR full-index: `https://www.sec.gov/Archives/edgar/full-index/`
- InsiderMonkey free tier scrape (daily top insider trades)

## Pipeline (C binary)
```
./insider_collector
├── libcurl → GET SEC EDGAR Form 4 index
├── Parse XML: issuer, reporting owner, transaction type, shares, price
├── Classify: buy/sell, open-market/exercise/grant/award
├── Aggregate by ticker → net insider flow
├── SQLite → ~/.hermes/insider_cache/insider.db
└── cron: daily
```

### Schema
```sql
CREATE TABLE insider_trades (
    ticker TEXT, filing_date TEXT,
    insider_name TEXT, insider_title TEXT,
    transaction_type TEXT, shares INTEGER,
    price REAL, value REAL,
    ownership_after INTEGER,
    is_direct INTEGER, is_open_market INTEGER,
    sec_accession TEXT UNIQUE
);

CREATE TABLE insider_aggregate (
    ticker TEXT PRIMARY KEY,
    buys_30d INTEGER, sells_30d INTEGER,
    net_flow REAL, buy_sell_ratio REAL,
    insider_signal TEXT -- 'bullish', 'bearish', 'neutral'
);
```

### MCP Tools
- `get_insider_recent(limit=50)` — recent open-market insider trades
- `get_insider_ticker(ticker)` — insider activity for a ticker with buy/sell ratio
- `get_insider_sector(sector)` — aggregated insider flow by sector

### Key Insight
Insider buying at >2× normal volume is a strong bullish signal. C engine should incorporate this as a feature (F21: insider_buy_ratio).

### Pitfalls
- SEC EDGAR rate limits: 10 req/sec, use User-Agent header
- Grants/awards vs open-market purchases — only open-market counts as signal
- Need to parse XML, not just HTML. Use libxml2 or manual string parsing
