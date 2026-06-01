# HOMEWORK LIST — Money Room Human Tasks

**Generated:** June 1, 2026 (DA Triple Research Audit)
**Source:** 365-cell battleship-ultimate.md gap analysis

---

## TIER 1 🏆 — "5 Minutes at a Keyboard" (Free Signups, No Payment)

| # | Task | Unlocks |
|---|------|---------|
| 1 | Register FRED API key (fred.stlouisfed.org) — free, API key emailed instantly | CB-MARKET: economic indicator time series (D12-D17) |
| 2 | Register NewsAPI free tier (newsapi.org) — 100 req/day free | CB-NEWS: ticker-level news headlines (H24-H25) |
| 3 | Register Finnhub free tier (finnhub.io) — 300 req/day | Stock fundamentals for 50+ tickers (already done but verify quota) |
| 4 | Sign up for CoinGecko API (pro.coingecko.com) — free 10k/mo | On-chain data: MVRV, Puell, SOPR (B13) |
| 5 | Create CoinMarketCap free account (pro.coinmarketcap.com) — 333 calls/day | Alternative pricing data via cmc_collector.sh |
| 6 | Register for TradingView free tier | Charting data, screener features |
| 7 | Sign up for marketstack (marketstack.com) — 1000 calls/mo free | Stock data fallback for yahoo_collector |
| 8 | Register OpenBB free tier | Broader market data access |
| 9 | Sign up for SEC EDGAR email alerts | Insider trading/13F filing notifications |
| 10 | Create GitHub account for PR monitoring (done) | gh_pr_monitor.c already built |
| 11 | Sign up for Glassnode free tier — limited free access | Advanced on-chain metrics |
| 12 | Register Santiment free tier | Social metrics, development activity |
| 13 | Create Messari free account | Crypto research data feeds |
| 14 | Sign up for Intrinio free tier | Alternative financial data |
| 15 | Register for Binance public API (no signup needed) | L/S ratio, funding rate alternative source |
| 16 | Sign up for Hyperliquid public API | Perp market data, funding rates |
| 17 | Register Deribit testnet | Options data, IV term structure |
| 18 | Create Bybit testnet account | Alternative L/S ratio source |
| 19 | Register for Yahoo Finance API (rapidapi) | More reliable stock data |
| 20 | Sign up for Twelve Data free tier (twelvedata.com) | 800 calls/day free, forex + crypto |

---

## TIER 2 🥈 — "15 Minutes at a Desk" (Platform Accounts, Some Setup)

| # | Task | Unlocks |
|---|------|---------|
| 21 | Fund Polymarket wallet with $50 USDC | E04: Polymarket CLOB integration. 4 rooms go from fake to real data |
| 22 | Create Kraken account with KYC | E01: Live crypto trading. API keys with trading permissions |
| 23 | Create Coinbase account with KYC | E02: Live Coinbase trading |
| 24 | Set up testnet Polymarket wallet ($0, test MATIC) | Test Polymarket execution without real $50 |
| 25 | Register for Alpaca Markets (free brokerage API) | Stock trading execution. Fractional shares. |
| 26 | Set up Polygon.io API — free tier | Real-time stock/options data. Chains, trades. |
| 27 | Subscribe to capitoltrades.com free tier (already done) | Congress trading data |
| 28 | Set up SEC EDGAR full-text search API | Better insider trading coverage |
| 29 | Register for BLS (Bureau of Labor Statistics) API | Employment/jobs data |
| 30 | Sign up for EIA (Energy Information Admin) API | Oil/gas inventory data |
| 31 | Register for NOAA weather API | Better weather data for weather room |
| 32 | Create Twitter/X developer account | Social sentiment features (B26) |
| 33 | Set up Reddit API (Pushshift) | Social volume monitoring |
| 34 | Register for FMP (Financial Modeling Prep) free tier | Earnings data, company filings |
| 35 | Sign up for EOD Historical Data free tier | Historical stock data API |
| 36 | Register for IEX Cloud free tier | Stock market data |
| 37 | Create Account for Alpha Vantage (free, 5/min) | Stock/forex/crypto data backup |
| 38 | Set up Discord webhook for alerts | F14: Alert channel beyond Telegram |
| 39 | Create Slack workspace for internal monitoring | Dev alert channel |
| 40 | Set up GitHub Pages custom domain | Professional website URL |
| 41 | Configure Cloudflare free tier for website | HTTPS, caching, DDoS protection |
| 42 | Register for FRED (already) — verify API key works | macro_pipeline.c fuel |
| 43 | Set up Binance Futures testnet | Test execution without risk |
| 44 | Create Deribit demo account | Options trading simulation |
| 45 | Sign up for Chainstack or Infura free tier | Blockchain node access for on-chain data |

---

## TIER 3 🥉 — "15+ Minutes at Desk" (Wallets, Legal, Real Setup)

| # | Task | Unlocks |
|---|------|---------|
| 46 | Complete LemonSqueezy KYC | I01: Payment processing. Revenue possible |
| 47 | Write Privacy Policy page | H22: Legal compliance |
| 48 | Write Terms of Service page | H21: Legal compliance |
| 49 | Add risk disclaimer to all PnL/performance displays | Legal: SEC Marketing Rule compliance |
| 50 | Set up business entity (LLC) | Personal liability shield |
| 51 | Open business bank account | Separate business finances |
| 52 | Set up crypto wallet for business | I: Crypto revenue processing |
| 53 | Configure exchange API IP whitelisting | G03: Security hardening |
| 54 | Enable 2FA on ALL exchange accounts | G28: Account security |
| 55 | Set up exchange withdrawal address allowlist | G29: Fund security |
| 56 | Create sub-accounts on exchanges per strategy | G30: Risk isolation |
| 57 | Write system runbook (recovery procedures) | F30: Operational resilience |
| 58 | Write incident response plan | F31: Security preparedness |
| 59 | Set up encrypted secrets vault (instead of plaintext) | G01: Credential security |
| 60 | Configure automated daily backups to cloud | F09: Data safety |
| 61 | Set up Docker containerization | F01: Portability |
| 62 | Write system architecture slide deck | Investor/partner comms |
| 63 | Create demo video of live system | Marketing |
| 64 | Draft pricing tiers for API product | I02-I03: Monetization |
| 65 | Write onboarding guide for new users | Product polish |

---

## BEFORE/AFTER IMPACT

| Metric | Before | After (all 65 tasks) |
|--------|--------|---------------------|
| Data sources | 15 (mostly BTC-centric) | 50+ (stocks, forex, bonds, crypto, options, news) |
| Training rooms | 16 rooms, all BTC-clone data | 16 rooms, each with real differentiated data |
| Evolution | Darwin.epoch=0 (never fired) | Live evolution tracking per room |
| Security | Plaintext secrets, no 2FA | Encrypted vault, IP whitelist, 2FA |
| Revenue | $0 (pre-revenue) | Payment processing ready, tiers designed |
| Legal | No ToS/Privacy | Full legal stack |
| Live trading | Paper only ($0 real) | Paper + optional exchange execution |
| Data freshness | Unknown (no tracking) | Dashboard per-feed freshness |
