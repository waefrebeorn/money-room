# BATTLESHIP ULTIMATE — Money Room Active Gap Map

**Generated:** June 1, 2026 (DA Triple Research Audit)
**Count:** 421 cells across 9 domains
**Legend:** 🔴 P0 | 🟡 P1 | 🟢 P2 | ⚪ P3 | ⚫ P4
**Status:** ⏳ Build | ⏳ Stuck | 📋 Planned | ✅ Done

---

## ── DOMAIN A: TRAINING ENGINE (60 cells) ──

| # | Gap | Domain | Pri | Status | Detail |
|---|-----|--------|-----|--------|--------|
|| A01 | No SGD weight update loop in multi_market_trainer | Training | 🔴 | ✅ | Added BCE gradient descent after every trade. feat_weight[i] -= lr * err * (feat[i]-0.5), bias -= lr * err. learning_rate now functional. |
| A02 | Darwin never fires in any room (cycle=1-2) | Training | 🔴 | ⏳ | Darwin triggers every 100 trades. Rooms have 0-2 trades. No evolution has ever happened. |
| A03 | All 16 rooms share identical binary (same md5) | Training | 🔴 | ⏳ | Differentiation is supposed to come from market_feed.json config. Binary is identical. |
| A04 | Rooms 7 (consensus, elections, manifold, etc.) show 0.50 price | Training | 🔴 | ⏳ | Placeholder binary price. No real prediction market data flowing. These rooms are running on fake data. |
| A05 | BTC-clone data fed to economic/macro rooms | Training | 🔴 | ⏳ | "economic" room shows close=7473 (BTC price). Not economic index data. |
| A06 | Room feed generator may not work | Training | 🔴 | ⏳ | FEED_GEN binary exists but per-room feed generation quality unknown. |
| A07 | No per-market-type genome initialization | Training | 🔴 | ⏳ | All rooms use same random genome init. Crypto agents get same weights as sports agents. |
| A08 | No market-specific feature calibration | Training | 🔴 | ⏳ | RSI=50 means different things for 0.50 binary markets vs crypto. No per-market scaling. |
| A09 | No per-asset volatility normalization | Training | 🟡 | ⏳ | BTC at $75K and binary at $0.50 use same feature computation. Price-based features broken. |
| A10 | Multi-market trainer not wired into cron | Training | 🔴 | ⏳ | trainers exist but auto_retrain_c isn't in the cron pipeline. |
| A11 | No walked-forward validation | Training | 🔴 | ⏳ | Training uses full dataset. No train/test split, no walk-forward. Overfit risk is 100%. |
| A12 | No out-of-sample test set | Training | 🔴 | ⏳ | All available data is training data. No holdout period. |
| A13 | No regime transition model | Training | 🟡 | ⏳ | Regime is computed per-tick but no Markov transition matrix to predict next regime. |
| A14 | No position sizing by volatility regime | Training | 🟡 | ⏳ | Volatile regime gets same stake as calm regime. Should reduce 50%. |
| A15 | No per-agent trade journal | Training | 🟡 | ⏳ | Only room-level trades logged. Can't analyze which genome configurations win. |
| A16 | No feature importance feedback loop | Training | 🟡 | ⏳ | FeatureImportance struct exists but never used to prune dead features. |
| A17 | N_FEATURES=18 but no convergence check | Training | 🟡 | ⏳ | No check for features that have flat importance for 1000+ cycles. Should auto-remove. |
| A18 | No learning rate scheduler | Training | 🟡 | ⏳ | learning_rate in Genome is fixed per agent. No decay or cosine annealing. |
| A19 | SGD uses last_trade only, not full batch | Training | 🟡 | ⏳ | gradient step computed from single trade outcome. No mini-batch. High variance. |
| A20 | No gradient clipping | Training | ⚪ | ⏳ | No limit on SGD step size. One outlier trade can destroy learned weights. |
| A21 | No weight decay / L2 regularization | Training | ⚪ | ⏳ | No penalty on large weights. Overfitting likely. |
| A22 | No early stopping | Training | ⚪ | ⏳ | Training runs for fixed epochs. No validation-based convergence. |
| A23 | No dropout / gene silencing | Training | ⚪ | ⏳ | No stochastic feature dropout. Co-adaptation likely. |
| A24 | No transfer learning between market types | Training | 🟡 | ⏳ | What a crypto agent learns can't seed a sports agent. Every market starts from random. |
| A25 | No ensemble prediction across rooms | Training | 🟡 | ⏳ | Each room produces one vote. No weighted ensemble combining crypto + macro + sentiment signals. |
| A26 | No backtest replay harness | Training | 🟡 | ⏳ | Can't replay historical scenarios. Everything runs on live data only. |
| A27 | No permutation feature importance | Training | ⚪ | ⏳ | Can't tell which features actually drive decisions vs are ignored. |
| A28 | No ablation testing | Training | ⚪ | ⏳ | Can't measure what happens if feature X is removed. Everything is always-on. |
| A29 | Single-training-path bottleneck | Training | 🟡 | ⏳ | Only one sequence: feed→feature→vote→resolve. No parallel exploration of strategies. |
| A30 | No exploration vs exploitation epsilon | Training | ⚪ | ⏳ | Agents always vote based on current genome. No random exploration. Early convergence likely. |
| A31 | Room_engine has no MARKET_TYPE selection at runtime | Training | 🔴 | ⏳ | ROOMS are differentiated only by feed data. Engine behavior doesn't change per market type. |
| A32 | No per-room loss function | Training | 🟡 | ⏳ | All rooms optimize same PnL. Crypto needs Sharpe, binary needs calibration, sports needs Brier. |
| A33 | No calibration score for prediction markets | Training | 🟡 | ⏳ | Binary markets need Brier score / calibration curves. PnL alone is insufficient. |
| A34 | No profit factor tracking | Training | ⚪ | ⏳ | TotalWins/TotalLosses ratio not computed anywhere. |
| A35 | No Sortino ratio | Training | ⚪ | ⏳ | Only Sharpe computed. Downside deviation ignored. |
| A36 | No Calmar ratio | Training | ⚪ | ⏳ | Return/maxDrawdown not tracked. |
| A37 | No Kelly criterion position sizing | Training | 🟡 | ⏳ | Position size is genome-evolved, not analytically computed from win rate and edge. |
| A38 | No minimum sample filter | Training | 🟡 | ⏳ | A 70% WR on 10 trades counts same as 70% on 1000 trades. No confidence weighting. |
| A39 | No trade count filter for Darwin ranking | Training | 🟡 | ⏳ | Agent that happened to win 1 coin flip ranks higher than agent with 45% on 500 trades. |
| A40 | No multi-objective evolution | Training | ⚪ | ⏳ | Only PnL optimizes. Sharpe, drawdown, trade frequency not in Darwin fitness. |
| A41 | No cross-validation strategy | Training | ⚪ | ⏳ | All data trained once. No k-fold. |
| A42 | No model checkpointing | Training | ⚪ | ⏳ | If binary crashes mid-training, all progress lost. |
| A43 | No training speed benchmark | Training | ⚪ | ⏳ | No baseline for how fast training should complete. Degradation invisible. |
| A44 | No gradient history for SGD diagnosis | Training | ⚪ | ⏳ | Can't tell if SGD is converging, diverging, or stuck in local minima. |
| A45 | No feature correlation matrix | Training | ⚪ | ⏳ | Two highly-correlated features get double-weight. No PCA/decorrelation. |
| A46 | Room_engine has PAPER_MODE vs LIVE_MODE but no HYBRID | Training | 🟡 | ⏳ | Can't run some rooms live and others paper. All-or-nothing. |
| A47 | No warm-start from prior genomes | Training | 🟡 | ⏳ | Every restart reinitializes all agents from scratch. No genome persistence. |
| A48 | Darwin epoch count always reads 0 in snapshot | Training | 🔴 | ⏳ | Despite cycle=2, Darwin.epoch=0. Evolution has NEVER executed. |
| A49 | room_engine_v2 and v3 binaries exist but unclear if used | Training | 🟡 | ⏳ | Multiple binary versions. Which one does cycle_all_rooms actually run? |
| A50 | No genome diversity metric tracked over time | Training | ⚪ | ⏳ | Can't tell if population is converging to monoculture. |
| A51 | No mutation rate decay schedule | Training | ⚪ | ⏳ | Initial high mutation rate persists forever. Should decay as population matures. |
| A52 | No elite preservation | Training | ⚪ | ⏳ | Best agents can be culled if they happen to lose. No guaranteed survival. |
| A53 | No island model for speciation | Training | ⚪ | ⏳ | One global population. Different strategies compete but can't specialize in niches. |
| A54 | Room engine market configs stored but not validated | Training | 🟡 | ⏳ | room_config.json exists per room but no schema validation. Bad configs run silently. |
| A55 | No A/B test harness for config changes | Training | ⚪ | ⏳ | Every engine change affects all rooms. Can't isolate effect of one parameter change. |
| A56 | No training DB for per-cycle metrics | Training | 🟡 | ⏳ | room_snapshot.json overwrites each cycle. No historical series of metrics. |
| A57 | Cycle count and trade count may not persist | Training | 🟡 | ⏳ | room_state.bin persists but room may reset cycle=0 on process restart. |
| A58 | No heartbeat timeout alert | Training | 🟡 | ⏳ | If cycle_all_rooms hangs, no alert fires until next cron tick. |
| A59 | No multi-threaded room cycling | Training | ⚪ | ⏳ | cycle_all_rooms runs rooms sequentially. 16 rooms × 5s = 80s. Parallel would be 5s. |
| A60 | Room watchdog only restarts, doesn't report | Training | ⚪ | ⏳ | watchdog.sh restarts dead processes but doesn't log or alert about restarts. |

