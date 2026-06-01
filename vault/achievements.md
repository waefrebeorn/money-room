# 🏆 Money Room Achievements

Resolved gaps with file:line proof.

| Cell | Fix | Evidence |
|------|-----|----------|
| T003 | `max_trades_per_cycle` 100→5000. 99.5% deferred fixed. | `room_engine.c:335` |
| T001 | Trade_count safety: hard reset + state validation on restore | `room_engine.c:412-418, 343-351` |
| T002 | CB guard: `consec_room_losses > 0` prevents false fire on init | `room_engine.c:570` |
| T004 | Feed format race: consensus_feed writes "timestamp", engine expected "window_ts". Added fallback parser. Live mode: 3+ cycles/25s, 15K trades, zero feed errors. | `room_feeds.c:242-246` |
| T006 | Misdiagnosis: real historical.db is 3.6GB at pm_logs/historical/, not 0-byte c_room/ symlink. btc_1min: 6.8M rows. candles_multi (live feed source): 11K rows, current. | `room_feed_bridge.c:30,84` |
| T007 | kraken_collector binary runs every 20min via system crontab, writes to pm_logs/historical/historical.db | crontab |
| T008 | Feed bridge writes market_feed.json atomically via rename(), valid 191+ field JSON every 60s | `room_feed_bridge.c:661-666` |
| T060 | Cron struct conflict: 16 room_engine_v3 (old 64MB struct) replaced with unified engine. cycle_all_rooms.sh runs one multi-market engine. | `cycle_all_rooms.sh` |
| T000 | Engine core verified in paper mode: 5,589 cycles, 66,479 trades, $124,685 from $50 seed | Test run |
| T050 | Battleship rebuilt: 100 real gaps, function-level, severity-classified | `battleship-ultimate.md` |
| T051-T052 | Vault/ directory + achievements.md created | `vault/achievements.md` |
|| T011 | Multi-market trainer cron wired: every 4h | Cron `multi-market-trainer` |
|| T009 | Per-market-type cascade features: compute_nested_prediction() now accepts MarketType. Probability-based features for SPORTS/WEATHER/PREDICTION/ELECTION. Per-market ring buffers. | `room_engine.c:69-208` |
|| T010 | Genome load error handling: fopen failure prints WARN; market_type suffix fread return checked; invalid market_type caught with default to CRYPTO. | `room_engine.c:336-356` |
|| T011 | Weather collector cron wired: daily 6:00 AM. Open-Meteo archive API. 728 entries/8 cities/90 days tested. | crontab |
|| T012 | Sports collector cron wired: daily 6:30 AM. ESPN free API. 62 games/3 leagues tested. | crontab |
|| T013 | Multi-market trainer cron wired: daily 7:00 AM (after data refresh). Trains 17 .bin genomes. | crontab |
||| T021 | Timestamp validation: LIVE_MODE feed rejects stale (>24h) and future (>5min) timestamps with WARN + ERR_NO_DATA. | `room_feeds.c:247-266` |
||| T027 | Per-market-type Darwin evolution: cull/clone within same market type. g_agent_market[] map. Same-type pref in repopulation. | `room_darwin.c:94-250` |
|||| T028 | Per-market feature buffers (10×50). Binary price normalized to probability. F1/F2 market-type-aware. RSI/EMA/MACD/Bollinger per-market. | `room_features.c:250-368` |
||||| T030 | Hot-reload genomes: every 1000 cycles scans data/multi_market/*.bin for new mtime, injects trained genome+noise into bottom 10% agents per market type, resets trade stats. Build-tested 5.5K+ cycles clean. | `room_engine.c:230-362` |
28|||||| T025 | Full feature matrix for binary markets: MarketData.double**feats stores all N_FEATURES per row. Sports/weather/prediction loaders use md_add_full(). train_market() reads feats directly instead of OHLCV compression. 550K trades across 17 markets. | `multi_market_trainer.c:41-520,769-789` |
29|||||| T022 | Pipeline health dashboard: C pipeline_monitor.c inspects 20 pipeline components (cron log mtimes, data freshness, engine state), writes `pipeline_status.json`. HTML dashboard at `dashboard.html` with KPI cards + filterable table. Cron every 5 min. | `engine/pipeline_monitor.c`, `docs/dashboard.html` |
|30|||||| T017 | Data quality checks: C data_quality.c validates 22 data sources — row counts, file age, JSON parse, value ranges. Writes `data_quality.json`. Dashboard tab shows pass/warn/fail with issue tags. Cron every 10 min. | `engine/data_quality.c`, `docs/dashboard.html` |
| T031 | **gdelt_sentiment missing binary found in mega-era backup**, copied to money-room/engine/ | `engine/gdelt_sentiment` |
| T032 | **options_feat missing binary found in mega-era backup**, copied to money-room/engine/ | `engine/options_feat` |
| T033 | **economic_collector missing binary found in mega-era backup**, copied to money-room/engine/ | `engine/economic_collector` |
| T034 | **cross_asset_c restored from git history (42048fb)**, recompiled, reads timeline.db | `engine/cross_asset_c.c` |
| T035 | **accuracy_scorer written from scratch** — 75-line C scorer reads outcomes.db | `engine/accuracy_scorer.c` |
| T036 | **room_engine_v3 rebuilt** with all 7 source files (was 89min stale, link errors from missing .c files) | `engine/room_engine_v3` |
| T037 | **frankfurter_collector ELF binary → .sh wrapper** — Hermes cron Script field cannot run ELF directly | `~/.hermes/scripts/frankfurter_wrapper.sh` |
| T038 | **cycle-all-rooms-c duplicate removed** — system crontab already runs C binary every minute | Cron removed |
| T039 | **Dual training pipeline consolidated** — removed 2 wasteful LLM-driven trainers (multi-market-training, multi-market-trainer). Kept daily paper-train (3am) + system crontab (7am). | Cron removed |
| T019 | **historical.db populated** — 734,160 BTC 1-min candles inserted from Coinbase CSV into pm_logs/historical/historical.db candles_multi table. Symlinks fixed (engine/ + c_room/). Data was CSV-only, now in DB for feed_bridge and timeline queries. | `engine/populate_btc_db.c`, symlinks |
| T020 | **Error recovery added**: collector_runner now retries failed collectors 3× with 5s backoff. Timeout, non-zero exit, and missing binary all trigger retry. FATAL logged after all attempts exhausted. | `engine/collector_runner.c:49-119` |
| T024 | **Weather features 3→7 dims**: Added precipitation intensity, wind speed, wind gust ratio, solar radiation. API fetches 6 daily params from Open-Meteo. | `engine/weather_collector.c:103-194` |
| T029 | **Polymarket live collector**: New C binary fetches resolved events from Gamma API. Incremental insert by poly_event_id. Wired into collector_runner SLOW. DB grows hourly. | `engine/polymarket_collector.c`, DB at 505 events |
| T014 | **market_summary path aligned**: Trainer writes summary to data/multi_market/market_summary.json in addition to data/trained_genomes.json. | `engine/multi_market_trainer.c:944-968` |
| T016 | **collector_runner verified**: 36 tasks across 4 categories (FAST/NORMAL/SLOW/SPORTS). All confirmed running. | `engine/collector_runner.c:130-182` |
| T026 | **Genome size math verified**: 348-byte struct + 4-byte market_type = 352 bytes. 87 floats, not "48 floats" as previously claimed. | `engine/types.h:64-82` |
| T032 | **rooms.html created**: 16 room cards showing all strategy rooms with descriptions and stat cards. | `docs/rooms.html` |
| T034 | **Sign In button fixed**: Points to register.html with JS login modal. Both navAuth + navRegister functional. | `docs/index.html:38-40` |
| T036 | **GH Pages auto-deploy**: .github/workflows/deploy.yml auto-deploys docs/ on push. | `.github/workflows/deploy.yml` |
| T037 | **data-sources.html cron count accurate**: Shows "24 C Jobs Running" — actual active count. | `docs/data-sources.html:44` |
| T041 | **research.html populated**: 5 quant-finance papers with abstracts, live SEC filings from JSON data. | `docs/research.html` |
| T043 | **404.html created**: Custom error page matching site theme. | `docs/404.html` |
| T049 | **paper-proof.md Python→C**: "10K-genome Python ecosystem" → "C ecosystem (all C, zero Python)". | `docs/paper-proof.md:61` |
| T050 | **battleship focused**: 166-line active-gap tracker with clean TODO/DONE separation. | battleship-ultimate.md |
| T065 | **CI/CD deploy**: GH Actions auto-deploy for docs/. | `.github/workflows/deploy.yml` |
|31|31|||||| T031 | Paper_feature_bridge: replaces hardcoded aux values (vix=16, sp500=5000) with real historical data from timeline.db. SP500: 2239→7580, VIX: 11→57.8 across 2017-2026. | `engine/paper_feature_bridge.c:1-310`, `room_feeds.c:154-165` |
|32|32|||||| T032 | BTC CSV refresh: btc_csv_refresher fetches Coinbase/Kraken OHLC every 15 min. BTC CSV went from 7 days stale to ~1 min latency. 723K candles. | `engine/btc_csv_refresher.c`, crontab |
|33|33|||||| T033 | Daytime paper trading: paper_live_bridge runs 2500 agents on live feed, outputs stats every 60s to paper_stats.json. Auto-dashboard at paper.html with leaderboard, capital distribution, PnL tracking. | `engine/paper_live_bridge.c`, `docs/paper.html` |
|34|34|||||| T034 | Evolution progress tracker: C binary reads paper training logs + live stats, writes evolution_progress.json every 5 min for website. | `engine/evolution_progress.c`, crontab |
|35|35|||||| T035 | Multi-training loop fixed: 10 epochs + 2500 agent paper cycles (was 5-cycle stubs). 17 markets, proper evolution. | `engine/multi_training_loop.sh` |
||37|37|37|||||| T037 | **Paper training complete**: 204,295 cycles, 87,897 trades, 48.1% WR, $127K capital. 2500 agents. Tail-risk hedging active throughout. Full 722K BTC candle run. | Test run, room_engine_paper |
||38|38|38|||||| T038 | **Paper→live genome sync**: paper_mode_sync distills top 43 agents (88.5%-99.7% WR) into 17 market genomes. Auto-backup to genome_backups/. Hot-reload picks up within 1000 cycles. | `engine/paper_mode_sync.c`, `genome_backups/` |
||39|39|39|||||| T039 | **Battleship stale claims updated**: T045-T048 (c file count, LOC, cron count, collector count) corrected to real numbers: 99 .c, 26,392 LOC, 45 crons. | `battleship-ultimate.md` |
|||40|40|40|40||| T040 | **Paper live bridge fixed**: Duplicate-timestamp bug prevented stats from writing. Now writes warmup stats immediately + periodic stats every 60s even on stale feed. Output path fixed to docs/data/ for website. | `engine/paper_live_bridge.c:259-268` |
|||41|41|41|41||| T070 | **Stale battle claims corrected**: fear_greed (T070), forex (T072), coingecko (T075) verified already scheduled in crontab — battleship marked PORTED. | crontab, `battleship-ultimate.md` |
|||42|42|42|42||| T071 | **finnhub_collector scheduled**: IPO calendar + economic events collector now runs every 6h via system crontab. Tested: 224KB economic_calendar.json + 1.2KB ipo_calendar.json written to data/. | crontab (0 */6 * * *), `engine/finnhub_collector.c` |
|||43|43|43|43||| T053 | **docs/setup.md rewritten**: C-only build guide. Removed all Python references (pm_teachers.py, pip, numpy, ecosystem/), added real C build steps (libcurl/libjansson/libsqlite3-dev, make all/tools/paper, crontab setup, secrets config). | docs/setup.md |
|||44|44|44|44||| T054 | **docs/genome-params.md rewritten**: Matches actual types.h. Fixed 10 params (was 11), 18 features (was 17 — added tail_risk_score), min_edge_pct range [1.0-100.0]. Added P22 multi-regime system. Total genome size table. | docs/genome-params.md, engine/types.h:40-107 |
|||45|45|45|45||| T058 | **Stale battleship claim corrected**: sports_room.c EXISTS (429 lines, compiles). sports_types.h, sports_feature_generator.c, sports_outcomes.c, sports_predictor.c all real. Battleship T058 changed REAL GAP→PARTIAL. | engine/sports_room.c, engine/sports_types.h |
|||46|46|46|46||| T064 | **health_check.c written**: C binary checks 13 collector binaries + 5 data files + engine process. Exits 0/1 for alerting/monitoring. Writes docs/data/health.json. Cron every 5 min. Tested: 20/20 checks healthy. | engine/health_check.c, crontab ( */5 * * * *) |
|||47|47|47|47||| T073 | **exchange_market_collector scheduled**: */30 * * * * system crontab. Public REST API, no key needed. 11/14 exchanges live (Coinbase/Kraken/OKX/Bitfinex/Gate/KuCoin tickers+orderbooks+funding+OI). | engine/exchange_market_collector.c, crontab |
|||48|48|48|48||| T074 | **edgar_collector scheduled**: 0 */12 * * * system crontab. SEC filings for AAPL/MSFT/AMZN/GOOGL/NVDA (55-73 filings each). Free API, no key. | engine/edgar_collector.c, crontab |
|||49|49|49|49||| T077 | **market_microstructure scheduled**: */30 * * * * system crontab. 18-dim analysis: fee comparison, order book imbalance, depth, trade flow, volume profile, VWAP/TWAP, kill zone, delta divergence. Reads from timeline.db. | engine/market_microstructure.c, crontab |
|||50|50|50|50||| T076 | **data_pipeline scheduled**: 0 5 * * * system crontab. Daily aggregator: 2,454 samples × 21 features from timeline.db (SP500/VIX/DGS10/BTC/MCAP). Writes training_data.bin. | engine/data_pipeline.c, crontab |
|||51|51|51|51||| T078 | **timeline_aggregator scheduled**: 0 * * * * system crontab. Rebuilds timeline_hourly (124,799 rows). Processes last 48h each run. | engine/timeline_aggregator.c, crontab |
|||52|52|52|52||| T069 | **blockchain_com_collector scheduled**: 0 */6 * * * system crontab. 15 BTC on-chain charts (tx count, fees, mempool, miner revenue, UTXOs, supply). Free API. | engine/blockchain_com_collector.c, crontab |
|||53|53|53|53||| T061 | **Log rotation configured**: /etc/logrotate.d/money-room installed. Weekly rotation, 4-weeks keep, compress, copytruncate. Systemd timer handles daily trigger. | /etc/logrotate.d/money-room |
|||54|54|54|54||| T060 | **systemd --user service created**: ~/.config/systemd/user/money-room-paper.service. Auto-start, auto-restart (10s), CPU-limit 80%, dedicated log. Old cron-managed engine killed. | ~/.config/systemd/user/money-room-paper.service |
|||55|55|55|55||| T059 | **DA QA pass completed**: qa-pass.md written verifying 3 DA docs + website investigation against current state. DA#3 100% PORTED, others PARTIAL. | docs/da/qa-pass.md |
||||56|56|56|56||| T063 | **Health alerter created**: health_alerter.c wraps health_check exit code. Alert marker + JSON on failure, auto-clears on recovery. History log. Cron every 5 min. | engine/health_alerter.c, crontab */5 * * * * |
||||57|57|57|57||| T038 | **Data server serving docs/data/ on port 9090**: data_server.c serves 8 JSON files (agents, data_quality, evolution_progress, health, paper_stats, pipeline_status, prices, stats) with CORS, no auth. systemd service (money-room-dashboard.service), auto-restart. Clean build, zero warnings. Old dashboard binary replaced at ~/.hermes/scripts/dashboard.bak. | engine/data_server.c, ~/.config/systemd/user/money-room-dashboard.service |
||||58|58|58|58||| T040 | **Rooms dashboard with live data + SVG charts**: rooms.html rewritten with SVG capital distribution bar chart, leaderboard bar chart, 16 room cards with data sources. Fetches live paper engine stats from data_server. Auto-refresh 30s. | docs/rooms.html |
||||59|59|59|59||| T042 | **register.html server-side backend**: data_server.c POST /register handler. Server-side key generation, JSON parsing, writes registrations.json. register.html tries server first, falls back to client-side. CORS preflight handled. Old dead port-8080 fetch removed. | engine/data_server.c:100-190, docs/register.html |
||||60|60|60|60||| T055 | **withdrawal_scheduler compiled**: 627-line C binary for virtual profit withdrawals. SQLite-backed with 6 CLI commands (status, schedule, history, config, withdraw, reset). Reads room_snapshot.json for capital. Wired into Makefile. | engine/withdrawal_scheduler.c, engine/Makefile |
||||61|61|61|61||| T056 | **WALLETS.md verified — real addresses**: BTC (15wLLZx...), ETH (0xc5394b...), SOL (GGMwro...), RTC (RTC17c...), NOWPayments. All match vault.md. Battleship claim was stale. | docs/WALLETS.md |
||||62|62|62|62||| T057 | **CHANGELOG.md + cron job**: docs/CHANGELOG.md auto-generated from git history (457 lines). gen_changelog.sh regenerates every 6h via Hermes cron. Script at ~/.hermes/scripts/. | docs/CHANGELOG.md, scripts/gen_changelog.sh |
||||63|63|63|63||| T066 | **Test suite created**: `make test` — C integration test harness (test_runner.c). 9 tests: health_check JSON, data_server, withdrawal_scheduler CLI, accuracy_scorer, data_quality JSON, engine compile, binary existence. All passing. | engine/test_runner.c, engine/Makefile |
|||||64|64|64|64||| T067 | **Memory leak checking established**: Valgrind installed. 8 critical binaries checked (room_engine, health_check, withdrawal_scheduler, paper_live_bridge, paper_mode_sync, collector_runner, multi_market_trainer, data_server) — 0 bytes definitely lost. Fixed 2 leaks: `json_array()` throwaway leak in exchange_api.c, `curl_slist` unfreed in feed_bridge.c. `make memcheck` target added. | engine/exchange_api.c:138-147, engine/feed_bridge.c:293-332, engine/Makefile:131-139 |
||||||65|65|65|65||| T079 | **C dashboard exists — stale ARCHITECTURE.md claim**: `dashboard.c` was a phantom name. Real implementation is `data_server.c` (22KB ELF, port 9090 static file server, CORS, file listing, POST /register). ARCHITECTURE.md + mega-era-architecture.md updated. | engine/data_server.c, docs/ARCHITECTURE.md, docs/mega-era-architecture.md |
|||||||66|66|66|66||| T080 | **Trade journal writes CSV→JSON every 5 min**: trade_journal.c rewritten (177 lines) to parse real trade_log.csv format (50K trades, 48.5% WR). Outputs docs/data/trade_journal.json with aggregates + recent trades. Cron: */5 * * * *. Wired into Makefile. | engine/trade_journal.c, crontab, engine/Makefile |
|||||||67|67|67|67||| T081 | **Regime detection scheduled every 30 min**: regime_detect.c reads BTC 1-min CSV, outputs RANGE/TREND/VOLATILE to docs/data/regime.json. Cron: */30 * * * *. Current: RANGE (ADX=13.52). | engine/regime_detect.c, crontab |
|||||||68|68|68|68||| T082 | **Risk report live — VaR/CVaR/Sharpe/Sortino**: risk_report.c reads real trade_log.csv, computes 11 risk metrics. Writes docs/data/risk_metrics.json. Cron: :00 and :30 hourly. | engine/risk_report.c, crontab |
|||||||69|69|69|69||| T083 | **Stress test results available**: stress_test runs 3 crash scenarios (2008/2020/2022) against current room state. Output to docs/data/stress_test_result.txt. `make stress_json` target. | engine/stress_test.c, engine/Makefile |
|||||||70|70|70|70||| T084 | **Survival stats analysis wired**: survival_stats reads room_state.bin, computes agent age distribution + generation stats. `make survival_json` target. | engine/survival_stats.c, engine/Makefile |
||||||71|71|71|71||| T107 | **Voting participation fixed**: conviction_threshold lowered 2-4× (0.50→0.20 for traders, 0.55→0.25 for bettors, 0.40→0.20 speculators, 0.60→0.30 hedgers). Tailslayer scaling 3×→2×. Clamp widened [0.08-0.80]. Estimated: 44/2500 (1.76%) → ~400/2500 (16%) voting per cycle. | engine/world_trainer.c:881-920, engine/room_vote.c:34-40 |
\
96|96|96|96|96|96|96|96|96|96||| T103 | **Hot-reload full agent coverage**: bottom 50% get genome replacement + noise, top 50% get noise-only mutation. Changed `mt_count / 10` to `mt_count / 2` with tiered injection. ~100% agents affected per reload. | `engine/room_engine.c:335-382` |
|||||||72|72|72|72||| D001 | **Barnacle hunt — doc sweep across 6 files**: README.md, ARCHITECTURE.md, mega-era-architecture.md: fixed 7+ stale claims (203→208 C files, 45→58 crons, 17→18 dim, 115→18 features, 200→208 files, feed_builder→actual components, battleship-ultimate→battleship-index, dead Python ecosystem refs). Verified 6 more docs clean. | README.md, docs/ARCHITECTURE.md, docs/mega-era-architecture.md, vault/achievements.md |
\
97|97|97|97|97|97|97|97|97|97||| T092 | **Darwin diversity-weighted selection**: clone parents now selected by blend of win_rate (60%) + genome distance from culled agents (40%). Replaces random-from-top-10%. Diversity metrics now actually DRIVE selection, not just computed-as-dead-code. | `engine/room_darwin.c:155-190` |

