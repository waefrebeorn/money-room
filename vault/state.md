# STATE — Money Room Walkway

## Current Status
- **Engine:** 209 C files, 10K agent paper trading (2500 active), 17 markets
- **Website:** GH Pages at waefrebeorn.github.io/money-room/, data_server port 9090
- **Data:** 13 JSON feeds serving live to docs/data/, 24 C crons running
- **Crontab:** 35 system cron entries + 3 Hermes crons (CHANGELOG, morning brief, key rotation)
- **Test:** `make test` — 8/8 pass, `make memcheck` — 0 leaks
- **Latest commit:** T088 on-chain→pump_score (41a2a9d)

## Battleship Remaining
Active gaps from battleship-index.md (Clough Bills vs Unusual Whales):
- **CB-STOCK (P0)** — Stock fundamentals/technicals: stock_screener.c compiled. Options chain exists. Greeks/IV/max pain still missing. STATE: PARTIAL
- **CB-POLITICIAN (P6)** — Politician portfolios: politician_portfolio.c exists. STATE: UNVERIFIED
- **CB-SEASONALITY (P5)** — Seasonality patterns: seasonality.c exists. STATE: UNVERIFIED

## Closed in this batch
- CB-CONGRESS (P2) → PORTED: congress_trades.c compiled, collector_runner wired 60min
- CB-INSIDER (P2) → PORTED: insider_trades.c compiled, collector_runner wired 60min
- CB-DARKPOOL (P1) → PORTED: dark_pool_feat.c compiled, collector_runner wired 60min
- CB-ETF-FLOW (P4) → PORTED: etf_flow_feat.c compiled, collector_runner wired 30min
- CB-SCREENER (P2) → PORTED: stock_screener.c compiled + wrapper created + collector_runner wired 60min
- CB-INSTITUTIONS → PORTED (confirmed): 13f_holdings.c binary existed
- CB-SHORTS → PORTED (confirmed): short_interest_feat.c binary existed