---

## ── DOMAIN B: FEATURES (45 cells) ──

| # | Gap | Domain | Pri | Status | Detail |
|---|-----|--------|-----|--------|--------|
| B01 | N_FEATURES=18 but only ~10 actually populated per snapshot | Features | 🔴 | ⏳ | At least 8 features show zero/default values in real snapshots. |
| B02 | dft_dominant always shows 0.0 | Features | 🟡 | ⏳ | DFT frequency feature appears never computed. Computational gap in feature pipeline. |
| B03 | phi_return/phi_vol/phi_momentum may be uninitialized | Features | 🟡 | ⏳ | φ-interval features defined in struct but computation may not be wired. |
| B04 | tail_risk_score always shows 0.0-0.1 range | Features | 🟡 | ⏳ | Tailslayer feature rarely triggers. Either no tail risk detected or feature broken. |
| B05 | No order book imbalance feature | Features | 🟡 | ⏳ | Order depth data (bids/asks) available from Kraken but not incorporated. |
| B06 | No cumulative volume delta (CVD) | Features | 🟡 | ⏳ | CVD shows aggressive buying/selling but cumulative_volume_delta.c exists as orphan. |
| B07 | No time-weighted average price (TWAP) | Features | ⚪ | ⏳ | TWAP only in execution (twap.c), not used as feature. |
| B08 | No VWAP proximity | Features | ⚪ | ⏳ | Relative position vs VWAP is a known alpha signal. |
| B09 | No realized volatility ratio (short/long vol) | Features | ⚪ | ⏳ | Ratio of 5-min to 1-hour volatility shows regime changes. |
| B10 | No skew / kurtosis features | Features | ⚪ | ⏳ | Higher moments of returns distribution missing. |
| B11 | No seasonal/time-of-day features | Features | 🟡 | ⏳ | Hour-of-day, day-of-week, month-of-year effects unmodeled. |
| B12 | No macro regime feature for equity correlation | Features | 🟡 | ⏳ | BTC correlation to SP500 changes in crisis vs calm regimes. |
| B13 | No on-chain feature beyond BTC dominance | Features | 🟡 | ⏳ | MVRV Z-score, Puell Multiple, SOPR all available from coingecko but not used. |
| B14 | No funding rate feature | Features | 🟡 | ⏳ | Perpetual futures funding rate is a strong short-term signal. Feature exists as standalone binary but not in engine. |
| B15 | No open interest change | Features | 🟡 | ⏳ | OI delta shows new money entering vs exiting. Feature exists as standalone. |
| B16 | No long/short ratio feature | Features | 🟡 | ⏳ | Exchange L/S ratio available. Feature exists as standalone but may not feed engine. |
| B17 | No liquidation cascade feature | Features | 🟡 | ⏳ | Cumulative liquidations signal capitulation events. Feature exists as standalone. |
| B18 | No stablecoin inflow/outflow | Features | 🟡 | ⏳ | Stablecoin flows to exchanges show buying power entering market. |
| B19 | No whale transaction tracking | Features | 🟡 | ⏳ | Large transactions flagged. Feature exists as standalone binary. |
| B20 | No inter-exchange basis | Features | ⚪ | ⏳ | Price difference between exchanges shows arbitrage pressure. |
| B21 | No options-derived features (IV skew, put/call ratio) | Features | 🟡 | ⏳ | Existing options_flow.c computes these but they're not in engine feature vector. |
| B22 | No volatility term structure | Features | ⚪ | ⏳ | Short vs long vol term structure (contango/backwardation) signal. |
| B23 | No VIX regime filter | Features | 🟡 | ⏳ | Market behavior in VIX<15 vs VIX>25 is fundamentally different. |
| B24 | No economic surprise index | Features | ⚪ | ⏳ | Actual vs expected macro data releases. |
| B25 | No news sentiment delta (change over time) | Features | ⚪ | ⏳ | Current sentiment only. Sentiment change (d(sentiment)/dt) is stronger signal. |
| B26 | No social media volume spike | Features | ⚪ | ⏳ | Sudden increase in social mentions precedes volatility. |
| B27 | No feature normalization/scaling | Features | 🟡 | ⏳ | RSI (0-100) and price_delta (-999 to 999) fed to same genome with equal weight. |
| B28 | No feature interaction terms | Features | ⚪ | ⏳ | pump_score * regime_indicator, volume_surge * volatility, etc. |
| B29 | No feature lag transforms | Features | ⚪ | ⏳ | Feature at t-1, t-2, t-3 as separate inputs. Temporally-aware features. |
| B30 | No feature difference transforms (delta) | Features | ⚪ | ⏳ | Feature[i]_t - Feature[i]_{t-1} gives momentum of features themselves. |
| B31 | No rolling z-score normalization | Features | ⚪ | ⏳ | Features should be normalized to z-scores over rolling window. Robust to outliers. |
| B32 | No feature selection process | Features | 🟡 | ⏳ | 18 features is arbitrary. No process for adding/removing features systematically. |
| B33 | No dimension reduction (PCA/UMAP) | Features | ⚪ | ⏳ | High feature space with correlation. Dimensionality reduction would help generalization. |
| B34 | No autoencoder for unsupervised features | Features | ⚪ | ⏳ | Neural feature extraction from raw market data. |
| B35 | No regime-specific feature scaling | Features | 🟡 | ⏳ | Volatile regime features should be scaled differently than calm regime. |
| B36 | No feature timestamp tracking | Features | ⚪ | ⏳ | Engine doesn't track WHEN each feature was last updated. Stale features are invisible. |
| B37 | No feature staleness detection | Features | 🟡 | ⏳ | If GDELT goes down, pump_score stays at 0 without warning. Feature appears valid but is stale. |
| B38 | No feature gradient reset | Features | ⚪ | ⏳ | If market regime changes fundamentally, old feature correlations become misleading. |
| B39 | No continuous feature ID system | Features | 🟡 | ⏳ | Adding a feature requires recompiling types.h and all binaries. No plug-in architecture. |
| B40 | Feature contribution to variance not tracked | Features | ⚪ | ⏳ | PCA variance explained per feature not computed. |
| B41 | No synthetic feature from ensemble predictions | Features | ⚪ | ⏳ | Other rooms' predictions as features for this room. |
| B42 | No attention-weighted feature aggregation | Features | ⚪ | ⏳ | All features equally weighted. Attention would weight salient features higher. |
| B43 | No feature importance drift monitoring | Features | ⚪ | ⏳ | Feature importance changes over time. Importance should be tracked as time series. |
| B44 | Feed bridge may write stale market_feed.json | Features | 🔴 | ⏳ | If bridge fails, previous feed.json is reused. Engine doesn't check feed freshness. |
| B45 | Only 14 JSON feeds in docs/data/ — missing many | Features | 🟡 | ⏳ | Website shows 14 feeds but we collect data for 27+ tickers and 16 rooms. |

