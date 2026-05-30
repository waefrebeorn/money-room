# CB-POLITICIAN — Politician Portfolio Tracker

**Priority:** ⚪ P6
**Cell:** P31 (3 points)
**Language:** C (extends CB-CONGRESS)

## Data Source
- **SEC EDGAR** (same pipeline as CB-CONGRESS) — all congressional trading filings
- Extended analysis from existing `congress.db` database

## Pipeline (C binary)
```
./politician_engine
├── Read ~/.hermes/congress_cache/congress.db
├── Aggregate by member: total portfolio value, diversification
├── Compute performance: mark portfolio → track if portfolio beats SPY
├── Pattern detection: buy after sell, sell after buy, concentrated bets
├── SQLite → ~/.hermes/congress_cache/congress.db (adds portfolio table)
└── cron: daily (extends existing congress run)
```

### Schema (new tables in congress.db)
```sql
CREATE TABLE politician_portfolio (
    member_name TEXT,
    ticker TEXT,
    total_buys REAL, total_sells REAL,
    net_position REAL,
    first_trade_date TEXT, last_trade_date TEXT,
    trade_count INTEGER,
    PRIMARY KEY (member_name, ticker)
);

CREATE TABLE politician_performance (
    member_name TEXT PRIMARY KEY,
    portfolio_return REAL,  -- vs SPY
    win_rate REAL,           -- % of profitable trades
    avg_trade_return REAL,
    total_trades INTEGER,
    rank INTEGER            -- rank among all members
);
```

## MCP Tools
- `get_politician_portfolio(name)` — full portfolio holdings for a politician
- `get_politician_performance(name)` — portfolio performance vs SPY
- `get_politician_top_picks()` — top 10 most-held stocks across congress

### Premium Edge
Unusual Whales charges premium for politician portfolios. Ours is free — same data (SEC EDGAR), just our analysis on top.

### Performance Ranking
Track and publish "Congressional Trading Leaderboard" — members ranked by portfolio return vs SPY over 1y/5y/10y.

### Pitfalls
- Amount ranges ($1K-$15K, $15K-$50K, etc.) limit precision
- Not all trades are disclosed within 45 days
- Spouse/kids' trades reported under member name
- Use midpoint of range for value approximation
