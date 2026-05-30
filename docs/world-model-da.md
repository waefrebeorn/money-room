# 🔬 DEVIL'S ADVOCATE: World Model Training + Prediction Market Universe

## PART 1: World Model Training DA — Weight Initialization & Architecture

### Finding 1: Uniform random init is a dead start

**Current:** `feat_weight[f] = frand_signed() * 0.05f` for all 80 features. Bias `frand_signed() * 0.1f`.

**Problem:** Uniform [-0.05, 0.05] puts every weight near zero. The initial sigmoid output is ~0.5 for ALL agents — they all vote randomly until SGD pushes weights away from 0. With 80 features × 10K agents × 0.05 range, the first 500 cycles produce noise, not signal.

**Fix:** Xavier/Glorot init: `w ~ U[-sqrt(6/fan_in), sqrt(6/fan_in)]` where fan_in = 80. Range becomes [-0.27, +0.27]. 5.4× wider variance per weight. Agents start with diverse opinions from cycle 1.

```c
// Current (dead start)
g->feat_weight[f] = frand_signed() * 0.05f;  // [-0.05, +0.05]

// Fix (Xavier init for 80-dim)
float scale = sqrtf(6.0f / N_FEATURES);  // = 0.273
g->feat_weight[f] = frand_signed() * scale;  // [-0.27, +0.27]
```

### Finding 2: 50/80 features are synthetic proxies — not real signal

The world trainer generates ~30 meaningful features from its synthetic price history (RSI, BB, MACD, φ-intervals) and 50 synthetic proxies from world state (trend→options, vol→liquidations, etc.). 

**Problem:** The 50 proxies are LINEAR TRANSFORMATIONS of the same 3 world state variables (trend, vol, liquidity). Any SGD update on feature[X] is perfectly correlated with feature[Y]. The 80-dim space is actually ~5-dim. The agent can never learn cross-asset relationships because ALL features derive from the same 3 source variables.

**Fix:** Generate genuinely independent synthetic features:
- Different noise sources per feature group
- Time-decoupled regimes (options regime != crypto regime)
- External shock injection independent of trend/vol

### Finding 3: Curriculum phases reset ALL agents — no memory

Every phase transition re-initializes the agent population from scratch. Agents that learned trend-following in Phase 2 are wiped when Phase 3 starts. They never graduate with knowledge.

**Fix:** 
- Phase transitions should REINFORCE successful agents, not replace them
- New archetypes are ADDED to the pool, existing agents kept
- Agents that perform well across multiple phases graduate to "FULL" status

### Finding 4: No out-of-sample evaluation

The trainer runs on synthetic data only. There's NO holdout set, no cross-validation, no test on real market data. The 46% WR from v3.0 is on synthetic data that the agents trained on.

**Fix:** After training, run the trained feat_weights against 6 months of real timeline.db data. Report OOS WR. Gate graduation on OOS WR > 52%.

### Finding 5: Learning rate is per-genome but never decays

Each agent has a `learning_rate` in [0.001, 0.1] that stays constant throughout training. SGD needs LR decay for convergence.

**Fix:** `lr *= 0.9995` each cycle, min 0.0001. Per-agent LR as starting point.

---

## PART 2: All Prediction Markets With API Access

### Tier 1: Live (API Ready, Can Trade NOW)

| Platform | Type | US-Legal | API Access | Deposit | Categories | Current Status |
|----------|------|:--------:|------------|:-------:|------------|:-------------:|
| **Polymarket** | Crypto (USDC) | ⛔ Geo-blocked | CLOB + Gamma REST/WS | $50 USDC | Crypto 5-min, elections, sports, science, weather | ✅ Collector + CLOB creds + live bridge. NEEDS $50. |
| **Kalshi** | USD (CFTC) | ✅ DCM | REST + WS, RSA auth | $10 USD | Elections, economy, sports, crypto, weather, science, health, climate | ✅ C collector exists. NEEDS paper trader + API key. |