---

## ── DOMAIN C: RISK MANAGEMENT (40 cells) ──

| # | Gap | Domain | Pri | Status | Detail |
|---|-----|--------|-----|--------|--------|
| C01 | No VaR computation in engine runtime | Risk | 🔴 | ⏳ | VaR only computed offline in risk_report.c. Engine doesn't self-monitor. |
| C02 | No CVaR/Expected Shortfall | Risk | 🟡 | ⏳ | Expected shortfall captures tail shape. More robust than VaR. |
| C03 | Circuit breaker configured but never triggered | Risk | 🔴 | ⏳ | circuit_breaker_count=0 in all snapshots. Either too lenient or not working. |
| C04 | Max drawdown threshold unknown | Risk | 🟡 | ⏳ | What's the max_drawdown_pct configured? No documented threshold. |
| C05 | No daily loss limit for room capital | Risk | 🟡 | ⏳ | Room can lose all capital in one day. No daily stop. |
| C06 | No max position concentration check | Risk | 🟡 | ⏳ | All agents could bet on same direction. No diversification enforcement. |
| C07 | No correlation-based position limits | Risk | ⚪ | ⏳ | If BTC and ETH are highly correlated, betting on both doesn't diversify. |
| C08 | No black swan scenario testing | Risk | 🟡 | ⏳ | Stress_test.c exists but may only test normal scenarios. |
| C09 | No flash crash simulation | Risk | ⚪ | ⏳ | 2020 style 40% drop in minutes. Room would lose everything before circuit breaker fires. |
| C10 | No exchange outage handling | Risk | 🟡 | ⏳ | If Kraken API goes down, what happens to open positions? |
| C11 | No position liquidation model | Risk | 🟡 | ⏳ | Paper trading doesn't model forced liquidation at margin thresholds. |
| C12 | No slippage shock test | Risk | ⚪ | ⏳ | High-vol slippage can be 50bps. Simulation uses 5bps. 10x discrepancy. |
| C13 | No fee model for different order types | Risk | 🟡 | ⏳ | Taker=0.1%, maker=0%. Engine always charges taker rate. Should model both. |
| C14 | No gas cost model for crypto trades | Risk | ⚪ | ⏳ | On-chain settlement costs $0.50-5 per trade. $50 seed would be decimated by gas. |
| C15 | No Polymarket minimum order enforcement | Risk | 🟡 | ⏳ | Polymarket enforces 5-share minimum. Engine may place smaller orders. |
| C16 | No position size floor check | Risk | 🟡 | ⏳ | MIN_TRADE_STAKE=1 exists but agents could generate smaller size on price*position calc. |
| C17 | No auto-kill on 6 consecutive losses | Risk | 🟡 | ⏳ | Defined in types.h but is it enforced at runtime? Agent with 6 losses should die. |
| C18 | No win-rate-floor auto-kill | Risk | ⚪ | ⏳ | Agent below 30% WR over 100 trades should be auto-culled between Darwin events. |
| C19 | No capital-floor auto-kill | Risk | ⚪ | ⏳ | Agent below $1 capital can't trade. Should be auto-killed. |
| C20 | No max_position_pct_room per agent | Risk | 🟡 | ⏳ | Defined in RoomState but may not be enforced in capital allocation. |
| C21 | No max_total_exposure_pct enforcement | Risk | 🟡 | ⏳ | All agents combined could bet >100% of capital. No leverage limit. |
| C22 | No trade throttle per agent | Risk | ⚪ | ⏳ | One agent could place 100 trades in one cycle. Should be rate-limited. |
| C23 | No duplicate trade detection | Risk | 🟡 | ⏳ | Two rooms could place same trade on same market. Double exposure. |
| C24 | No market correlation across rooms | Risk | 🟡 | ⏳ | Sports room and consensus room both trade binary events. Correlation unknown. |
| C25 | No panic stop for all rooms | Risk | 🟡 | ⏳ | Single kill switch to close all positions and stop trading. |
| C26 | No overnight gap risk model | Risk | ⚪ | ⏳ | Crypto trades 24/7 but positions held overnight face gap risk. |
| C27 | No weekend liquidity model | Risk | ⚪ | ⏳ | Weekend spreads are wider. Engine uses same slippage 7 days/week. |
| C28 | No holiday effect model | Risk | ⚪ | ⏳ | Low volume holidays have different market microstructure. |
| C29 | No fee-aware position sizing | Risk | 🟡 | ⏳ | $1 trade on Kraken costs $0.001 fee (0.1%). But $0.99 minimum. | 
| C30 | No win rate stability filter | Risk | ⚪ | ⏳ | Agent with volatile WR (0.8 then 0.3 then 0.8) is less reliable than steady 0.55. |
| C31 | No t-tested edge | Risk | ⚪ | ⏳ | Is the agent's edge statistically significant? p-value not computed. |
| C32 | No Kelly bet sizing | Risk | 🟡 | ⏳ | Fractional Kelly (half/quarter) adapts position to edge confidence. |
| C33 | No position unwind schedule | Risk | ⚪ | ⏳ | If room needs capital, which positions get closed first? |
| C34 | No stop-loss at room level | Risk | 🟡 | ⏳ | Agents have individual stop-loss but room has no aggregate stop. |
| C35 | No take-profit at room level | Risk | ⚪ | ⏳ | Room keeps trading indefinitely. No "we made 20%, lock in profits" mode. |
| C36 | No correlation between agent positions | Risk | 🟡 | ⏳ | 6 agents all buying same asset same direction = 6x same risk. |
| C37 | No hedge ratio optimization | Risk | ⚪ | ⏳ | Optimal hedge ratio between positions not computed. |
| C38 | No tail-risk overlay strategy | Risk | ⚪ | ⏳ | Put options/tail hedge on top of engine strategy. |
| C39 | No portfolio-level VaR model | Risk | 🟡 | ⏳ | Each room independent. Aggregate portfolio VaR not computed. |
| C40 | No margin adequacy check | Risk | 🟡 | ⏳ | If trading on margin (future), equity check needed before each trade. |

