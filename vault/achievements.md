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
|31|31|||||| T031 | Paper_feature_bridge: replaces hardcoded aux values (vix=16, sp500=5000) with real historical data from timeline.db. SP500: 2239→7580, VIX: 11→57.8 across 2017-2026. | `engine/paper_feature_bridge.c:1-310`, `room_feeds.c:154-165` |
|32|32|||||| T032 | BTC CSV refresh: btc_csv_refresher fetches Coinbase/Kraken OHLC every 15 min. BTC CSV went from 7 days stale to ~1 min latency. 723K candles. | `engine/btc_csv_refresher.c`, crontab |
|33|33|||||| T033 | Daytime paper trading: paper_live_bridge runs 2500 agents on live feed, outputs stats every 60s to paper_stats.json. Auto-dashboard at paper.html with leaderboard, capital distribution, PnL tracking. | `engine/paper_live_bridge.c`, `docs/paper.html` |
|34|34|||||| T034 | Evolution progress tracker: C binary reads paper training logs + live stats, writes evolution_progress.json every 5 min for website. | `engine/evolution_progress.c`, crontab |
|35|35|||||| T035 | Multi-training loop fixed: 10 epochs + 2500 agent paper cycles (was 5-cycle stubs). 17 markets, proper evolution. | `engine/multi_training_loop.sh` |
||37|37|37|||||| T037 | **Paper training complete**: 204,295 cycles, 87,897 trades, 48.1% WR, $127K capital. 2500 agents. Tail-risk hedging active throughout. Full 722K BTC candle run. | Test run, room_engine_paper |
||38|38|38|||||| T038 | **Paper→live genome sync**: paper_mode_sync distills top 43 agents (88.5%-99.7% WR) into 17 market genomes. Auto-backup to genome_backups/. Hot-reload picks up within 1000 cycles. | `engine/paper_mode_sync.c`, `genome_backups/` |
||39|39|39|||||| T039 | **Battleship stale claims updated**: T045-T048 (c file count, LOC, cron count, collector count) corrected to real numbers: 99 .c, 26,392 LOC, 45 crons. | `battleship-ultimate.md` |
||40|40|40|||||| T040 | **Paper live bridge fixed**: Duplicate-timestamp bug prevented stats from writing. Now writes warmup stats immediately + periodic stats every 60s even on stale feed. Output path fixed to docs/data/ for website. | `engine/paper_live_bridge.c:259-268` |
