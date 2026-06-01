# PLAN — Money Room Walkway

## Current Focus
Complete the Clough Bills — closing data pipeline gaps identified in battleship-index.md.

## Priority Queue
1. **Compile missing binaries** — congress_trades, insider_trades, stock_screener, dark_pool_feat, etf_flow_feat, options_flow — add to Makefile, compile, verify
2. **Verify T085/T086** — check if vol surface/news sentiment actually work or need development
3. **Verify T090/T091/T093** — Telegram alerts, perf dashboard, param opt
4. **Update battleship-index.md** — correct stale claims (many "NONE" have C code)
5. **Barnacle sweep** — fix stale numbers across .md tree after every batch
6. **Commit/push** after each batch

## Next Cell
CB-FLOW (P0) — options_flow.c has 667 lines of C source but no compiled binary. Add to Makefile, compile, verify it wires into collector_runner.
