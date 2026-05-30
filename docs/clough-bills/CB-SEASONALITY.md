# CB-SEASONALITY — Market Seasonality Patterns

**Priority:** ⚪ P5
**Cell:** P28 (3 points)
**Language:** C

## Data Source
- **yfinance historical data** (already collected by CB-STOCK pipeline)
- Pure computation — no new data fetches

## Pipeline (C binary)
```
./seasonality_engine
├── Read historical price data from ~/.hermes/stocks_cache/
├── Compute by month: mean return, win rate, std dev
├── Compute by weekday: Monday effect, Friday effect
├── Compute by quarter: Q1-Q4 patterns
├── Compute holiday window: pre/post holiday returns
├── SQLite → ~/.hermes/seasonality_cache/seasonality.db
└── cron: weekly (computed from existing data, no new fetches)
```

### Schema
```sql
CREATE TABLE monthly_pattern (
    ticker TEXT, month INTEGER,  -- 1-12
    avg_return REAL, win_rate REAL,
    std_dev REAL, sharpe_ratio REAL,
    best_year INTEGER, worst_year INTEGER,
    sample_size INTEGER,
    PRIMARY KEY (ticker, month)
);

CREATE TABLE weekday_pattern (
    ticker TEXT, weekday INTEGER,  -- 0=Mon..4=Fri
    avg_return REAL, win_rate REAL,
    PRIMARY KEY (ticker, weekday)
);

CREATE TABLE seasonal_analytics (
    ticker TEXT PRIMARY KEY,
    best_month INTEGER, best_month_return REAL,
    worst_month INTEGER, worst_month_return REAL,
    january_barometer REAL,
    santa_claus_return REAL,
    summer_slump REAL,
    updated_at TIMESTAMP
);
```

## MCP Tools
- `get_seasonality_monthly(ticker)` — month-by-month return table
- `get_seasonality_pattern(ticker)` — aggregated seasonal analytics

### C Engine Integration
F29: seasonal_factor (best_month - worst_month return spread, normalized). Entering a ticker's best month → boost conviction.

### Pitfalls
- Requires 10+ years of data for statistical significance
- Pure computation, no external source needed
- Sample size matters — display confidence intervals
