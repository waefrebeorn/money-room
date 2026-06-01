# STATE — Money Room Walkway

## Current Status
- **Engine:** 210 C files, 10K agent paper trading (2500 active), 17 markets
- **Website:** GH Pages at waefrebeorn.github.io/money-room/, data_server port 9090
- **Data:** 14 JSON feeds serving live to docs/data/, 25 C crons running
- **Crontab:** 35 system cron entries + 4 Hermes crons (CHANGELOG, morning brief, key rotation, volatility)
- **Test:** `make test` — 8/8 pass, `make memcheck` — 0 leaks
- **Latest batch:** DA cleanup — battleship corrected, earnings_cal compiled, vault cleaned

## Battleship Status
12/15 Unusual Whales categories PORTED.
3 PARTIAL: CB-MARKET (sector tide/FDA), CB-OPTIONS (chain-level extraction), CB-NEWS (news headlines)

## Closed in this batch
- **DA cleanup**: Fixed overstated header "All 15 PORTED" → "12/15 PORTED, 3 PARTIAL"
- **earnings_cal.c** (159 lines) compiled for first time — was claimed "compiled" but no binary existed
- **earnings_calendar** binary rebuilt (source was 7h newer than stale binary)
- **Battleship doc** stripped stale execution order, updated line counts, marked partials honestly
- **Vault** cleaned: replaced vague "7 CB categories" / "Min trade stake enforcement" with specific file:line refs

## Previously closed
- CB-STOCK PORTED: volatility_calc.c (HV10/HV30)
- CB-POLITICIAN, CB-SEASONALITY, IV rank tracker
- CB-CONGRESS, CB-INSIDER, CB-DARKPOOL, CB-ETF-FLOW, CB-SCREENER
- CB-INSTITUTIONS, CB-SHORTS confirmed PORTED
- Fundamentals, options chains, max pain, Greeks, stock screener
