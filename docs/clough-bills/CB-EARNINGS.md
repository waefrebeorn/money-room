# CB-EARNINGS — Earnings Calendar & History

**Priority:** 🟡 P3
**Cell:** P25 (4 points)
**Language:** C

## Data Source
- **Yahoo Finance earnings calendar** (HTTP API, free): `https://query1.finance.yahoo.com/calendar/earnings`
- **Financial Modeling Prep** free tier (250 req/day): earnings history with EPS surprises

## Pipeline (C binary)
```
./earnings_collector
├── libcurl → GET yahoo earnings calendar (next 30 days)
├── cJSON parse → ticker, date, EPS estimate, expected move
├── libcurl → GET FMP historical earnings
├── Parse → actual EPS, estimated EPS, surprise %
├── SQLite → ~/.hermes/earnings_cache/earnings.db
└── cron: daily
```

### Schema
```sql
CREATE TABLE earnings_calendar (
    ticker TEXT, report_date TEXT,
    quarter TEXT, fiscal_year INTEGER,
    eps_estimate REAL, eps_actual REAL,
    surprise_pct REAL,
    revenue_estimate REAL, revenue_actual REAL,
    expected_move REAL, -- IV-derived expected move
    time_of_day TEXT,   -- 'bmo', 'amc', 'during'
    PRIMARY KEY (ticker, quarter, fiscal_year)
);

CREATE TABLE earnings_analytics (
    ticker TEXT PRIMARY KEY,
    avg_surprise REAL,    -- avg surprise over last 8 quarters
    beat_rate REAL,        -- % of quarters that beat
    avg_move REAL,         -- avg post-earnings move
    iv_skew REAL,          -- current IV skew (premium expensive?)
    updated_at TIMESTAMP
);
```

## MCP Tools
- `get_earnings_calendar(date=None, limit=20)` — upcoming earnings with expected move
- `get_earnings_history(ticker)` — historical EPS surprises + beat rate
- `get_earnings_ticker(ticker)` — next earnings date, estimate, expected move + historical analytics

### C Engine Integration
Earnings events are powerful features for the C engine:
- F25: days_to_earnings (0-90, high = far away)
- F26: earnings_beat_rate (0-1, historical)
- F27: expected_move_pct (IV-derived)
- On earnings day: special regime mode with higher conviction thresholds

### Pitfalls
- Yahoo earnings calendar only shows ~30 days out
- Expected move from IV requires options chain data (CB-STOCK dependency)
- FMP free tier is 250 req/day — cache results aggressively
