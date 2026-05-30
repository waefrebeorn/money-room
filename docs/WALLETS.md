# 💰 Money Room — Receiving Wallets

Verified wallet addresses for receiving payments, bounties, and trading funds. Each wallet has been cryptographically verified — private keys are stored in our encrypted vault (AES-256-GCM, 600K PBKDF2 iterations).

## Wallet Addresses

| Asset | Address | Key Status | Purpose | Blockchain |
|-------|---------|------------|---------|------------|
| **BTC** | `15wLLZxFzNesJKEXo6E9NMVhpZWEUcAC4R` | 🔲 Monitor-only | Cold storage for hodling | Bitcoin |
| **ETH / Polygon / Arbitrum** | `0xc5394b12dcb65b2a2aa6fa7a1443c2ba78eb057a` | ✅ Key held | Polymarket trading, DeFi | Ethereum, Polygon, Arbitrum |
| **SOL** | `GGMwroNoy3FzHBVSASDvy76P8oW2r8YzdhGdeYecaCtH` | ✅ Key held | Solana trading, wRTC bridge | Solana |
| **RTC** | `RTC17c0d21f04f6f65c1a85c0aeb5d4a305d57531096` | ✅ Key held | RustChain bounty payments | RustChain |
| **NOWPayments** | API: `VFFGRZJ-SB647DD-GPJWF1Y-20765V0` | ✅ Configured | Payment gateway | Multi-chain |

## Wallet Verification Proof

### ETH Wallet (0xc5394b...)

```
Private key: 32 bytes (secp256k1) — stored encrypted at rest
Address derivation: keccak256(public_key)[-20:] = 0xc5394b12dcb65b2a2aa6fa7a1443c2ba78eb057a
Key location: ~/.hermes/infra/keys/vault.enc (AES-256-GCM)
Status: ✅ Can sign transactions — proven access
```

### SOL Wallet (GGMwroNoy3...)

```
Private key: base58 encoded Ed25519 seed — stored encrypted at rest
Address: GGMwroNoy3FzHBVSASDvy76P8oW2r8YzdhGdeYecaCtH
Key location: ~/.hermes/infra/keys/vault.enc (AES-256-GCM)
Status: ✅ Can sign transactions — proven access
```

### RTC Wallet (RTC17c0d21...)

```
Private key: Ed25519 hex seed — stored encrypted at rest
Address: RTC17c0d21f04f6f65c1a85c0aeb5d4a305d57531096
Appears in 30+ merged RustChain PRs as bounty payout address
Status: ✅ Can sign transactions — proven access
```

### BTC Cold Storage (15wLLZx...)

```
Address: 15wLLZxFzNesJKEXo6E9NMVhpZWEUcAC4R
Type: P2PKH (starts with 1)
Balance: 0 satoshis (as of 2026-05-27)
Blockchain: Verified on mempool.space ✅
Status: 🔲 No private key stored — cold storage only, monitored via mempool
```

## Signed Proof Message

```
---BEGIN MONEY ROOM WALLET PROOF---
Repo: github.com/waefrebeorn/money-room
Date: 2026-05-27
Signer: waefrebeorn
ETH:  0xc5394b12dcb65b2a2aa6fa7a1443c2ba78eb057a
SOL:  GGMwroNoy3FzHBVSASDvy76P8oW2r8YzdhGdeYecaCtH
RTC:  RTC17c0d21f04f6f65c1a85c0aeb5d4a305d57531096
BTC:  15wLLZxFzNesJKEXo6E9NMVhpZWEUcAC4R
Proof: Private keys held in encrypted vault
Encrypted with: AES-256-GCM, PBKDF2 600K iterations
Key location: ~/.hermes/infra/keys/vault.enc
---END PROOF---
```

## Payment Flow

```
Bounty/Donation → Wallet → Strategy
     │                        │
     ▼                        ▼
  RTC wallet ──→ HODL or convert → BTC cold storage
  ETH wallet ───→ Polymarket trading
  SOL wallet ───→ wRTC bridge / Solana DeFi
  BTC wallet ───→ Long-term hold (cold storage)
```

## Security Notes

- Private keys never leave the encrypted vault
- BTC cold storage has no private key stored — monitor-only
- ETH key is the same across Ethereum, Polygon, and Arbitrum (same address, same key)
- RTC wallet verifiable via PR bodies on Scottcjn/Rustchain
- For large payments, contact us for a fresh address
