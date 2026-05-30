# CB-SCREENER — Stock & Options Screener

**Priority:** 🟠 P2
**Cell:** P29 (5 points)
**Language:** C

## Data Source
All data already collected by other pipelines (CB-STOCK, CB-FLOW, CB-SHORTS, etc.). Screener is **computed** from existing SQLite databases — no separate data collection needed.

## Engine (C binary)
```
./screener_engine [--filter volume>1M] [--filter iv_rank>80] [--output json]
├── Read all SQLite DBs (~/.hermes/*_cache/*.db)
├── JOIN tickers across tables
├── Apply filter predicates (volume, IV rank, short ratio, insider flow...)
├── Rank results by composite score
├── Output JSON → stdout
└── No cron (on-demand query tool)
```

### Filter Predicates (all combinable via AND)
- `volume>` / `volume<` — daily trading volume
- `iv_rank>` / `iv_rank<` — implied volatility percentile
- `short_ratio>` — short volume ratio (squeeze candidates)
- `insider_net>` — net insider buying
- `price>` / `price<` — price range
- `sector=` — sector filter (tech, energy, healthcare...)
- `market_cap>` — market cap floor
- `dividend>` — dividend yield minimum

### Score Formula (in C)
```c
double score = 0.0;
score += 0.20 * normalize(iv_rank, 0, 100);      // IV percentile
score += 0.15 * normalize(volume_surge, 0, 10);  // volume surge ratio
score += 0.15 * normalize(short_ratio, 0, 1);    // short squeeze pressure
score += 0.10 * normalize(insider_buy_ratio, -1, 1); // insider signal
score += 0.10 * normalize(institution_change, -1, 1);// 13F signal
score += 0.10 * normalize(flow_premium, 0, 1e6); // options flow premium
score += 0.10 * normalize(rsi, 0, 100);           // RSI oversold/overbought
score += 0.10 * normalize(momentum_5d, -0.2, 0.2); // 5-day momentum
```

## MCP Tools
- `get_screener_stocks(filters)` — ranked stock screener results
- `get_screener_options(min_iv=50, max_expiry_days=60)` — highest IV optionable tickers
- `get_screener_bullish()` — stocks with confluence of bullish signals
- `get_screener_bearish()` — stocks with confluence of bearish signals

### Verification
```
./screener_engine --filter iv_rank>80 --filter short_ratio>0.4 --limit 10
```
