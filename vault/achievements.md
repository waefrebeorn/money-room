# ACHIEVEMENTS — Money Room Vault

## Batch 2026-06-01 — DA Triple Research + CB-STOCK Closure
- **365-cell battleship** (vault/battleship-ultimate.md) — 9-domain gap analysis: 35 🔴 P0, 172 🟡 P1, 158 ⚪ P3
- **65-task homework** (vault/homework-list.md) — 3 tiers: 20 free signups, 25 desk tasks, 20 setup tasks
- **Go-mantra pasteback** for perpetual gap-closing loop
- **Key DA finding:** 16 rooms all share same binary (same md5). 7 on fake 0.50 data. Darwin.epoch=0 across all rooms.
- **volatility_calc.c** (201 lines C) — HV10/HV30 calculator from timeline.db OHLCV
  - 27 tickers: SPY HV10=10.4%, QQQ HV10=16.0%, BTC HV10=17.6%/HV30=20.6%
  - Wired into collector_runner SLOW via ~/.hermes/scripts/volatility_fetch.sh
- **earnings_calendar.c** rebuilt, **earnings_cal.c** (159 lines) compiled for first time
  - Both added to Makefile build targets, clean, and tools list
- **Battleship doc sweep**: corrected "12/15 PORTED, 3 PARTIAL" (was overstating)
  - Execution order section removed (stale past-tense)
  - CB-MARKET, CB-OPTIONS, CB-NEWS honestly labeled PARTIAL with gaps listed
  - Line counts updated to match source (393→stock_collector, 326→screener, etc.)

## Previous Achievements
- CB-POLITICIAN PORTED — politician_portfolio.c (388 lines C, compiled, cron 240min)
- CB-SEASONALITY PORTED — seasonality.c (203 lines C, compiled, cron 30min)
- IV rank tracker — iv_rank.c (181 lines C, wired collector_runner 60min)
- CB-CONGRESS PORTED — congress_trades.c (363 lines C, cron 60min)
- CB-INSIDER PORTED — insider_trades.c (338 lines C, cron 60min)
- CB-DARKPOOL PORTED — dark_pool_feat.c (546 lines C, cron 60min)
- CB-INSTITUTIONS PORTED — 13f_holdings.c (338 lines C)
- CB-SCREENER PORTED — stock_screener.c (326 lines C, cron 60min)
- CB-SHORTS PORTED — short_interest_feat.c (727 lines C)
- CB-ETF PORTED — etf_flow_feat.c (174 lines C) + etf_holdings.c (151 lines C)
- CB-EARNINGS PORTED — earnings_calendar.c (251 lines C) + earnings_cal.c (159 lines C)
- Key rotation health monitor — key_rotation.c (314 lines, 16 API keys, daily cron)
- Min trade stake enforcement — MIN_TRADE_STAKE=$1 in types.h/room_capital.c
- On-chain blending into pump_score — feed_bridge.c (BTC dominance 30% weight)