### Tier 2: Research Complete (API Docs Read, Need Implementation)

| Platform | Type | US-Legal | API Access | Deposit | Categories | Priority |
|----------|------|:--------:|------------|:-------:|------------|:--------:|
| **Robinhood** | USD (SEC) | ✅ Broker | REST API (released 2026) | $0 | Event contracts on sports, elections, economics | 🟡 MED |
| **ForecastEx (IBKR)** | USD (CFTC) | ✅ IB sub | REST API | $0 w/ IBKR | Economic indicators, crypto, elections | 🟡 MED |
| **CME Group** | USD (CFTC) | ✅ DCM | CME Globex API | $5K+ | Fed funds futures, economic event futures | 🔴 LOW (capital req) |
| **Limitless** | Crypto | ? | REST API | Crypto | Sports, elections, crypto | 🟢 LOW (new) |

### Tier 3: No Real $$$ (Play/Academic)

| Platform | Settlement | API | Volume | Reason to Skip |
|----------|:----------:|:---:|:------:|----------------|
| **PredictIt** | USD ($850 cap) | Unofficial/scrape | Low | Position cap too small. No official API. |
| **Manifold** | Play money | ✅ REST | N/A | Can't withdraw. No real PnL. |
| **Metaculus** | None | ✅ REST | N/A | Forecasting only, no trading. |
| **Augur** | REPv2 | On-chain | Dead | < $100K volume. Protocol discontinued. |

### Platform Feature Comparison

| Feature | Polymarket | Kalshi | Robinhood | ForecastEx |
|---------|:----------:|:------:|:---------:|:----------:|
| Settlement | USDC (stable) | USD (cash) | USD (cash) | USD (cash) |
| Min Deposit | $50 | $10 | $0 | $0 w/ IBKR |
| Taker Fee | 0.1-0.3% | 0% (most) | 0% | 0% |
| Maker Fee | 0% | 0% | 0% | 0% |
| API Key | EIP-712 derive | RSA keypair | OAuth | API key |
| Rate Limit | 9K/10s | ~10/s | Unknown | Unknown |
| KYC Required | No | Yes | Yes | Yes |
| US Users | VPN required | Legal | Legal | Legal |
| Sports Markets | ✅ | ✅ | ✅ | Coming |
| Crypto Markets | ✅ 5-min | ✅ | No | Yes |
| Election Markets | ✅ | ✅ | ✅ | ✅ |
| Economic Markets | No | ✅ (deep) | ✅ | ✅ (deep) |
| Weather Markets | ✅ | ✅ | Coming | No |
| Science/Health | ✅ | ✅ | No | No |

### Key Insight: Kalshi is the BEST platform for US-legal, USD-resolving trades

Kalshi has:
- Zero fees on most markets
- $10 minimum deposit
- CFTC-regulated (money in segregated bank accounts)
- Deepest economic event contracts (CPI, Fed, NFP, GDP)
- Sports, elections, crypto, weather, science
- Official REST API with WebSocket support
- No crypto needed (USD in/out)

**Gap:** We have the C collector but NO paper trader, NO ecosystem integration, NO Kalshi-specific room.

---

## PART 3: Room Expansion Architecture

### Current Rooms

| Room | Domain | 10K Agents | Data Source | Features | Status |
|------|--------|:----------:|-------------|----------|:------:|
| btc_main | BTC 1-min direction | ✅ | Kraken OHLC | 80-dim OHLCV+makro | ✅ Running |
| macro | SP500 daily | ✅ | FRED SP500+VIX | 13-dim technical | ✅ Paper proof |
| momentum | Momentum signals | ✅ | Timeline.db | 80-dim | ✅ Running |
| polymarket | Paper Polymarket | ✅ | Gamma API | 80-dim | ✅ Running |
| sports | Sports outcomes | ❌ | Kalshi/Odds API | 18-dim sports | 🔴 No data |

### New Rooms Needed

