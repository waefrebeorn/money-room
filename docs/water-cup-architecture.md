# 🌊 CASCADE WATER CUP ARCHITECTURE — Complete Market Simulation Ecosystem

> Paper trading is the only thing that matters.
> Simulate every market. Train every room. Prove every edge.
> Water cup flows: each ecosystem fills, overflows into the next.
> The $50 deploys the winner of ALL tournaments, not just one.

## Architecture: Cascade Water Cups

```
                    ╔══════════════════════════════════════╗
                    ║       DATA WATER SOURCE              ║
                    ║  Timeline.db (10M+ rows, 200+ sources)║
                    ╚══════════════════════════════════════╝
                                │
           ┌────────────────────┼────────────────────┐
           ▼                    ▼                    ▼
    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
    │  CUP 1       │    │  CUP 2       │    │  CUP 3       │
    │  Crypto 5-min│    │  Kalshi Event│    │  Sports      │
    │  10K genomes  │    │  10K genomes  │    │  10K genomes  │
    └──────┬───────┘    └──────┬───────┘    └──────┬───────┘
           │                   │                   │
           ▼                   ▼                   ▼
    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
    │  CUP 4       │    │  CUP 5       │    │  CUP 6       │
    │  Economic    │    │  Election    │    │  Weather     │
    │  10K genomes  │    │  10K genomes  │    │  10K genomes  │
    └──────┬───────┘    └──────┬───────┘    └──────┬───────┘
           │                   │                   │
           └──────────┬────────┴────────┬──────────┘
                      ▼                 ▼
               ┌──────────────┐  ┌──────────────┐
               │  CUP 7       │  │  CUP 8       │
               │  Science     │  │  Options     │
               │  10K genomes  │  │  10K genomes  │
               └──────┬───────┘  └──────┬───────┘
                      │                 │
                      └──────┬──────────┘
                             ▼
                    ┌────────────────┐
                    │  FINAL CUP      │
                    │  Macro Ensemble │
                    │  Best of ALL    │
                    └────────────────┘
                             │
                             ▼
                    ┌────────────────┐
                    │  $50 DEPLOY     │
                    │  Winner genome  │
                    └────────────────┘
```

## WATER CUP RULES

1. **Each cup fills independently** — 10K genomes train on its domain data
2. **Cup overflows** — When OOS WR > 52%, cup graduates. Results cascade downstream.
3. **Water flows one direction** — Downstream cups receive upstream predictions as features.
4. **Tandem training** — All cups train simultaneously (parallel 10K ecosystems)
5. **Final ensemble** — The macro cup votes across all upstream predictions
6. **$50 deploys the macro cup's top genome** — The one that learned from ALL markets

## COMPLETE MARKET INDEX (21 Markets, 21 Rooms)

### ⚡ Tier 1: Crypto Markets (Live Now)

| Room | Market | Resolution | Data Source | Current State |
|------|--------|:----------:|-------------|:-------------:|
| CR1 | BTC 5-min direction | 5 min | Kraken OHLC | ✅ pm_fivemin.py, pm_live_clob.py |
| CR2 | ETH 5-min direction | 5 min | Kraken OHLC | ✅ Same pipeline |
| CR3 | SOL 5-min direction | 5 min | Kraken OHLC | ✅ Same pipeline |
| CR4 | XRP 5-min direction | 5 min | Kraken OHLC | ✅ Same pipeline |
| CR5 | BTC 1-min direction | 1 min | Kraken OHLC | ✅ Wubu Market (pm_traderoom.py) |
| CR6 | Crypto general event | Event expiry | Polymarket Gamma | ✅ pm_data_collector.c |

### 🏛️ Tier 2: Kalshi Regulated Markets (Next Priority)

| Room | Market | Resolution | Data Source | Current State |
|------|--------|:----------:|-------------|:-------------:|
| KA1 | Kalshi Sports (NFL) | Game final | Kalshi API | ❌ Need kalshi_trader.py |
| KA2 | Kalshi Sports (NBA) | Game final | Kalshi API | ❌ Need trader |
| KA3 | Kalshi Sports (MLB) | Game final | Kalshi API | ❌ Need trader |
| KA4 | Kalshi Sports (NHL) | Game final | Kalshi API | ❌ Need trader |
| KA5 | Kalshi Elections | Election result | Kalshi API | ❌ Need trader |
| KA6 | Kalshi Economy (CPI) | Data release | Kalshi API | ❌ Need trader |
| KA7 | Kalshi Economy (NFP) | Data release | Kalshi API | ❌ Need trader |
| KA8 | Kalshi Economy (FOMC) | Rate decision | Kalshi API | ❌ Need trader |
| KA9 | Kalshi Crypto | BTC price target | Kalshi API | ❌ Need trader |
| KA10 | Kalshi Weather | Temp/precip | Kalshi API | ❌ Need trader |
| KA11 | Kalshi Science | FDA/clinical | Kalshi API | ❌ Need trader |
| KA12 | Kalshi Climate | Temp records | Kalshi API | ❌ Need trader |

