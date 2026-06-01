# STATE — Money Room Walkway

## Current Status
- **Engine:** 210 C files, 10K agent paper trading (2500 active), 17 markets
- **Website:** GH Pages at waefrebeorn.github.io/money-room/, data_server port 9090
- **Battleship:** 365 cells across 9 domains (35 🔴 P0, 172 🟡 P1, 158 ⚪ P3)
- **Rooms:** 16 configured. All same binary (same md5). 7 on fake data. Darwin.epoch=0.
- **Data:** 14 JSON feeds serving live, 25+ collectors, timeline.db has 21-33 rows/ticker
- **Latest batch:** DA Triple Research — full system gap audit

## Gap Map Available
- `vault/battleship-ultimate.md` — 365 cells (training, features, risk, data, execution, infra, security, website, monetization)
- `vault/homework-list.md` — 65 human tasks in 3 tiers
- `vault/go-mantra.md` — compact pasteback for loop

## Top 🔴 P0 Killers
1. No SGD weight update loop (A01)
2. Darwin never fires in any room (A02)
3. All 16 rooms identical binary, not differentiated (A03)
4. 7 rooms on fake 0.50 prices (A04)
5. BTC-clone data in economic/macro rooms (A05)
6. N_FEATURES=18 but only ~10 populated (B01)
7. No live exchange API integration (E01-E04)
8. API keys in plaintext, no encryption (G01)
9. No prompt injection guard (G05)
10. No DA guard on wallet ops (G06)
11. LemonSqueezy KYC blocked (I01)
