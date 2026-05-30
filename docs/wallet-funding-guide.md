# Wallet Funding Guide — $50 USDC + $2 MATIC on Polygon

**Wallet:** `0xc5394b12dcb65b2a2aa6fa7a1443c2ba78eb057a`
**Network:** Polygon (chain_id=137)

## Cheapest Path: Coinbase → Polygon ($55)

1. **Buy $53 USDC** on Coinbase (free to buy USDC)
2. **Buy $2 MATIC** on Coinbase (for gas)
3. **Withdraw USDC to Polygon network:**
   - Network: Polygon
   - Address: `0xc5394b12dcb65b2a2aa6fa7a1443c2ba78eb057a`
   - Fee: ~$0 (Polygon withdrawals are ~$0.01)
4. **Withdraw MATIC** same way (~$0.01 fee)
5. **Total cost: ~$55.02**
6. **Confirms in ~2 minutes**

## Alternative: Bridge from Another Chain

If you already have USDC elsewhere:
- Polymarket Bridge: `https://bridge.polymarket.com`
- Supports: Arbitrum, Optimism, Base, Ethereum → Polygon
- Gas: ~$3-8 depending on source chain

## What Happens After Deposit

1. **polygon_monitor.py** (runs every 15m) detects the deposit
2. Sets `live_trading=true` in `~/.hermes/infra/live_clob_state.json`
3. **pm_live_clob.py** (runs every 5m) activates:
   - Reads top genome from `~/.hermes/pm_logs/eco/portfolios.json`
   - Checks current 5-min BTC window on Polymarket
   - Derives CLOB API creds from `POLYMARKET_PK` env var
   - Computes TA signal from Kraken data
   - Places FOK market order at current ask price
   - Kelly sizing: max 20% of wallet per trade
   - Logs to `~/.hermes/pm_logs/live/orders.jsonl`

## Wallet State

Currently: **$0.00 USDC + 0.00 MATIC** — empty.
Need: **$50.00 USDC + $2.00 MATIC**
Auto-monitoring: ✅ Every 15min
Auto-trading: ✅ Ready when funded