### 🏈 Tier 3: Sports Betting Markets (Odds API)

| Room | Market | Resolution | Data Source | Current State |
|------|--------|:----------:|-------------|:-------------:|
| SP1 | NFL spread | Game final | Odds API | ❌ Need sports_collector.c |
| SP2 | NFL moneyline | Game final | Odds API | ❌ Need |
| SP3 | NFL over/under | Game final | Odds API | ❌ Need |
| SP4 | NBA spread | Game final | Odds API | ❌ Need |
| SP5 | NBA moneyline | Game final | Odds API | ❌ Need |
| SP6 | MLB spread | Game final | Odds API | ❌ Need |
| SP7 | NHL moneyline | Game final | Odds API | ❌ Need |
| SP8 | Soccer (Premier) | Game final | Odds API | ❌ Need |
| SP9 | College football | Game final | Odds API | ❌ Need |
| SP10 | College basketball | Game final | Odds API | ❌ Need |

### 📈 Tier 4: Economic Event Markets

| Room | Market | Resolution | Data Source | Current State |
|------|--------|:----------:|-------------|:-------------:|
| EC1 | CPI print direction | Monthly | FRED + Kalshi | ❌ Need economic_trader.py |
| EC2 | NFP jobs number | Monthly | FRED + Kalshi | ❌ Need |
| EC3 | FOMC rate decision | 8x/year | FRED + Kalshi | ❌ Need |
| EC4 | GDP print | Quarterly | FRED + Kalshi | ❌ Need |
| EC5 | Unemployment rate | Monthly | FRED + Kalshi | ❌ Need |
| EC6 | PPI print | Monthly | FRED + Kalshi | ❌ Need |
| EC7 | Retail sales | Monthly | FRED + Kalshi | ❌ Need |
| EC8 | Consumer sentiment | Monthly | FRED + Kalshi | ❌ Need |

### 🗳️ Tier 5: Election Markets

| Room | Market | Resolution | Data Source | Current State |
|------|--------|:----------:|-------------|:-------------:|
| EL1 | Senate control | Election day | 538 polls | ❌ Need election_trader.py |
| EL2 | House control | Election day | 538 polls | ❌ Need |
| EL3 | Governor races | Election day | 538 polls | ❌ Need |
| EL4 | Presidential 2028 | Election day | 538 polls | ❌ Need (future) |
| EL5 | Ballot initiatives | Election day | State data | ❌ Need |

### 🌤️ Tier 6: Weather & Climate Markets

| Room | Market | Resolution | Data Source | Current State |
|------|--------|:----------:|-------------|:-------------:|
| WE1 | High temp record | Daily | Open-Meteo | ❌ Need weather_trader.py |
| WE2 | Hurricane landfall | Season end | NOAA | ❌ Need |
| WE3 | Rainfall record | Daily | Open-Meteo | ❌ Need |
| WE4 | Climate temp anomaly | Yearly | NOAA | ❌ Need |

### 🔬 Tier 7: Science & Health Markets

| Room | Market | Resolution | Data Source | Current State |
|------|--------|:----------:|-------------|:-------------:|
| SC1 | FDA drug approval | Decision date | FDA | ❌ Need science_trader.py |
| SC2 | Clinical trial result | Trial end | ClinicalTrials.gov | ❌ Need |
| SC3 | NASA/space event | Event date | NASA API | ❌ Need |
| SC4 | Nobel prize winner | Award date | Prediction | ❌ Need |

### ⚙️ Tier 8: Options & Derivatives Markets

| Room | Market | Resolution | Data Source | Current State |
|------|--------|:----------:|-------------|:-------------:|
| OP1 | SPY IV direction | 15 min | CBOE CDN | ✅ options_feat.c |
| OP2 | VIX direction | Daily | CBOE/FRED | ⏳ Partial |
| OP3 | Put/Call ratio | Daily | CBOE | ✅ options_feat.c |

