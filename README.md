# 🏦 Money Room — Multi-Agent Trading Engine

> **C-first automated trading infrastructure.**
> 10K agents across 16 rooms. Darwin evolution. Real market data. Zero Python in production.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     DATA PIPELINE                       │
├────────────┬────────────┬────────────┬──────────────────┤
│   Crypto   │   Equities │   Macro    │   Alternative    │
│ Binance.vis│ Yahoo Fin  │ FRED (87)  │ Finnhub IPO/Econ │
│ Coinbase   │ SPY/QQQ/.. │ BLS labor  │ Fear & Greed    │
│ Kraken     │ Sector ETFs│ Yield curve│ News RSS/GDELT  │
│ CoinGecko  │ Options    │ ExchangeR. │ StockTwits      │
│ DefiLlama  │ Dividends  │ Sentiment  │ SEC EDGAR       │
└──────┬─────┴──────┬─────┴──────┬─────┴────────┬─────────┘
       │            │           │               │
       ▼            ▼           ▼               ▼
┌─────────────────────────────────────────────────────────┐
│                    FEED BUILDER                          │
│              18 features → room_features.c                  │
└─────────────────────┬───────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────┐
│                  ROOM ENGINE (C)                          │
│  10K agents | 16 rooms | 18 features | 3 regimes         │
│  SGD learning | Darwin evolution | Kelly sizing           │
│  Platform fees (Kraken 0.26%, Coinbase 0.6%+$0.99)       │
│  Slippage | Min $1 trades | 3x retry on errors              │
└─────────┬────────────┬──────────────┬────────────────────┘
          │            │              │
          ▼            ▼              ▼
    ┌────────┐  ┌──────────┐  ┌──────────────┐
    │trade   │  │paper      │  │genome        │
    │journal │  │trainer    │  │distiller     │
    │(CSV)   │  │(722K BTC) │  │(paper→live)  │
    └───┬────┘  └─────┬────┘  └──────┬───────┘
        │            │              │
        │            ▼              │
        │     ┌─────────────────┐   │
        │     │ PAPER FEATURE   │   │
        │     │ BRIDGE          │   │
        │     │ Real SP500/VIX  │   │
        │     │ BTC CSV live    │   │
        │     └───────┬─────────┘   │
        │             │             │
        │             ▼             │
        │     ┌─────────────────┐   │
        │     │ DAYTIME PAPER   │   │
        │     │ TRADING         │   │
        │     │ Live feed stats │   │
        │     │ paper.html      │   │
        │     └─────────────────┘   │
        ▼                           ▼
    ┌─────────────────────────────────────┐
    │         GITHUB PAGES (docs/)         │
    │  paper.html · dashboard.html · data  │
    └─────────────────────────────────────┘
```

## Components

| Component | Lang | Files | Description |
|-----------|------|-------|-------------|
| **Room engine** | C | room_engine.c | 10K agents, SGD, Darwin, voting |
| **Paper trainer** | C | room_engine (PAPER_MODE) | 722K BTC candles, 5ms/cycle |
| **Multi-market** | C | multi_market_trainer.c | 17 market .bin genomes |
| **Data server** | C | data_server.c | HTTP server, port 9090, CORS |
| **Feed bridge** | C | feed_bridge.c | 191-field live feed |
| **Collector runner** | C | collector_runner.c | 36 tasks, 4 categories |
| **Paper live bridge** | C | paper_live_bridge.c | 2500 agents, live feed |
| **Health monitor** | C | health_check+alerter | 5-min cycle, auto-alert |
| **Website** | HTML/CSS/JS | docs/ | Dark trading floor theme |

## Key Metrics

- **Agents**: 10,000 per room (2500 paper mode)
- **Candles**: 722K BTC 1-min + 17 market .bin genomes
- **Features**: 18 per agent (3 regimes × 18 weights)
- **Multi-markets**: 17 trained across crypto, stocks, macro, sports, weather, prediction
- **Exchange Data**: 7 exchanges, 11 live endpoints
- **Blockchain On-Chain**: 15 BTC metrics, free API
- **Market Micro.**: 18 analysis dimensions
- **Collectors**: 209 C files compiled, 35 crons active
- **Cost**: $0/month (all free APIs)
- **Revenue**: $0 (pre-launch)
- **Language**: C11 (production), zero Python

## Build & Run

```bash
# Engine
cd engine && make && ./room_engine

# Paper training
make paper && ./room_engine_paper

# Data collectors
./coingecko_collector        # Crypto prices
./finnhub_collector ipo      # IPO calendar
./finnhub_collector economic # Economic calendar
./yahoo_collector --range 5y # Yahoo Finance

# Health & monitoring
./health_check
./health_alerter

# Risk & analysis
./risk_report
./regime_detect /home/wubu2/.hermes/pm_logs/historical/btc_1min_latest.csv --window 20 --smooth 14
./stress_test /home/wubu2/.hermes/pm_logs/c_room/room_state.bin

# Website data server
./data_server
```

## Battleship

See [`docs/battleship-index.md`](docs/battleship-index.md) — Unusual Whales comparison + 15 free-data clone pipelines.

## Internal Gap Tracker

All T-number gaps (T001–T107+) are resolved and recorded in [`vault/achievements.md`](vault/achievements.md).

## Data Sources (Free)

| Type | Sources | Key Required |
|------|---------|-------------|
| Crypto | Binance, Coinbase, Kraken, CoinGecko, CoinMarketCap | ❌ |
| Stocks | Yahoo Finance, Alpha Vantage, EODHD, StockData | ❌ |
| Macro | FRED (87 series), BLS, Finnhub | ❌ |
| Sentiment | Finnhub filings, Google News RSS, StockTwits | ❌ |
| Forex | Frankfurter, ExchangeRate-API, ForexRateAPI | ❌ |
| On-chain | Blockchain.com, DefiLlama, DIA | ❌ |
| SEC | SEC EDGAR (10-K, Form 4, 13F) | ❌ |
| Weather | Open-Meteo | ❌ |
