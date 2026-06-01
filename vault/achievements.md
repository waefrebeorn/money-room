# ACHIEVEMENTS — Money Room Vault

## Batch 2026-06-01 — CB-STOCK Closure
- **volatility_calc.c** (168 lines C) — HV10/HV30 calculator from timeline.db OHLCV
  - Reads close prices from yahoo_collector data, computes rolling HV10/HV30
  - Outputs docs/data/volatility.json for 27 tickers (ETFs, sectors, commodities, crypto)
  - Wires into collector_runner SLOW queue, wrapper at ~/.hermes/scripts/volatility_fetch.sh
  - All 6/6 Stock tools PORTED: info, chains, max pain, IV rank, volatility, Greeks
- Battleship fully cleared: 15/15 Unusual Whales categories PORTED

## Previous Achievements
- CB-POLITICIAN PORTED (politician_portfolio.c 388 lines)
- CB-SEASONALITY confirmed PORTED (seasonality.c)
- IV rank tracker (iv_rank.c 180 lines)
- 7 CB categories PORTED in prior batches
- Key rotation health monitor (key_rotation.c 314 lines)
- Min trade stake enforcement
- On-chain blending into pump_score (feed_bridge.c)
