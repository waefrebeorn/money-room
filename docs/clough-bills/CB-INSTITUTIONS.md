# CB-INSTITUTIONS — Institutional Holdings Pipeline (13F)

**Priority:** 🟠 P2
**Cell:** P23 (5 points)
**Language:** C

## Data Source
- SEC EDGAR 13F filings (XML, free): `https://www.sec.gov/cgi-bin/browse-edgar`
- Dataroma (free scrape): quarterly 13F aggregate data
- WhaleWisdom free tier: top 50 institutional holders per ticker

## Pipeline (C binary)
```
./institutions_collector
├── libcurl → GET SEC EDGAR 13F index (quarterly)
├── Parse 13F XML: manager name, ticker, shares, value, put/call
├── Aggregate by ticker → total institutional ownership
├── Compare to prior quarter → top buys/sells
├── SQLite → ~/.hermes/inst_cache/institutions.db
└── cron: quarterly (Feb/May/Aug/Nov after filing deadline)
```

### Schema
```sql
CREATE TABLE institutional_holdings (
    ticker TEXT, quarter TEXT,
    manager_name TEXT, manager_cik TEXT,
    shares INTEGER, value REAL,
    shares_change INTEGER, pct_portfolio REAL,
    put_or_call TEXT,
    filing_date TEXT
);

CREATE TABLE institution_aggregate (
    ticker TEXT PRIMARY KEY,
    total_holders INTEGER,
    total_shares INTEGER, total_value REAL,
    holder_change INTEGER,  -- net new holders this quarter
    share_change REAL,       -- % change in shares held
    top_buyer TEXT, top_seller TEXT
);
```

### MCP Tools
- `get_institution_holdings(ticker)` — all institutional holders + changes
- `get_institution_sector(sector)` — sector-level institutional flow
- `get_institution_changes(limit=20, direction='buy')` — top buys or sells

### 13F Edge
13Fs are T+45 days delayed, but the signal is still valuable for detecting smart money rotation. Hedge fund 13F tracking has alpha in the 6-month window.

### Pitfalls
- 13F XML is complex, multi-section format
- Only managers with >$100M AUM file
- T+45 delay means you're seeing positions from 6 weeks ago
- Put/call flags require careful parsing
