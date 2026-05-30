# Genome Parameter Reference

Each genome in the ecosystem is defined by **11 parameters** and **17 learned feature weights**.

## Genome Parameters

| # | Parameter | Range | Description | Mutation Scale |
|---|-----------|-------|-------------|----------------|
| 1 | `position_size` | [0.01, 0.50] | Fraction of capital risked per trade | 0.05 |
| 2 | `conviction_threshold` | [0.01, 0.70] | Minimum conviction to act | 0.08 |
| 3 | `risk_tolerance` | [0.00, 1.00] | Willingness to take risk | 0.10 |
| 4 | `lie_sensitivity` | [0.10, 0.98] | Distrust of crony signals | 0.08 |
| 5 | `herd_antipathy` | [0.00, 1.00] | Contrarian bias | 0.12 |
| 6 | `stop_loss_pct` | [0.01, 0.25] | Max loss before exiting | 0.03 |
| 7 | `take_profit_pct` | [0.01, 0.60] | Profit target | 0.05 |
| 8 | `min_edge_pct` | [0.50, 50.0] | Minimum expected return | 5.0 |
| 9 | `time_horizon` | [0.10, 10.0] | Minutes before reassessment | 0.80 |
| 10 | `mean_reversion_bias` | [-1.0, 1.0] | -1=trend, +1=reversion | 0.15 |

## Learned Feature Weights

Each genome has 17 feature weights + 1 bias + 1 learning rate, trained via REINFORCE:

```
w += lr × (actual - predicted) × feature × importance
```

| Weight | Feature | Learning Signal |
|--------|---------|----------------|
| w[0] | price_delta_pct | Momentum +/ - |
| w[1] | micro_momentum | Short-term trend |
| w[2] | rsi_7 | Overbought/oversold |
| w[3] | volume_surge_ratio | Volume confirmation |
| w[4] | ema_fast | Fast trend |
| w[5] | ema_slow | Slow trend |
| w[6] | macd_hist | Trend momentum |
| w[7] | bollinger_pct | Volatility position |
| w[8] | divergence_score | Hidden divergence |
| w[9] | pump_score | News sentiment |
| w[10] | regime_indicator | Market regime |
| w[11] | fear_greed_norm | Fear/greed |
| w[12] | herd_consensus | Anti-herd |
| w[13] | phi_return | GAAD φ return |
| w[14] | phi_vol | GAAD φ volatility |
| w[15] | phi_momentum | GAAD φ momentum |
| w[16] | dft_dominant | DFT cycle strength |

## Trading Logic

```
signal = dot(feat_weight, features) + bias
conviction = sigmoid(signal)

if conviction > conviction_threshold:
    direction = 1 if signal > 0 else 0
    position = capital × position_size × (0.5 + conviction × 0.5)
    execute_trade(direction, position)
```

## Darwin Evolution

Every 100 trades:
1. Sort by `win_rate_ema` (EMA with α=0.9)
2. Cull bottom 10% — capital pooled
3. Clone top 10% — capital redistributed equally
4. Mutate each parameter by ±mut_scale × mutation_rate
5. Mutation rate decays: `max(0.05, 0.3 - epoch × 0.01)`

## Diversity Metrics

- **weight_diversity**: Standard deviation of ||feat_weight||₂ across population
- **genome_diversity**: Mean pairwise L1 distance in genome parameter space
- Tracked after every Darwin epoch to prevent convergence to monoculture