---

## ── DOMAIN D: DATA PIPELINE (55 cells) ──

| # | Gap | Domain | Pri | Status | Detail |
|---|-----|--------|-----|--------|--------|
| D01 | timeline.db only has 21-33 rows per ticker | Data | 🔴 | ⏳ | Less than 1 month of daily data per yahoo_collector ticker. |
| D02 | No backfill capability for historical data | Data | 🔴 | ⏳ | yahoo_collector uses INSERT OR IGNORE. Can't backfill missing history. |
| D03 | Yahoo v7 API limits to ~125 days | Data | 🟡 | ⏳ | Range=5y but API only returns recent. Need v8 or alternative. |
| D04 | No BTC 1-min historical data pipeline | Data | 🟡 | ⏳ | BTC 1-min CSV exists but no active pipeline to keep it updated. |
| D05 | Kraken OHLC API can't backfill historical | Data | 🟡 | ⏳ | Kraken returns max 720 most recent candles. No historical access. |
| D06 | Coinbase has historical but no active collector | Data | 🟡 | ⏳ | Coinbase API supports start/end params but coinbase_live.c may not use them. |
| D07 | No SP500 daily data pipeline | Data | 🟡 | ⏳ | Market_tide.c exists but SP500 data freshness unknown. |
| D08 | No forex historical data | Data | 🟡 | ⏳ | forex_collector.c exists but only gets current rates. No history. |
| D09 | No commodity data pipeline | Data | 🟡 | ⏳ | GC=F, CL=F data via yahoo but no dedicated collector. |
| D10 | No bond yield data pipeline | Data | 🟡 | ⏳ | ^TNX via yahoo but yield changes tracked erratically. |
| D11 | No VIX data pipeline | Data | 🟡 | ⏳ | ^VIX via yahoo but high-resolution VIX data (1-min) missing. |
| D12 | No economic indicator time series | Data | 🟡 | ⏳ | FRED data collected but may not be complete time series. |
| D13 | No GDP data (current or historical) | Data | 🟡 | ⏳ | Not tracked. |
| D14 | No unemployment data | Data | 🟡 | ⏳ | Not tracked. |
| D15 | No CPI/inflation data | Data | 🟡 | ⏳ | Not tracked. |
| D16 | No PMI manufacturing/services | Data | 🟡 | ⏳ | Not tracked. |
| D17 | No retail sales data | Data | ⚪ | ⏳ | Not tracked. |
| D18 | No central bank rate decisions | Data | 🟡 | ⏳ | FOMC dates not tracked. |
| D19 | No earnings calendar data (company-specific) | Data | 🟡 | ⏳ | Yahoo earnings data may be stale or infrequent. |
| D20 | No real-time Polymarket data | Data | 🔴 | ⏳ | Polymarket CLOB needs $50 USDC. Without it, these rooms run on fake data. |
| D21 | No PredictIt data | Data | 🟡 | ⏳ | PredictIt API may not be continuously collected. |
| D22 | No Kalshi data | Data | 🟡 | ⏳ | Kalshi collector exists but API auth may block continuous collection. |
| D23 | No Manifold markets data | Data | 🟡 | ⏳ | Manifold API not integrated. |
| D24 | No Sports betting data (live odds) | Data | 🟡 | ⏳ | Sports collector may get scores but not betting odds. |
| D25 | No weather data (other than current) | Data | ⚪ | ⏳ | Weather predictions need forecasts, not current conditions. |
| D26 | No election data pipeline | Data | 🟡 | ⏳ | 538/FiveThirtyEight poll data not collected. |
| D27 | No sentiment by ticker | Data | 🟡 | ⏳ | GDELT is macro/event-based. Stock-specific sentiment not computed. |
| D28 | No news for non-US markets | Data | ⚪ | ⏳ | GDELT covers English. Non-English financial news is untapped. |
| D29 | No dark pool data for single-stock tickers | Data | 🟡 | ⏳ | dark_pool_feat.c only fetches SPY. Other tickers not tracked. |
| D30 | No SEC filings beyond 13F | Data | 🟡 | ⏳ | 8-K, 10-Q, 10-K not processed. Material event detection missing. |
| D31 | No analyst rating changes | Data | 🟡 | ⏳ | Not tracked. |
| D32 | No insider transaction beyond Form 4 | Data | ⚪ | ⏳ | Form 144 (planned sales) and Section 16 changes not tracked. |
| D33 | No options flow beyond PCR/IV | Data | 🟡 | ⏳ | Real-time options flow flags missing. Only summary stats. |
| D34 | No data freshness dashboard | Data | 🟡 | ⏳ | Each collector runs on cron but no centralized "last successful run" tracking. |
| D35 | No data quality scoring per source | Data | 🟡 | ⏳ | Some sources may return stale/empty data. No quality metric. |
| D36 | No data consistency validation | Data | 🟡 | ⏳ | Cross-source consistency not checked (e.g., Kraken BTC vs Coinbase BTC). |
| D37 | No data gap alerting | Data | 🟡 | ⏳ | If yahoo_collector fails for 3 days, no alert fires. |
| D38 | No anomaly detection on incoming data | Data | 🟡 | ⏳ | Spikes, flatlines, missing ticks in raw data go undetected. |
| D39 | No data staleness flag in engine | Data | 🔴 | ⏳ | Engine uses whatever market_feed.json says without checking its age. |
| D40 | No fallback data source for critical feeds | Data | 🟡 | ⏳ | If Yahoo Finance goes down, BTC data stops. CoinGecko as backup exists but not wired. |
| D41 | CoinGecko collector exists but may not be wired | Data | 🟡 | ⏳ | coingecko_collector.c exists but not in collector_runner. |
| D42 | CBOE data has 15-min delay | Data | 🟡 | ⏳ | Options chain data is delayed. Real-time requires paid OPRA feed. |
| D43 | Finnhub API limited to 300 req/day | Data | 🟡 | ⏳ | stock_collector uses Finnhub free tier. 300 req/day covers ~50 tickers. |
| D44 | No exchange fee table in engine | Data | 🟡 | ⏳ | Fee constants in types.h are hardcoded. No per-exchange fee lookup. |
| D45 | No overnight swap/funding rate data | Data | ⚪ | ⏳ | Futures funding rates not collected. |
| D46 | No order book snapshot archive | Data | ⚪ | ⏳ | Current orderbook_depth.c may get snapshot but no history. |
| D47 | No trade history beyond room_log.csv | Data | 🟡 | ⏳ | CSV format is fragile. No DB-backed trade history. |
| D48 | No human-readable trade journal | Data | 🟡 | ⏳ | Trade journal JSON exists but format may be machine-optimized. |
| D49 | No PnL attribution by market type | Data | 🟡 | ⏳ | Total PnL tracked but not by asset class. |
| D50 | No benchmark comparison | Data | 🟡 | ⏳ | Buy-and-hold BTC benchmark not tracked alongside room PnL. |
| D51 | No risk-free rate for Sharpe | Data | 🟡 | ⏳ | Currently uses 0% as risk-free rate. Should use T-bill rate. |
| D52 | No multi-timeframe data (1m, 5m, 1h, 1d) | Data | 🟡 | ⏳ | All features computed on single timeframe. Multi-scale analysis missing. |
| D53 | No data compression archive | Data | ⚪ | ⏳ | Raw data accumulates unbounded. No archival strategy for old data. |
| D54 | No data retention policy | Data | ⚪ | ⏳ | How long to keep 1-min ticks? 1 year? Forever? No policy. |
| D55 | No privacy-protected data pipeline for user data | Data | ⚪ | ⏳ | If user trading data collected, no anonymization step. |

