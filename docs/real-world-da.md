# 🔬 DEVIL'S ADVOCATE: REAL-WORLD TRADING CONSTRAINTS

## The Core Problem

**The 10K genome ecosystem assumes 10,000 independent traders executing simultaneously.**
In reality, ONE person (you) has:
- ONE Polymarket account
- ONE wallet ($50)
- ONE API key
- ONE IP address (WSL laptop)

**10K agents isn't a trading army. It's 10K simulated strategies competing for ONE slot.**

---

## CONSTRAINT 1: Rate Limits Kill Parallelism

| Platform | Endpoint | Limit | What This Means |
|----------|----------|:-----:|-----------------|
| Polymarket CLOB | Order placement | ~9K/10s | Fine for 1 order/5min. Impossible for 10K simultaneous. |
| Polymarket Gamma | Market data | 4K/10s | Can scan 500 markets. 10K agents don't each get their own scan. |
| Kalshi REST | Order placement | Unknown (~10/s) | 1 genome at a time. |
| Kraken OHLC | Public data | ~1/s | Fine for 1 genome's data feed. |

**Verdict:** You can execute ONE order at a time. Not 10K.

---

## CONSTRAINT 2: Capital Limits ($50 ≠ 10K × $1K)

The paper ecosystem gives each of 10K agents $1,000 → $10M total paper capital.

**Reality with $50:**
- 1 trade per 5-min window
- Max $10/trade (20% Kelly)
- 288 trades/day max (across all assets)
- $0.30-1.50/day expected at 50-65% WR
- 10K agents sharing $50 = 0.5¢ each → can't even buy 1 share

**Verdict:** Only ONE genome's signal gets traded per window. Not a portfolio of 10K.

---

## CONSTRAINT 3: Position Limits

**Polymarket:**
- Max position per market: varies by liquidity ($500-$5K typical)
- With $50 wallet: $10/trade max = well under limits (fine)

**Kalshi (CFTC-regulated):**
- Some contracts have position limits (election: 10K contracts max)
- KYC required (1 identity = 1 account)
- $10 minimum deposit

**SEC/CFTC:**
- Prediction markets are regulated as commodity derivatives
- Kalshi is a DCM (Designated Contract Market)
- Polymarket settled with CFTC ($1.4M fine, 2024)
- Operating as unregistered exchange = risk

---

## CONSTRAINT 4: 10K Genomes = 1 Strategy Selector

**The real architecture is NOT "10K agents trade live."**
It's:

```
Paper Ecosystem (10K agents, $10M paper)
       ↓ Darwin evolution
       ↓ Fitness ranking
       ↓
TOP GENOME SELECTED (best PnL, ≥10 trades)
       ↓
       ↓  Genome params → position_size, conviction, risk_tolerance
       ↓  feat_weight → signal computation
       ↓
ONE REAL ORDER on Polymarket 5-min BTC
       ↓
Result fed back to ecosystem → Darwin learns from REAL PnL
```

The 10K agents are a **GENETIC ALGORITHM FOR STRATEGY DISCOVERY**, not a trading collective.

---

## CONSTRAINT 5: Market Timing

**Paper profit ≠ real profit.**
- Paper: trades resolve in 60s
- Real: Polymarket 5-min windows resolve in ~5 min
- Real slippage: FOK market orders fill at the book, not midprice
- Real settlement: USDC arrives after ~30-60s resolution lag
- Real fees: 0.1-0.3% taker vs 0% simulated

**Trade cadence:**
```
Paper: 1,440 trades/day/asset (1-min Wubu Market)
Real:  12 trades/day/asset (5-min windows, 1 order/window)
      = 48 trades/day across 4 assets (BTC/ETH/SOL/XRP)
      = 0.003% of paper volume
```

---

## CONSTRAINT 6: API Key + Identity = Single Thread

- ONE Polymarket CLOB API key
- ONE rate limit bucket
- ONE Polygon wallet
- ONE IP (WSL, ~200ms latency)

**Can't parallelize across agents.** The "10K agent ecosystem" is a **batch backtest engine** that picks the best single strategy, then that ONE strategy trades live.

---

## REVISED ARCHITECTURE: Survival Genome Deployment

```
╔══════════════════════════════════════════════════════════╗
║                   PAPER ECOSYSTEM                        ║
║  10K agents × 80-dim features × Darwin evolution         ║
║  $10M paper capital  |  176K+ trades/day                 ║
║  Fitness ranking every tick                               ║
╚══════════════════════════════════════════════════════════╝
                      │
                      ▼  Top genome by PnL
         ┌───────────────────────────┐
         │      SURVIVOR QUEUE        │
         │  Top-10 genomes by fitness  │
         │  (PnL, WR, Sharpe, trades)  │
         └───────────────────────────┘
                      │
                      ▼  #1 genome's feat_weight
         ┌───────────────────────────┐
         │    LIVE TRADE SIGNAL       │
         │  1 genome × 1 asset × 1 window    │
         │  Kelly sized: $2.50-$10.00         │
         │  Max: 48 trades/day (4 assets)     │
         └───────────────────────────┘
                      │
                      ▼  Resolution feedback
         ┌───────────────────────────┐
         │     REAL PNL FEEDBACK      │
         │  Updates genome portfolio   │
         │  Darwin evolves on REAL PnL │
         └───────────────────────────┘
```

---

## IMPLICATIONS

### What the 10K agents ARE:
- **Strategy discovery engine** — find which genome params + weight sets work
- **Backtest validation** — 176K trades/day proves statistical significance 
- **Regime adaptation** — which genome works in which market regime
- **Diversity pool** — if #1 starts losing, #2-#10 are ready to swap in

### What they are NOT:
- Simultaneous live traders
- A diversified portfolio
- A way to scale $50 across 10K positions

### Scaling Path (Realistic):

| Capital | Trades/Day | Per-Trade | Daily Return (50% WR) | Days to Next Tier |
|---------|:----------:|:---------:|:---------------------:|:-----------------:|
| $50 | 48 | $5-10 | $0.30-$1.50 | 33-167 |
| $100 | 48 | $10-20 | $0.60-$3.00 | 33-167 |
| $500 | 48 | $50-100 | $3.00-$15.00 | 33-167 |
| $2K | 48 | $200-400 | $12-$60 | 33-83 |
| $5K | 48 | $500-1K | $30-$150 | 33-167 |

**Growth is linear with capital, not exponential with agent count.**

---

## CORRECTIVE ACTIONS

1. **pm_live_clob.py is already correct** — it picks top genome, places 1 order. ✅
2. **Add survivor queue** — track top-10 genomes, swap on WR drop below 40% over 10 trades
3. **Fix ecosystem messaging** — never say "10K agents trade live." Say "10K strategies compete for 1 live slot."
4. **Update battleship** — reframe R1-R50 around single-genome deployment
5. **Remove parallelism assumptions** — all docs, references, and skills must reflect ONE wallet, ONE order at a time
