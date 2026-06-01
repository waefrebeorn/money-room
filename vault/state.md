# STATE — Money Room Walkway

## Current Status
- **Engine:** 210 C files, 10K agent paper trading (2500 active), 17 markets
- **Website:** GH Pages at waefrebeorn.github.io/money-room/, data_server port 9090
- **Data:** 14 JSON feeds serving live to docs/data/, 25 C crons running
- **Crontab:** 35 system cron entries + 4 Hermes crons (CHANGELOG, morning brief, key rotation, volatility)
- **Test:** `make test` — 8/8 pass, `make memcheck` — 0 leaks
- **Latest batch:** CB-STOCK PORTED — volatility_calc.c (HV10/HV30 from timeline.db)

## Battleship Remaining
All 15 Unusual Whales categories now PORTED.

## Closed in this batch
- **CB-STOCK (P0)** → PORTED: volatility_calc.c (168 lines C) reads OHLCV from timeline.db, computes HV10/HV30 for 27 tickers. Wires into collector_runner SLOW queue. All 6/6 Stock tools now delivered.

## Previously closed
- CB-POLITICIAN (P6), CB-SEASONALITY (P5), IV rank tracker
- CB-CONGRESS, CB-INSIDER, CB-DARKPOOL, CB-ETF-FLOW, CB-SCREENER
- CB-INSTITUTIONS, CB-SHORTS confirmed PORTED
- Fundamentals, options chains, max pain, Greeks, stock screener