---

## ── DOMAIN E: EXECUTION (35 cells) ──

| # | Gap | Domain | Pri | Status | Detail |
|---|-----|--------|-----|--------|--------|
| E01 | No live exchange API integration | Execution | 🔴 | ⏳ | All trading is paper. No exchange API calls to execute real trades. |
| E02 | No Kraken REST API integration | Execution | 🔴 | ⏳ | kraken_collector.c reads data only. No trade execution. |
| E03 | No Coinbase integration | Execution | 🔴 | ⏳ | coinbase_live.c reads data only. |
| E04 | No Polymarket CLOB integration | Execution | 🔴 | 🟡 | Blocked on $50 USDC. pm_live_clob.py exists but can't execute. |
| E05 | No order type support (market/limit) | Execution | 🟡 | ⏳ | All paper trades are "market" orders. No limit order model. |
| E06 | No partial fill model | Execution | 🟡 | ⏳ | Paper assumes fills at exact price. Real orders may partially fill. |
| E07 | No order cancellation | Execution | 🟡 | ⏳ | Once an order is placed, can't be canceled. |
| E08 | No order replacement | Execution | ⚪ | ⏳ | Can't improve price on existing order. |
| E09 | No TWAP execution | Execution | ⚪ | ⏳ | Large orders split into smaller tranches. |
| E10 | No iceberg order model | Execution | ⚪ | ⏳ | Hidden orders for large positions. |
| E11 | No execution quality scoring | Execution | ⚪ | ⏳ | No metric for how well orders get filled. |
| E12 | No exchange latency model | Execution | ⚪ | ⏳ | Assumes instant execution. Real orders have 100-500ms latency. |
| E13 | No exchange rate limits | Execution | 🟡 | ⏳ | Rate_limiter.c exists but may not be used in engine loop. |
| E14 | No API key rotation for trading | Execution | 🟡 | ⏳ | Key rotation exists for health check but not for trade execution. |
| E15 | No exchange-specific auth | Execution | 🟡 | ⏳ | Kraken uses API key + secret. Engine has no auth module. |
| E16 | No multi-account trading (sub-accounts) | Execution | ⚪ | ⏳ | Some exchanges allow sub-accounts for strategy isolation. |
| E17 | No multi-wallet support for crypto | Execution | ⚪ | ⏳ | Single wallet. Can't segregate trading capital. |
| E18 | No settlement cycle modeling | Execution | 🟡 | ⏳ | Crypto settles T+0, stocks T+1, options T+1. Not modeled. |
| E19 | No margin trading model | Execution | ⚪ | ⏳ | Leverage trading not modeled. |
| E20 | No futures contract rollover | Execution | ⚪ | ⏳ | Futures expire. Roll costs, contango/backwardation not modeled. |
| E21 | No multi-exchange arbitrage | Execution | ⚪ | ⏳ | Cross-exchange price differences not exploited. |
| E22 | No smart-order-routing | Execution | ⚪ | ⏳ | Best execution across venues not computed. |
| E23 | No trade cost analysis | Execution | 🟡 | ⏳ | Real cost of each trade (fee + slippage + impact) not recorded. |
| E24 | No execution vs signal delay model | Execution | ⚪ | ⏳ | Time from signal generation to order placement not tracked. |
| E25 | No order book simulation | Execution | 🟡 | ⏳ | Paper trades assume top-of-book price. L2 impact not modeled. |
| E26 | No market impact model | Execution | 🟡 | ⏳ | Large orders move price. Impact model exists in SLIPPAGE_VOL_SCALE but uncalibrated. |
| E27 | No price improvement model | Execution | ⚪ | ⏳ | Market orders can get better than NBBO. Not modeled. |
| E28 | No dark pool execution model | Execution | ⚪ | ⏳ | Dark pools offer different execution characteristics. |
| E29 | No time-in-force options (IOC, FOK, GTC) | Execution | ⚪ | ⏳ | Paper assumes GTC. Different TIF have different fill probabilities. |
| E30 | No exchange connection health check | Execution | 🟡 | ⏳ | If exchange API is down, engine still places "trades" without error. |
| E31 | No exchange-specific min order sizes in execution | Execution | 🟡 | ⏳ | Kraken min $10 crypto buy. MIN_TRADE_STAKE=$1 may be too low. |
| E32 | No withdrawal automation | Execution | ⚪ | ⏳ | Withdrawal_scheduler.c exists for paper profits only. |
| E33 | No staking/yield integration | Execution | ⚪ | ⏳ | Idle capital earns no yield. Could stake for 3-5% APR. |
| E34 | No exchange sandbox/testnet | Execution | ⚪ | ⏳ | Kraken/Coinbase offer testnet. Real-money testing is the only test. |
| E35 | No transaction cost analysis dashboard | Execution | ⚪ | ⏳ | Total fees paid / total trade value ratio not displayed anywhere. |

