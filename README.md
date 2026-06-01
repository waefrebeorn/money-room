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
│              115 fields → room_features.c                  │
└─────────────────────┬───────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────┐
│                  ROOM ENGINE (C)                          │
│  10K agents | 16 rooms | 18 features | 3 regimes         │
│  SGD learning | Darwin evolution | Kelly sizing           │
│  Platform fees (Kraken 0.26%, Coinbase 0.6%+$0.99)       │
│  Slippage | PDT enforcement | $10 min trade              │
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
| **Feed builder** | C | feed_builder.c | 115-field feature vector |
| **Paper trainer** | C | room_engine (PAPER_MODE) | 722K BTC candles, 5ms/cycle |
| **TS Storage** | C | ts_engine.c | 26 bytes/candle binary format |
| **Finnhub** | C | finnhub_collector.c | IPO, economic, filings, quotes |
| **Genome bridge** | C | genome_distiller.c | Paper→live genome transfer |
| **Website** | HTML/CSS/JS | docs/ | Dark trading floor theme |

## Key Metrics

- **Agents**: 10,000 per room (2500 paper mode)
- **Candles**: 722K BTC 1-min + 3.3M exchange data
- **Features**: 115 fields (18 active per agent)
- **FRED Series**: 54 economic series, 160,763 rows
- **Exchange Data**: 7 exchanges, 11 live endpoints
- **Blockchain On-Chain**: 15 BTC metrics, free API
- **Market Micro.**: 18 analysis dimensions
- **Collectors**: 99 C files compiled, 45 crons active
- **Cost**: $0/month (all free APIs)
- **Revenue**: $0 (pre-launch)
- **Language**: C99 (production), Python (dead — no new Python)

## Build & Run

```bash
# Engine
cd engine && make && ./room_engine

# Paper training
make paper && ./room_engine

# Data collectors
./coingecko_collector        # Crypto prices
./finnhub_collector ipo      # IPO calendar
./finnhub_collector economic # Economic calendar

# Storage
./ts_engine info /path/to/file.ts
```

## Battleship

See [`~/.hermes/mind-palace/battleship-ultimate.md`] — 100 active gaps.

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
