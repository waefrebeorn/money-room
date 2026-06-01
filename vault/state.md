# STATE — Money Room Walkway

## Current Status
- **Engine:** 209 C files, 10K agent paper trading (2500 active), 17 markets
- **Website:** GH Pages at waefrebeorn.github.io/money-room/, data_server port 9090
- **Data:** 13 JSON feeds serving live to docs/data/, 24 C crons running
- **Crontab:** 35 system cron entries + 3 Hermes crons (CHANGELOG, morning brief, key rotation)
- **Test:** `make test` — 8/8 pass, `make memcheck` — 0 leaks
- **Latest commit:** CB batch 2 — stock_collector.c (Finnhub fundamentals, 50 tickers)

## Battleship Remaining
Active gaps from battleship-index.md (Clough Bills vs Unusual Whales):
- **CB-STOCK (P0)** — Stock fundamentals: stock_collector.c + options_chain.c + options_flow.c cover 4/6 tools. IV rank infrastructure built (iv_rank.c), need 52 weeks of data. HV10/HV30 volatility calculator still missing. STATE: PARTIAL

## Closed in this batch (current)
- CB-POLITICIAN (P6) → PORTED: politician_portfolio.c (388 lines) compiled 0w 0e, wired collector_runner 240min
- CB-SEASONALITY (P5) → PORTED: seasonality.c (203 lines) was already compiled and running. Verified binary exists, collector_runner wired every 30min
- IV rank tracker: iv_rank.c (180 lines) — reads options flow DBs, computes IV rank/percentile. Writes docs/data/iv_rank.json. Wired collector_runner 60min


## Closed in previous batch
- CB-CONGRESS, CB-INSIDER, CB-DARKPOOL, CB-ETF-FLOW, CB-SCREENER compiled + wired
- CB-INSTITUTIONS, CB-SHORTS confirmed PORTED
