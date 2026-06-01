# Genome Parameter Reference

Each genome in the C engine is defined by **10 parameters**, **18 learned feature weights**, and a **3-regime multi-weight system** (P22). Total: 87 floats = 348 bytes.

## Genome Parameters (10)

Defined in `engine/types.h:64-74`:

| # | Parameter | Range | Description | Mutation Scale |
|---|-----------|-------|-------------|----------------|
| 1 | `position_size` | [0.01, 0.50] | Fraction of capital risked per trade | 0.05 |
| 2 | `conviction_threshold` | [0.05, 0.95] | Minimum conviction to act | 0.08 |
| 3 | `risk_tolerance` | [0.00, 1.00] | Willingness to take risk | 0.10 |
| 4 | `lie_sensitivity` | [0.10, 0.98] | Distrust of crony signals | 0.08 |
| 5 | `herd_antipathy` | [0.00, 1.00] | Contrarian bias | 0.12 |
| 6 | `stop_loss_pct` | [0.01, 0.25] | Max loss before exiting | 0.03 |
| 7 | `take_profit_pct` | [0.01, 0.60] | Profit target | 0.05 |
| 8 | `min_edge_pct` | [1.0, 100.0] | Minimum expected return (bps) | 5.0 |
| 9 | `time_horizon` | [0.10, 10.0] | Minutes before reassessment | 0.80 |
| 10 | `mean_reversion_bias` | [-1.0, 1.0] | -1=trend, +1=reversion | 0.15 |

## Feature Vector (18-dim)

Defined in `engine/types.h:84-107`. Calculated by `room_features.c`.

| # | Feature | Description |
|---|---------|-------------|
| 0 | `price_delta_pct` | Current vs window open % |
| 1 | `micro_momentum` | Last 2 closes delta % |
| 2 | `rsi_7` | 7-period RSI (0–100) |
| 3 | `volume_surge_ratio` | Recent/prior volume ratio |
| 4 | `ema_fast` | 3-period EMA |
| 5 | `ema_slow` | 8-period EMA |
| 6 | `macd_hist` | MACD histogram value |
| 7 | `bollinger_pct` | %B position (0=lower, 1=upper) |
| 8 | `divergence_score` | Price-RSI divergence (-1..1) |
| 9 | `pump_score` | Crony-weighted news sentiment (-1..1) |
| 10 | `regime_indicator` | 0=range, 1=trend, 2=volatile |
| 11 | `fear_greed_norm` | Normalized Fear & Greed (0–1) |
| 12 | `herd_consensus` | % agents voting same direction |
| 13 | `phi_return` | φ-weighted multi-scale return |
| 14 | `phi_vol` | φ-weighted multi-scale volatility |
| 15 | `phi_momentum` | φ-weighted multi-scale momentum |
| 16 | `dft_dominant` | Dominant DFT frequency strength (0-1) |
| 17 | `tail_risk_score` | Tailslayer tail risk score (0-1) |

## Learned Weights

### Base weights (Darwin-evolved)
Feat_weight is an 18-dim array evolved by Darwin on trade outcomes.

```c
signal = dot(feat_weight, features) + bias
conviction = sigmoid(signal)
```

### Per-regime weights (SGD-trained, P22)
Each genome carries 3 separate weight sets — one per market regime:

```c
regime_weight[3][18]   // 0=range, 1=trend, 2=volatile
regime_bias[3]         // one bias per regime
```

SGD trains regime_weight and regime_bias from trade outcomes (`room_capital.c:244`). Voting uses the active regime's weight set:

```c
int regime = get_active_regime();
signal = dot(regime_weight[regime], features) + regime_bias[regime];
```

## Trading Logic

```c
// Vote: agent evaluates features against genome
signal = dot(feat_weight, features) + bias
conviction = sigmoid(signal)

if conviction > conviction_threshold:
    direction = 1 if signal > 0 else 0  // 1=long, 0=short
    position = capital × position_size × (0.5 + conviction × 0.5)
    execute_trade(direction, position)

// Capital: executed by room_capital.c with slippage + fees
// fee = TAKER_FEE (0.1%) + SLIPPAGE (5bps + volume scale)
```

## Darwin Evolution

Every 100 trades per agent:
1. Sort agents by `win_rate_ema` (EMA with α=0.9)
2. Cull bottom 10% — capital pooled to reserve
3. Clone top 10% — capital redistributed equally from reserve
4. Mutate each parameter by ±mut_scale × mutation_rate
5. Mutation rate decays: `max(0.05, 0.3 - epoch × 0.01)`
6. Per-market-type evolution: cull/clone within same market type (`room_darwin.c:94-250`)

## Diversity Metrics

- **weight_diversity**: Standard deviation of ||feat_weight||₂ across population
- **genome_diversity**: Mean pairwise L1 distance in genome parameter space
- Tracked after every Darwin epoch to prevent convergence to monoculture

## Total Genome Size

```
10 params              = 10 floats
1 feat_weight[18]      = 18 floats
1 bias                 =  1 float
1 learning_rate        =  1 float
regime_weight[3][18]   = 54 floats
regime_bias[3]         =  3 floats
                      --------
Total:                 87 floats = 348 bytes
+ 4-byte market_type suffix
= 352 bytes on disk
```