---

## ── DOMAIN F: INFRASTRUCTURE (35 cells) ──

| # | Gap | Domain | Pri | Status | Detail |
|---|-----|--------|-----|--------|--------|
| F01 | No Docker container for engine | Infra | 🟡 | ⏳ | Single-host deployment. No portability. |
| F02 | No CI/CD beyond GitHub Pages | Infra | 🟡 | ⏳ | No automated build test on git push. |
| F03 | No hermetic build environment | Infra | 🟡 | ⏳ | Builds depend on global libraries. Version pinning absent. |
| F04 | No environment variable management | Infra | 🟡 | ⏳ | secrets.h has hardcoded paths. No .env pattern. |
| F05 | No graceful shutdown | Infra | 🟡 | ⏳ | SIGTERM kills engine mid-cycle. No state save on shutdown. |
| F06 | No process health beyond heartbeat | Infra | 🟡 | ⏳ | watchdog.sh only checks process existence. Not responsiveness. |
| F07 | No resource monitoring (CPU/memory/disk) | Infra | 🟡 | ⏳ | If engine starts OOM-killing, no alert. |
| F08 | No disk space monitoring | Infra | 🟡 | ⏳ | timeline.db grows unbounded. Disk full = silent crash. |
| F09 | No database backup strategy | Infra | 🟡 | ⏳ | timeline.db, options DBs, room_state.bin — no backups. |
| F10 | No recovery from corrupt state files | Infra | 🟡 | ⏳ | If room_state.bin is corrupted, engine segfaults. No integrity check. |
| F11 | No state version migration | Infra | 🟡 | ⏳ | When Genome struct changes size, old binfiles become incompatible. No migrator. |
| F12 | No rollback capability | Infra | 🟡 | ⏳ | git revert code but DB state can't be rolled back. |
| F13 | No monitoring dashboard beyond CLI | Infra | 🟡 | ⏳ | Web dashboard shows summary but no real-time engine status. |
| F14 | No alert integration (Telegram/email) | Infra | 🟡 | ⏳ | health_alerter.c exists but alert channel unknown. |
| F15 | No systemd service for engine | Infra | 🟡 | ⏳ | engine runs from crontab. No proper service management. |
| F16 | No log rotation for all logs | Infra | 🟡 | ⏳ | Logrotate config exists but may not cover all log files. |
| F17 | No structured logging (JSON) | Infra | ⚪ | ⏳ | Engine logs are text printf. Machine parsing hard. |
| F18 | No performance benchmark suite | Infra | ⚪ | ⏳ | No baseline for cycle time, memory usage, trade throughput. |
| F19 | No regression test suite for engine | Infra | 🟡 | ⏳ | test_runner.c exists but may not cover engine logic. |
| F20 | No memory leak detection in CI | Infra | 🟡 | ⏳ | make memcheck exists but not in CI pipeline. |
| F21 | No dependency update tracking | Infra | ⚪ | ⏳ | libcurl/libjansson/sqlite3 versions not tracked. |
| F22 | No SSL cert management | Infra | ⚪ | ⏳ | If website adds HTTPS, cert renewal not automated. |
| F23 | No multi-region/DR | Infra | ⚪ | ⏳ | Single WSL host. No disaster recovery. |
| F24 | No data export API | Infra | ⚪ | ⏳ | No REST API for external systems to query engine state. |
| F25 | No read-replica for website data | Infra | ⚪ | ⏳ | Website reads live files. No caching layer. |
| F26 | No gzipped/compressed data serving | Infra | ⚪ | ⏳ | JSON files served uncompressed. Large payloads. |
| F27 | No ETag/cache headers on data files | Infra | ⚪ | ⏳ | GitHub Pages data files have no caching optimization. |
| F28 | No preview/staging deployment | Infra | ⚪ | ⏳ | All changes go directly to production. |
| F29 | No feature flag system | Infra | ⚪ | ⏳ | Can't enable/disable features at runtime. Requires recompile. |
| F30 | No runbook for common failures | Infra | 🟡 | ⏳ | If something breaks, no documented recovery procedure. |
| F31 | No incident response plan | Infra | ⚪ | ⏳ | No defined severity levels, escalation paths. |
| F32 | No post-mortem process | Infra | ⚪ | ⏳ | Failures not formally documented. |
| F33 | No time-series metrics database | Infra | ⚪ | ⏳ | Prometheus/InfluxDB not deployed. No historical metric queries. |
| F34 | No anomaly detection on engine metrics | Infra | ⚪ | ⏳ | If cycle time triples or trade volume drops 90%, no alert. |
| F35 | No automated dependency install | Infra | 🟡 | ⏳ | New dev needs to manually install libcurl, jansson, sqlite3. |

