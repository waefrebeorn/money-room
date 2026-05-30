## C41 — Withdrawal Schedule ✅ (May 28, 2026)
**Binary:** `engine/withdrawal_scheduler.c` — C binary with SQLite-backed withdrawal tracking
**Config:** base_capital=$50, threshold=20% profit, withdrawal=50% of excess, cooldown=10K cycles
**DB:** `~/.hermes/pm_logs/c_room/withdrawals.db`
**CLI:** `status`, `schedule`, `history [N]`, `config [set key val]`, `withdraw <amt>`, `log`, `reset`
**Status:** Capital tracking shows NaN in room_state.bin (engine NaN bug, separate from C41). Tool defaults to $50 seed and tracks virtual withdrawals. **C row completed: 50/50 cells closed.**