| Room | Platform | Resolution | 10K Required | Data Pipeline | Features |
|------|----------|:----------:|:------------:|---------------|----------|
| **Kalshi Room** | Kalshi API | Event resolution | ✅ | kalshi_collector.c → timeline.db → room_kalshi.c | Event-type indicators, volume, OI, momentum |
| **Crypto 5-min Room** | Polymarket | 5-min windows | ✅ | Gamma slug lookup + Kraken TA → pm_live_clob.py | 7-indicator TA + Polymarket-specific |
| **Sports Room** | Kalshi/Odds API | Game outcome | ✅ | sports_collector.c → timeline.db → room_sports.c | Team stats, spread, moneyline, O/U |
| **Election Room** | Kalshi/Polymarket | Election result | ✅ | Same collectors + election-specific | Polling, betting volume, date proximity |
| **Economic Room** | Kalshi | Data release | ✅ | Same + FRED | Consensus vs market, historical accuracy |

### Architecture: One Binary, Many Domains

```
C engine (room_engine.c) with ROOM_DIR
  ├── ROOM_DIR=/rooms/btc_main    →  10K BTC traders
  ├── ROOM_DIR=/rooms/macro       →  10K SP500 traders  
  ├── ROOM_DIR=/rooms/kalshi      →  10K Kalshi event traders
  ├── ROOM_DIR=/rooms/sports      →  10K sports outcome traders
  ├── ROOM_DIR=/rooms/elections   →  10K election traders
  └── ROOM_DIR=/rooms/economy     →  10K economic event traders

Each room:
  - 10K independent genomes
  - Domain-specific FeatureVector (different fields per domain)
  - Domain-specific data pipeline (C collector → timeline.db → feed JSON)
  - Same Darwin engine (cull/clone/mutate)
  - Same SGD learning (domain-specific features)
  - Survivor queue per room (top-10, one trades live)
```

### Implementation Priority

| Priority | Room | Why | Effort |
|:--------:|------|-----|:------:|
| P0 | **Crypto 5-min** | $50 seed incoming. Ready to trade. | ✅ Done |
| P1 | **Kalshi** | $10 deposit. US-legal. Zero fees. BEST platform. | Large (API key + paper trader + room) |
| P2 | **Sports** | Massive volume. Kalshi has sports. Odds API free. | Medium (collector + features) |
| P2 | **Economic** | Fed/CPI/NFP are highest-value events. Kalshi has deepest contracts. | Medium (features from existing data) |
| P3 | **Elections** | 2026 midterms approaching. High volume Nov 2026. | Medium (poll aggregation + room) |
| P3 | **Robinhood** | Huge user base. New event contracts. API just released. | Large (new platform integration) |

---

## PART 4: Required Code Changes

### world_trainer.c v4.0
1. ✓ No lookahead bias (v3.0 fix)
2. Xavier weight init: `sqrt(6/N_FEATURES) × frand_signed()`
3. Independent synthetic features (de-correlated from world state)
4. Phase transitions KEEP agents, don't reset
5. LR decay per-genome: `lr *= 0.9995`
6. OOS evaluation on real timeline.db data after training
7. Serialize trained feat_weights to JSON for engine import

### Kalshi Room
1. `kalshi_trader.py` — Paper trading module (parallel to pm_money_loop.py)
2. Update `kalshi_collector.c` — Get all market categories, update more frequently
3. `room_kalshi.c` — Domain-specific features (event type, volume trend, OI change)
4. `kalshi_live.py` — Live trading bridge (parallel to pm_live_clob.py)
5. Kalshi API key in vault: `api_keys.kalshi.{key,secret,passphrase}`

### Sports Room
1. `sports_collector.c` — C binary for odds API (the-odds-api.com free tier)
2. `sports_features.c` — Feature computation (team ELO, spread movement, public betting %)
3. `room_sports.c` — 10K sports fans genome
4. Timeline DB integration: `source='sports_nfl_spread'`