---

## ── DOMAIN G: SECURITY (35 cells) ──

| # | Gap | Domain | Pri | Status | Detail |
|---|-----|--------|-----|--------|--------|
| G01 | API keys stored in ~/.hermes/secrets.env in plaintext | Security | 🔴 | ⏳ | Only protected by file permissions (chmod 600). No encryption. |
| G02 | No API key permission scoping | Security | 🟡 | ⏳ | Keys have full exchange access. No read-only / trading-only separation. |
| G03 | No IP whitelist on exchange keys | Security | 🟡 | ⏳ | Most exchanges support IP whitelisting. Not configured. |
| G04 | No key usage monitoring | Security | 🟡 | ⏳ | No alert if key suddenly used from new IP or higher volume. |
| G05 | No prompt injection guard for external content | Security | 🔴 | ⏳ | GitHub issues/bounties may contain injection payloads. |
| G06 | No DA guard on wallet operations | Security | 🔴 | ⏳ | No "are you sure?" before fund transfers. |
| G07 | No rate limit on API calls | Security | 🟡 | ⏳ | Could trigger exchange rate limits and get banned. |
| G08 | No exchange-connection encryption check | Security | 🟡 | ⏳ | All connections use HTTPS, but no cert pinning. |
| G09 | No local network exposure control | Security | 🟡 | ⏳ | data_server runs on port 9090. No auth. Local network can access. |
| G10 | No CORS policy on data_server | Security | 🟡 | ⏳ | No CORS headers. Browser access from different origin may be blocked. |
| G11 | No input validation on market_feed.json | Security | 🟡 | ⏳ | Engine reads market_feed.json without validation. Corrupt data = undefined behavior. |
| G12 | No limits on genome mutation ranges | Security | ⚪ | ⏳ | Genome mutation bounds exist but no overflow guard. |
| G13 | No code signing | Security | ⚪ | ⏳ | Built binaries not signed. Tampering undetectable. |
| G14 | No integrity check on state files | Security | 🟡 | ⏳ | room_state.bin has magic number but no checksum. |
| G15 | No sandbox for collector binaries | Security | ⚪ | ⏳ | Collectors have full filesystem access. |
| G16 | No seccomp or capability dropping | Security | ⚪ | ⏳ | Binaries run with full Linux capabilities. |
| G17 | No audit log for state changes | Security | ⚪ | ⏳ | Who changed what config when? No audit trail. |
| G18 | No session management for web dashboard | Security | ⚪ | ⏳ | If dashboard adds auth later, need session system. |
| G19 | No CSRF protection | Security | ⚪ | ⏳ | No web forms currently, but prep needed. |
| G20 | No HTTPS for data_server | Security | 🟡 | ⏳ | Port 9090 serves HTTP. No TLS. |
| G21 | No secrets rotation schedule | Security | 🟡 | ⏳ | Key_rotation.c monitors key age but doesn't auto-rotate. |
| G22 | No SSH key management for deploys | Security | 🟡 | ⏳ | GitHub deploy keys exist but no rotation policy. |
| G23 | No fail2ban for repeated API failures | Security | ⚪ | ⏳ | If API key fails auth repeatedly, no ban. |
| G24 | No DDoS protection | Security | ⚪ | ⏳ | Single-host. No WAF/rate-limiting. |
| G25 | No backup encryption | Security | ⚪ | ⏳ | Backups (if they exist) are unencrypted. |
| G26 | No secure deletion of old keys | Security | ⚪ | ⏳ | If key is rotated, old key persists in secrets.env or git. |
| G27 | No git-crypt for secrets in repo | Security | 🟡 | ⏳ | secrets.h may contain sensitive paths. No encryption. |
| G28 | No 2FA for exchange accounts | Security | 🟡 | ⏳ | Exchange accounts should have 2FA enabled. |
| G29 | No withdrawal address allowlisting | Security | 🟡 | ⏳ | Exchanges support address allowlists. Not configured. |
| G30 | No sub-account isolation for trading | Security | ⚪ | ⏳ | All trading from same account. Sub-accounts isolate risk. |
| G31 | No security scan in CI | Security | ⚪ | ⏳ | No automated vulnerability scanning. |
| G32 | No dependency CVE monitoring | Security | ⚪ | ⏳ | libcurl, jansson, sqlite3 CVEs not tracked. |
| G33 | No incident response plan for breach | Security | ⚪ | ⏳ | If breach happens, what to do? No documented plan. |
| G34 | No session timeout for dashboard | Security | ⚪ | ⏳ | Dashboard has no auth currently. Future concern. |
| G35 | No data minimization policy | Security | ⚪ | ⏳ | What data is collected and why? No documented policy. |

---

## ── DOMAIN H: WEBSITE & UI (30 cells) ──

