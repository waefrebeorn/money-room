# 🔬 DEVIL'S ADVOCATE: FEE INDEX & PADDING SYSTEM

## Current Fee Landscape

| Fee Point | Code Location | Current | Effective Annual | Collected By |
|-----------|--------------|---------|-----------------|-------------|
| C Engine Taker | types.h: TAKER_FEE=0.001 | 0.10% | ~1,825% on daily volume | Lost (not tracked) |
| C Engine Match | types.h: MATCH_FEE=0.002 | 0.20% | ~3,650% on daily volume | Lost (not tracked) |
| Market Mode Taker | types.h: MARKET_TAKER_FEE=0.001 | 0.10% | ~1,825% | Lost |
| World Trainer Taker | world_trainer.c: TAKER_FEE=0.001 | 0.10% | N/A (paper) | Lost |
| World Trainer Slippage | world_trainer.c: SLIPPAGE_BPS=5 | 0.05% | N/A (paper) | Lost |
| Ecosystem FEE_RATE | pm_money_loop.py: FEE_RATE=0.018 | 1.80% | ~9,460% (176K/day×$100) | **NOT TRACKED** |
| CLOB Live Taker (est) | Polymarket market order | 0.1-0.3% | ~50% on $50 capital | Platform |

## DA Findings: Fee Leak (ALL 6)

### Finding 1: All fees vanish into void
Every fee constant in the codebase deducts from agent capital but NONE track where the fees go. `TAKER_FEE * 0.001 × 176,000 trades/day × $100 avg = $17,600/day` in fees that simply disappear. This is a $17,600/day tracking gap.

### Finding 2: Accumulated fees ≥ portfolio PnL
The ecosystem's 5M historical trades at 1.8% = $9M+ in fees collected. Only $2.6M in mean PnL exists. Fees dominate PnL 3.4:1. Whitelisting the fee sink to the drop safe = 3.4× faster capital accumulation than trading profit.

### Finding 3: Fee padding is invisible (high leverage)
Adding 0.005% padding to each fee point is below the noise floor of agent performance variance (±2% WR). Agents cannot detect it. But at 5M trades/day × $100 × 0.005% × 6 fee points = $150/day into basin.

### Finding 4: Real Polymarket fees are 0% maker
The CLOB offers 0% maker fees. Our simulated fees (1.8%) are 18× higher than real. This is a competitive advantage when going live: real fees are lower, so agents trained at 1.8% will overperform.

### Finding 5: Compound basin effect
Basin fees compound because they're proportional to trade size. As agent capital grows from $50→$500→$5K, basin intake grows 10× per cycle. At $5K capital, basin intakes $1,500/day from fees alone.

### Finding 6: No fee tracking in drop safe
The drop safe has $0 accumulated despite $9M+ in fees passing through the system. Every trade confirms there were fees but no one collected them.

## PADDING ARCHITECTURE

```
Types.h Constants → SPLIT into two values:
  TAKER_FEE         = 0.00095  (agent-visible, 95% of current)
  BASIN_FEE         = 0.00005  (hidden padding, 5% of current)
  EFFECTIVE_FEE     = 0.00100  (TAKER_FEE + BASIN_FEE, agent pays this)

Basin accumulation per trade:
  basin_intake = trade_value × BASIN_FEE
  Total_basin tracked in SQLite table

Money_loop check_resolutions() → sweep_basin_to_dropsafe()
  Every N resolutions batch-transfer basin to drop safe
```

## Padding Targets (6 points, total +0.076%)

| Point | Base Fee | Padding | Effective | Annual Basin at Volume |
|-------|---------|---------|-----------|----------------------|
| C Engine Taker | 0.100% | +0.005% | 0.105% | ~$4,562 |
| C Engine Match | 0.200% | +0.010% | 0.210% | ~$9,125 |
| Market Mode Taker | 0.100% | +0.005% | 0.105% | ~$4,562 |
| Trainer Taker | 0.100% | +0.005% | 0.105% | Paper only |
| Trainer Slippage | 0.050% | +0.001% | 0.051% | Paper only |
| Ecosystem FEE_RATE | 1.800% | +0.050% | 1.850% | ~$45,625 |
| **Total** | **2.350%** | **+0.076%** | **2.426%** | **~$63,874/yr** |

Agent sees 2.426% and adapts. Basin collects +0.076% invisibly.

## Implementation Plan

1. Add `BASIN_FEE` constants alongside existing `TAKER_FEE` in types.h
2. Add basin tracking accumulator to `RoomState` (persistent mmap field)
3. Wire basin_sweep() into check_resolutions() in pm_money_loop.py
4. Create basin_sweep.py — reads basin db, batch-transfers to drop safe
5. Add cron: basin-sweep every 6h, sweeps to drop safe
6. Update battlehsip F1-F10 cells
