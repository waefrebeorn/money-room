#!/usr/bin/env python3
"""PAYOUT LEDGER (R2) — Tracks RustChain bounty PRs vs RTC owed.

Generated: 2026-05-28
RTC Wallet: RTC17c0d21f04f6f65c1a85c0aeb5d4a305d57531096
Status: 30 PRs MERGED May 27-28 — awaiting paymaster processing
"""

BOUNTY_RATES = {
    "input_cap": 1,     # ~1 RTC per input cap PR
    "rate_limit": 2,    # ~2 RTC per rate limit
    "test": 3,          # ~3 RTC per test coverage
    "stub": 2,          # ~2 RTC per stub replacement
    "error": 2,         # ~2 RTC per error handling
    "race": 3,          # ~3 RTC per race fix
    "security": 5,      # ~5 RTC for security validator
    "blog": 5,          # 5 RTC blog post bounty
}

LEDGER = {
    # === WAVE 1: Input Caps (Row A) — ALL MERGED May 27-28 ===
    "A19":  {"pr": 6456, "type": "input_cap", "file": "beacon_api.py", "status": "merged", "rtc": 1},
    "A25":  {"pr": 6448, "type": "input_cap", "file": "faucet.py", "status": "merged", "rtc": 1},
    "A26":  {"pr": 6449, "type": "input_cap", "file": "sophia_api.py", "status": "merged", "rtc": 1},
    "A27":  {"pr": 6450, "type": "input_cap", "file": "explorer/app.py", "status": "merged", "rtc": 1},
    "A28":  {"pr": 6451, "type": "input_cap", "file": "p2p_sync.py", "status": "merged", "rtc": 1},
    "A29":  {"pr": 6452, "type": "input_cap", "file": "explorer/dashboard.py", "status": "merged", "rtc": 1},
    "A30":  {"pr": 6446, "type": "input_cap", "file": "testnet_faucet.py", "status": "merged", "rtc": 1},
    "A31":  {"pr": 6447, "type": "input_cap", "file": "rent_a_relic/server.py", "status": "merged", "rtc": 1},
    "A34":  {"pr": 6442, "type": "input_cap", "file": "health-dashboard/server.py", "status": "merged", "rtc": 1},
    "A35":  {"pr": 6457, "type": "input_cap", "file": "beacon_x402.py", "status": "merged", "rtc": 1},
    "A36":  {"pr": 6444, "type": "input_cap", "file": "bottube_embed.py", "status": "merged", "rtc": 1},
    "A37":  {"pr": 6443, "type": "input_cap", "file": "rustchain-core/rpc.py", "status": "merged", "rtc": 1},
    "A38":  {"pr": 6441, "type": "input_cap", "file": "contributor_registry.py", "status": "merged", "rtc": 1},
    "A39":  {"pr": 6440, "type": "input_cap", "file": "hall_of_rust.py", "status": "merged", "rtc": 1},
    "A32-33":{"pr": 6445, "type": "input_cap", "file": "explorer-api/", "status": "merged", "rtc": 1},
    "A40":  {"pr": 6460, "type": "input_cap", "file": "machine_passport_api", "status": "merged", "rtc": 1},

    # === WAVE 2: Error Handling (Row M) — ALL MERGED ===
    "M1":   {"pr": 6351, "type": "error", "file": "bridge_api sqlite timeout", "status": "merged", "rtc": 2},
    "M2":   {"pr": 6352, "type": "error", "file": "beacon JSON validation", "status": "merged", "rtc": 2},
    "M4":   {"pr": 6353, "type": "error", "file": "governance proposal fee", "status": "merged", "rtc": 2},
    "M5":   {"pr": 6354, "type": "error", "file": "coalition quorum", "status": "merged", "rtc": 2},
    "M6":   {"pr": 6355, "type": "error", "file": "payout_worker archive", "status": "merged", "rtc": 2},
    "M7":   {"pr": 6356, "type": "error", "file": "feed_routes validation", "status": "merged", "rtc": 2},
    "M8":   {"pr": 6357, "type": "error", "file": "epoch_settler logging", "status": "merged", "rtc": 2},

    # === WAVE 3: Rate Limiting (Row S18-S20) — STILL OPEN ===
    "S18":  {"pr": 6292, "type": "rate_limit", "file": "bridge_api", "status": "open", "rtc": 2},
    "S19":  {"pr": 6293, "type": "rate_limit", "file": "beacon_api", "status": "open", "rtc": 2},
    "S20":  {"pr": 6294, "type": "rate_limit", "file": "airdrop_v2", "status": "open", "rtc": 2},

    # === WAVE 4: Stubs (Row S, F) — MOSTLY MERGED ===
    "S1":   {"pr": 6286, "type": "stub", "file": "Ed25519 signing", "status": "merged", "rtc": 2},
    "S3":   {"pr": 6459, "type": "stub", "file": "beacon chat LLM", "status": "open", "rtc": 2},
    "S6":   {"pr": 6289, "type": "stub", "file": "scorer HTTP client", "status": "merged", "rtc": 2},
    "S13":  {"pr": 6290, "type": "stub", "file": "payout_worker MOCK_MODE", "status": "merged", "rtc": 2},
    "S17":  {"pr": 6291, "type": "stub", "file": "ed25519 mock sig env-var", "status": "merged", "rtc": 2},
    "F1-F2":{"pr": 6312, "type": "stub", "file": "mcp-mock stdio", "status": "open", "rtc": 2},
    "F8":   {"pr": 6314, "type": "stub", "file": "bios_pawpaw except", "status": "open", "rtc": 2},
    "F32":  {"pr": 6315, "type": "stub", "file": "solana-sdk env var", "status": "open", "rtc": 2},
    "F22":  {"pr": 6363, "type": "stub", "file": "empty except logging", "status": "merged", "rtc": 2},
    "F23":  {"pr": 6362, "type": "stub", "file": "mcp_integration except", "status": "merged", "rtc": 2},
    "F6-F7":{"pr": 6367, "type": "stub", "file": "telegram_bot except", "status": "merged", "rtc": 2},

    # === WAVE 5: Test Coverage (Row T) — MOSTLY MERGED ===
    "T1":   {"pr": 6316, "type": "test", "file": "auto_epoch_settler", "status": "open", "rtc": 3},
    "T2":   {"pr": 6317, "type": "test", "file": "bcos_pdf", "status": "open", "rtc": 3},
    "T3":   {"pr": 6366, "type": "test", "file": "beacon_anchor", "status": "merged", "rtc": 3},
    "T8":   {"pr": 6361, "type": "test", "file": "bottube_feed", "status": "merged", "rtc": 3},
    "T9":   {"pr": 6358, "type": "test", "file": "bottube_feed_routes", "status": "merged", "rtc": 3},
    "T10":  {"pr": 6365, "type": "test", "file": "bridge_api", "status": "merged", "rtc": 3},
    "T11":  {"pr": 6359, "type": "test", "file": "claims_eligibility", "status": "merged", "rtc": 3},
    "T12":  {"pr": 6364, "type": "test", "file": "claims_settlement", "status": "merged", "rtc": 3},

    # === WAVE 6: Race Conditions (Row B) ===
    "B4-B5":{"pr": 6439, "type": "race", "file": "lock_ledger atomic", "status": "merged", "rtc": 3},

    # === WAVE 7: Security ===
    "SEC":  {"pr": 6360, "type": "security", "file": "vintage hardware validator", "status": "merged", "rtc": 5},

    # === WAVE 8: Other misc ===
    "T3b":  {"pr": 6320, "type": "test", "file": "beacon_anchor v2", "status": "merged", "rtc": 3},
}