98|98|98|98|98|98|98|98|98|98||| T097 | **Minimum trade enforcement**: added `MIN_TRADE_STAKE 1.0f` to types.h and `stake < MIN_TRADE_STAKE` guard in room_capital.c. Agents with <$20 capital (at 5% max loss) can no longer place sub-$1 trades. | `engine/types.h:49`, `engine/room_capital.c:62` |
99|99|99|99|99|99|99|99|99|99||| T096 | **PDT enforcement reclassified — stale claim**: Engine is P2P prediction market (YES vs NO matching, resolve next candle). No day trading concept exists — no intraday round-trips to regulate. PDT rules apply to margin brokerage accounts, not binary prediction markets. | `engine/room_capital.c:34-142` (P2P match model) |
100|100|100|100|100|100|100|100|100|100||| T098 | **Key rotation health monitor**: 314-line C binary scans secrets.env, tracks key age in key_rotation.db, flags keys >90 days. Outputs JSON to docs/data/key_health.json. Detects 4 empty placeholders. Cron: daily at 3am. | `engine/key_rotation.c`, `engine/Makefile` |

101|101|101|101|101|101|101|101|101|101||| T088 | **On-chain blended into pump_score**: feed_bridge.c now reads ONCHAIN_FEAT (BTC dominance %) and blends it into pump_score (30% weight). On-chain sentiment reaches engine without genome changes. | `engine/feed_bridge.c:178-192` |
102|
103|102|102|102|102|102|102|102|102|102|102||| CB-CONGRESS | **Congress Trades compiled**: congress_trades.c (363 lines) compiled to binary, 0 warnings. Wrapper existed, collector_runner already wired every 60min. Capitol Trades public API, no key needed. | `engine/congress_trades.c`, `engine/Makefile` |
104|
105|103|103|103|103|103|103|103|103|103|103||| CB-INSIDER | **Insider Trades compiled**: insider_trades.c (338 lines) compiled to binary, 0 warnings. SEC EDGAR Form 4 collector. Wrapper existed, collector_runner wired every 60min. | `engine/insider_trades.c`, `engine/Makefile` |
106|
107|104|104|104|104|104|104|104|104|104|104||| CB-DARKPOOL | **Dark Pool Features compiled**: dark_pool_feat.c (546 lines) compiled to binary, 0 warnings. FINRA/CBOE dark pool data, SQLite-backed. Wrapper existed, collector_runner wired every 60min. | `engine/dark_pool_feat.c`, `engine/Makefile` |
108|
109|105|105|105|105|105|105|105|105|105|105||| CB-ETF-FLOW | **ETF Flow compiled**: etf_flow_feat.c (173 lines) compiled to binary, 0 warnings. 7 BTC ETFs tracked. Wrapper existed, collector_runner wired every 30min. | `engine/etf_flow_feat.c`, `engine/Makefile` |
110|
111|106|106|106|106|106|106|106|106|106|106||| CB-SCREENER | **Stock Screener compiled + wired**: stock_screener.c (322 lines) compiled to binary, 0 warnings. Multi-source composite screener (joins 12 DBs). Wrapper script created, added to collector_runner SLOW (every 60min). | `engine/stock_screener.c`, `~/.hermes/scripts/stock_screener.sh`, `engine/collector_runner.c:172` |
