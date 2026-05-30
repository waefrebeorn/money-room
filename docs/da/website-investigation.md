# ⚔️ DEVIL'S ADVOCATE — Website Investigation
> May 30, 2026 — waefrebeorn.github.io/money-room

## Phase 1: CLAIMS

The website at https://waefrebeorn.github.io/money-room/ claims:

| # | Claim | Source |
|---|-------|--------|
| 1 | "Live BTC Market Prediction Engine" with realtime dashboard | Hero section |
| 2 | "10K evolving AI genomes" — Darwin evolution active | Hero + engine section |
| 3 | "80-dim feature engine (F1-F80)" | Engine metadata |
| 4 | "$50 seed capital" harsh capital for agents | Hero |
| 5 | "All C, zero Python" — production is C11 only | Engine section |
| 6 | "43 cron jobs" — collector infrastructure | Verification metadata |
| 7 | "76 C sources" — codebase size | Meta section |
| 8 | "System confidence: 0.875 — 7/8 checks passing" | Verification section |
| 9 | Paper proof: SP500 WR 54.86%, BTC WR 47.5% | Paper proof section |
| 10 | "153,000 active agents across 16 rooms" | Stats section |

---

## Phase 2: VERIFY

| # | Claim | Verified | Evidence |
|---|-------|----------|----------|
| 1 | Live dashboard | 🟡 PARTIAL | HTML loads, but all live values show `--` (JS fetch fails on static GitHub Pages). demo_snapshot.json has data but isn't loaded by the HTML. Dashboard = demo mode, not live. |
| 2 | 10K evolving genomes | 🟡 PARTIAL | MAX_MARKET_AGENTS=10K ✅. BUT engine cycle=1 with 0 Darwin epochs — evolution hasn't run. `darwin.epoch: 0` means genomes are random initialization. No evolution happened. |
| 3 | 80-dim features | 🟡 PARTIAL | `MAX_FEATURES=80` defined ✅. But snapshot shows only 10 features NON-ZERO (price_delta_pct, rsi_7, volume_surge, etc.). The other 70 are 0/default. Real active dimensionality ≈ 10, not 80. |
| 4 | $50 seed | 🔴 FALSE | `room_price_avg: 50.0` ✅ but `capital_current: $481,935` — that's $500K, not $50. The simulation ran with $500K initial capital, not $50. Seed capital mismatch. |
| 5 | All C, zero Python | ✅ TRUE | Zero Python files in whole repo. 179 C files. Claim holds. |
| 6 | 43 cron jobs | 🔴 FALSE | Actual active cron jobs: **24**. Website claims 43. 56% of claimed cron count. |
| 7 | 76 C sources | 🔴 FALSE | Actual C sources: **179**. Website claims 76. That's 57% of actual. Number is stale (was 76 at some earlier point) |
| 8 | System confidence 0.875 | 🟡 PARTIAL | 7/8 tier checks pass ✅. BUT T4 collector health shows **"degraded"** status with 1 fail, 1 timeout, 3 stale collectors. The aggregate "0.875" hides real degradation. |
| 9 | Paper proof | 🟡 PARTIAL | 4/8 criteria passed. SP500 WR 54.86% (0.14% below threshold). BTC WR 47.5% (below 50%). Displayed transparently. Though the 4/8 fails are clearly shown, the "Paper WR" label on the dashboard implies stronger confidence than reality warrants. |
| 10 | 153K agents across 16 rooms | 🟡 PARTIAL | 16 rooms exist ✅. But max agents per room = 10000, and 16 × 10000 = 160K, so 153K is plausible. However with cycle=1 and Darwin.epoch=0, these agents haven't learned or evolved yet. They're random. |

### Key Codebase Verification

```
SEED_CAPITAL: Not grep-able in current engine/ directory
  → Found in types.h? Let me look...
  → money_loop.c had SEED_CAPITAL 50.0 (from earlier session)
  → But snapshot shows $481K capital = $500K seed, not $50 

MAX_AGENTS: 10000 (types.h)
  → Website claims 153K agents active
  → That's 15× MAX_AGENTS from engine code

Feature writes: 42 lines setting feat[] in multi_market_trainer.c
  → 42/80 features populated in the trainer
  → 10/80 shown in snapshot (others are 0.0 or default)
```

---

## Phase 3: RISK ASSESSMENT

| Risk | Severity | Detail |
|------|----------|--------|
| **Seed capital illusion** | 🔴 CRITICAL | Website claims $50 seed but simulation ran $500K. Marketing the system as "harsh $50 training" while actual capital is $500K is misleading. If the website is showing PnL numbers from a $500K simulation, a $50 trader would see very different results. |
| **Dashboard not live** | 🟡 HIGH | The dashboard shows `--` on all data points. Visitors see a broken page. The data IS in demo_snapshot.json but JS doesn't load it from GitHub Pages. |
| **Evolution claims unsubstantiated** | 🟡 HIGH | "Evolving AI genomes" implies learning over time. darwin.epoch=0 means NO evolution has occurred. The website displays "Evolution cycle ongoing" but engines haven't cycled. |
| **Stale numbers** | 🟡 MODERATE | 76 C sources (actual: 179), 43 cron jobs (actual: 24), 80-dim features (actual: ~10 populated). These stale numbers undermine credibility. |
| **Feature dimension exaggeration** | 🟡 MODERATE | "80-dim feature engine" sounds impressive. But 70/80 features are zeros or defaults. This is the synthetic feature illusion: claiming dimensions you don't populate. |
| **Paper proof undermarketed** | 🟢 LOW | 4/8 criteria passed is actually decent for a first run, but the dashboard shows it neutrally. Could emphasize progress rather than failed criteria. |
| **Capital ≠ seed mismatch** | 🟡 MODERATE | `capital_current: $481K` vs claim of $50 seed. Either the simulation used $500K (and should say so) or the PnL numbers are wrong. |

---

## Phase 4: MITIGATIONS

| # | Finding | Fix | Priority | Status |
|---|---------|-----|----------|--------|
| M1 | Dashboard shows `--` on static pages | Wire JS to fetch demo_snapshot.json on page load. Replace `fetch('/api/...')` with `fetch('./demo_snapshot.json')` for GitHub Pages compatibility. | 🔴 P0 | ⏳ |
| M2 | Seed capital mismatch ($500K vs $50) | Update snapshot generator to use actual SEED_CAPITAL=$50. Or fix the demo_snapshot.json to show correct starting capital. | 🔴 P0 | ⏳ |
| M3 | darwin.epoch=0 — no evolution | Ensure c_room engine cycle actually runs. Check if engine cron is functional. Run at least 1 full cycle before snapshot. | 🟡 P1 | ⏳ |
| M4 | Stale numbers: "76 C sources" | Update to actual count (179). Or filter engine/ only .c files and count real production sources. | 🟡 P1 | ⏳ |
| M5 | Stale numbers: "43 cron jobs" | Count actual crons and update. Currently 24. | 🟡 P1 | ⏳ |
| M6 | Feature exaggeration: "80-dim" | Either populate remaining 70 features, or lower claim to "10-dim active, 80-dim architecture" | 🟡 P1 | ⏳ |
| M7 | T4 degraded (1 fail, 1 timeout) | Investigate failing collector. Fix or remove from verification pipeline. | 🟡 P1 | ⏳ |
| M8 | 153K agents unrealistic | Align with MAX_AGENTS (10000). Snapshot claims 15× actual capacity. | 🟡 P1 | ⏳ |