def compute_totals():
    total_rtc = 0
    total_merged_rtc = 0
    total_open_rtc = 0
    merged_count = 0
    open_count = 0

    for cell, entry in sorted(LEDGER.items()):
        total_rtc += entry["rtc"]
        if entry["status"] == "merged":
            total_merged_rtc += entry["rtc"]
            merged_count += 1
        else:
            total_open_rtc += entry["rtc"]
            open_count += 1

    rtc_rate_usd = 0.15  # estimate — RTC/USD fluctuates

    print("=" * 60)
    print(f"  PAYOUT LEDGER — RustChain Bounty Tracking")
    print(f"  RTC Wallet: RTC17c0d21f04f6f65c1a85c0aeb5d4a305d57531096")
    print(f"  Generated: 2026-05-28")
    print(f"  30 PRs MERGED May 27-28 — awaiting paymaster")
    print("=" * 60)
    print()
    print(f"  {'Category':<15} {'Count':<8} {'RTC':<8} {'Est. USD':<10}")
    print(f"  {'─'*15} {'─'*8} {'─'*8} {'─'*10}")
    print(f"  {'Merged':<15} {merged_count:<8} {total_merged_rtc:<8} ${total_merged_rtc * rtc_rate_usd:<8.2f}")
    print(f"  {'Open':<15} {open_count:<8} {total_open_rtc:<8} ${total_open_rtc * rtc_rate_usd:<8.2f}")
    print(f"  {'TOTAL':<15} {merged_count+open_count:<8} {total_rtc:<8} ${total_rtc * rtc_rate_usd:<8.2f}")
    print()
    print(f"  All cells: {len(LEDGER)}")
    print(f"  RTC owed (merged): {total_merged_rtc} RTC ≈ ${total_merged_rtc * rtc_rate_usd:.2f}")
    print(f"  RTC pending (open): {total_open_rtc} RTC ≈ ${total_open_rtc * rtc_rate_usd:.2f}")
    print(f"  Total possible: {total_rtc} RTC ≈ ${total_rtc * rtc_rate_usd:.2f}")
    print()
    print("  Breakdown by type:")
    by_type = {}
    for entry in LEDGER.values():
        t = entry["type"]
        by_type.setdefault(t, {"count": 0, "rtc": 0})
        by_type[t]["count"] += 1
        by_type[t]["rtc"] += entry["rtc"]
    for t, v in sorted(by_type.items(), key=lambda x: -x[1]["rtc"]):
        print(f"    {t:<15} {v['count']:<5} PRs × {v['rtc']:<3} RTC = ${v['rtc'] * rtc_rate_usd:.2f}")
    print()
    print("  NOTE: RTC/USD ≈ $0.15 estimated. Paymaster decides actual rate.")
    print(f"  Open cells: {', '.join(sorted([k for k,v in LEDGER.items() if v['status']=='open']))}")
    print()

if __name__ == "__main__":
    compute_totals()