| # | Gap | Domain | Pri | Status | Detail |
|---|-----|--------|-----|--------|--------|
| H01 | No live trading dashboard for rooms | Website | 🟡 | ⏳ | 16 rooms shown as names only. No per-room PnL, WR, cycles. |
| H02 | No per-market breakdown on website | Website | 🟡 | ⏳ | Crypto vs sports vs election PnL not shown. |
| H03 | No trade history explorer | Website | 🟡 | ⏳ | Can't browse individual trades. |
| H04 | No agent browser (top/bottom performers) | Website | 🟡 | ⏳ | Best agents invisible. No leaderboard. |
| H05 | No genome visualizer | Website | ⚪ | ⏳ | Genome weights represented as heatmap. |
| H06 | No feature importance chart | Website | 🟡 | ⏳ | FeatureImportance struct populated but not displayed. |
| H07 | No system confidence score on dashboard | Website | 🟡 | ⏳ | Aggregate "how healthy is the system" indicator. |
| H08 | No data freshness indicators per feed | Website | 🟡 | ⏳ | Each data source shows last-updated time. |
| H09 | No alert history page | Website | ⚪ | ⏳ | Past system alerts visible. |
| H10 | No settings/configuration page | Website | ⚪ | ⏳ | Can't change parameters from web. |
| H11 | No registration/login system | Website | 🟡 | ⏳ | If multi-user, no auth system. |
| H12 | No API key management UI | Website | ⚪ | ⏳ | API key self-service for external users. |
| H13 | No registration page for user accounts | Website | 🟡 | ⏳ | registration.html exists but flows may not work. |
| H14 | No pricing/subscription page | Website | 🟡 | ⏳ | pricing.html exists but no payment integration. |
| H15 | No documentation site | Website | 🟡 | ⏳ | ARCHITECTURE.md is developer-facing. No user docs. |
| H16 | No mobile-responsive design | Website | 🟡 | ⏳ | dashboards may not render well on phone. |
| H17 | No dark mode toggle | Website | ⚪ | ⏳ | Personalization feature. |
| H18 | No live charting (time-series of PnL) | Website | 🟡 | ⏳ | Charts are static snapshots. No interactive time series. |
| H19 | No WebSocket for real-time updates | Website | ⚪ | ⏳ | Page requires refresh. No push updates. |
| H20 | No service worker for offline | Website | ⚪ | ⏳ | No PWA support. |
| H21 | No Terms of Service page | Website | 🟡 | ⏳ | Legal requirement if users access. |
| H22 | No Privacy Policy page | Website | 🟡 | ⏳ | Legal requirement. |
| H23 | No Cookie Consent | Website | ⚪ | ⏳ | EU requirement if cookies used. |
| H24 | No SEO metadata | Website | ⚪ | ⏳ | No meta tags, sitemaps, structured data. |
| H25 | No blog/updates page | Website | ⚪ | ⏳ | Changelog exists but not user-facing. |
| H26 | No "paper proof" results page | Website | 🟡 | ⏳ | Paper trading results should be published. |
| H27 | No comparison vs benchmarks | Website | ⚪ | ⏳ | "We returned X% vs BTC's Y%" comparison chart. |
| H28 | No demo mode that works without live data | Website | 🟡 | ⏳ | Website shows "--" values when data not available. |
| H29 | No JSON API explorer | Website | ⚪ | ⏳ | Endpoints documented with live examples. |
| H30 | No performance metrics (page load, data age) | Website | ⚪ | ⏳ | Website itself not monitored for performance. |

---

## ── DOMAIN I: MONETIZATION (30 cells) ──

| # | Gap | Domain | Pri | Status | Detail |
|---|-----|--------|-----|--------|--------|
| I01 | No payment integration (LemonSqueezy blocked by KYC) | Money | 🔴 | ⏳ | KYC stuck. No revenue possible. |
| I02 | No subscription tiers defined | Money | 🟡 | ⏳ | Free/Pro/Enterprise levels not designed. |
| I03 | No API product for external users | Money | 🟡 | ⏳ | API exists but not packaged for sale. |
| I04 | No data feed product | Money | 🟡 | ⏳ | "Money Room data" as SaaS product. |
| I05 | No signal/alert product | Money | 🟡 | ⏳ | "Buy/Sell signals" as Telegram bot subscription. |
| I06 | No affiliate program | Money | ⚪ | ⏳ | Referral-based growth not set up. |
| I07 | No demo account tier | Money | 🟡 | ⏳ | Free tier should offer limited/lagged data. |
| I08 | No usage-based billing | Money | ⚪ | ⏳ | API call counting + billing not implemented. |
| I09 | No free tier rate limiting | Money | 🟡 | ⏳ | Free users get same access as dev. Need rate limiting. |
| I10 | No user quota tracking | Money | ⚪ | ⏳ | How many API calls per user? No tracking. |
| I11 | No value-add analytics product | Money | 🟡 | ⏳ | C engine's feature importance, tail risk as sellable insights. |
| I12 | No portfolio tracking product | Money | ⚪ | ⏳ | Users connect exchange APIs, get ML portfolio suggestions. |
| I13 | No alerting/notification product | Money | 🟡 | ⏳ | Telegram alert system exists but not packaged for sale. |
| I14 | No white-label option | Money | ⚪ | ⏳ | Enterprise customers get rebranded dashboard. |
| I15 | No revenue dashboard (MRR/ARR) | Money | 🟡 | ⏳ | Not tracking potential revenue. |
| I16 | No churn analysis | Money | ⚪ | ⏳ | No user base yet. But churn model needed before launch. |
| I17 | No A/B pricing test framework | Money | ⚪ | ⏳ | Can't test price points. |
| I18 | No coupon/discount system | Money | ⚪ | ⏳ | Promotional pricing not supported. |
| I19 | No invoice generation | Money | ⚪ | ⏳ | Legal requirement for paid tiers. |
| I20 | No tax computation (VAT, sales tax) | Money | ⚪ | ⏳ | Merchant of Record handles this (LemonSqueezy). |
| I21 | No refund policy page | Money | ⚪ | ⏳ | Legal requirement. |
| I22 | No SLA page | Money | ⚪ | ⏳ | For paid tiers. Uptime guarantees. |
| I23 | No bug bounty program | Money | ⚪ | ⏳ | Security researchers need compensation path. |
| I24 | No referral tracking | Money | ⚪ | ⏳ | User referral links not implemented. |
| I25 | No partner/integration program | Money | ⚪ | ⏳ | Third-party integrations as revenue source. |
| I26 | No sponsored content/product placement | Money | ⚪ | ⏳ | "Powered by Money Room data" endorsements. |
| I27 | No consulting/services offering | Money | 🟡 | ⏳ | Custom trading system builds as service. |
| I28 | No educational content (courses/guides) | Money | ⚪ | ⏳ | Sell access to "how to build a trading engine" course. |
| I29 | No enterprise license model | Money | ⚪ | ⏳ | Per-seat or per-deployment pricing. |
| I30 | No revenue share with data providers | Money | ⚪ | ⏳ | If repackaging data, revenue sharing needed. |

---

## ── SUMMARY ──

| Domain | Cells | 🔴 P0 | 🟡 P1 | 🟢 P2 | ⚪ P3 | ⚫ P4 | 
|--------|-------|-------|-------|-------|-------|-------|
| A: Training Engine | 60 | 14 | 22 | 0 | 24 | 0 |
| B: Features | 45 | 3 | 23 | 0 | 19 | 0 |
| C: Risk Management | 40 | 4 | 21 | 0 | 15 | 0 |
| D: Data Pipeline | 55 | 5 | 38 | 0 | 12 | 0 |
| E: Execution | 35 | 4 | 10 | 0 | 21 | 0 |
| F: Infrastructure | 35 | 0 | 17 | 0 | 18 | 0 |
| G: Security | 35 | 4 | 15 | 0 | 16 | 0 |
| H: Website & UI | 30 | 0 | 16 | 0 | 14 | 0 |
| I: Monetization | 30 | 1 | 10 | 0 | 19 | 0 |
| **TOTAL** | **365** | **35** | **172** | **0** | **158** | **0** |

🔴 P0: 35 critical gaps | 🟡 P1: 172 major gaps | ⚪ P3: 158 minor/feature gaps