## TRAINER BLUEPRINTS

Every room needs its own trainer module. Pattern:

```python
# Trainer for room X
# 1. Load historical data from timeline.db (source='room_x_*')
# 2. Compute domain-specific features (12-20 features)
# 3. Initialize 10K genomes with Xavier weights
# 4. Run Darwin evolution for N cycles
# 5. Validate OOS on last 20% of data
# 6. Export top genome feat_weight + bias to JSON
# 7. Cascade: output → input features for downstream room
```

### Crypto 5-min Trainer ✅ (pm_fivemin.py)
- Features: 7-indicator TA (window delta, momentum, acceleration, EMA, RSI, volume, tick trend)
- Resolution: 5 min
- Data: Kraken 1-min OHLC
- WR target: >52% OOS

### Kalshi Event Trainer ❌ (kalshi_trainer.py)
- Features: event type (0-7), volume trend, OI change, spread, days to expiry, consensus proximity, historical accuracy
- Resolution: Event-based (variable)
- Data: Kalshi API → timeline.db
- WR target: >55% (binary events have higher predictability)

### Sports Trainer ❌ (sports_trainer.py)
- Features: team ELO, spread movement (24h), public betting %, line consensus, O/U trend, home/away, back-to-back, division game, primetime, weather
- Resolution: Game-based (daily)
- Data: Odds API + historical
- WR target: >53% (spread covers -110 juice)

### Economic Trainer ❌ (economic_trainer.py)
- Features: consensus proximity, prior print, trend (last 3 prints), market implied probability, GDELT sentiment, volume pre-release
- Resolution: Event-based (monthly/quarterly)
- Data: FRED + Kalshi
- WR target: >55% (economic events are mean-reverting)

### Election Trainer ❌ (election_trainer.py)
- Features: polling average, polling trend (last 30d), fundraising, endorsements, incumbency, betting volume trend, date proximity
- Resolution: Election day
- Data: Kalshi + 538 + Polymarket
- WR target: >55%

### Weather Trainer ❌ (weather_trainer.py)
- Features: historical average, ENSO state, seasonal trend, model consensus (GFS vs ECMWF), date proximity
- Resolution: Daily
- Data: Open-Meteo historical
- WR target: >55%

## BUILD ORDER (Parallelizable)

### Wave 1: Data Collection — Build ALL collectors (while trainers run)
- [ ] kalshi_trainer.py
- [ ] sports_collector.c (Odds API)
- [ ] economic_collector.py
- [ ] election_collector.py
- [ ] weather_collector.py (extend Open-Meteo)
- [ ] science_collector.py

### Wave 2: Historical Backfill — Load 5+ years per market
- [ ] Kalshi historical (all categories)
- [ ] Sports historical (MLB 2009+, Kaggle)
- [ ] Economic historical (FRED 20yr)
- [ ] Election historical (2016/2020/2022)
- [ ] Weather historical (Open-Meteo 1940+)

### Wave 3: Train All Rooms — Parallel 10K ecosystems
- [ ] CR1-CR6: Crypto 5-min / 1-min (✅ existing)
- [ ] KA1-KA12: Kalshi all categories
- [ ] SP1-SP10: Sports all types
- [ ] EC1-EC8: Economic all events
- [ ] EL1-EL5: Election all races
- [ ] WE1-WE4: Weather all types
- [ ] SC1-SC4: Science all events
- [ ] OP1-OP3: Options all markets

### Wave 4: Cascade Ensemble — Water cup integration
- [ ] Build cascade features: upstream predictions → downstream features
- [ ] Train macro ensemble: best-of-all upstream predictions
- [ ] Validate on 6-month holdout
- [ ] Select $50 deploy genome

## WATER CUP PRINCIPLES

1. **Fill each cup completely** — Train until WR plateaus (200 cycles < 1% change)
2. **Cascade overflow** — When cup graduates, its prediction becomes a feature for downstream cups
3. **No cup left empty** — ALL 21 rooms must have trained 10K genomes before $50 deploy
4. **Water finds the path** — Darwin selects which upstream signals matter
5. **Tandem flow** — Cups train simultaneously, not sequentially. Data flows both ways during cascade phase.
6. **Only the final ensemble deploys** — Not the best single market, but the best multi-market genome
