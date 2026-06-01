# GOAL-MANTRA — Money Room Walkway

## Loop
Read state → pick lowest undone cell → verify claim against source (function-level API, not filenames) → implement in C (libcurl+jansson+sqlite3, zero Python) → build → test → doc sweep ALL walkway files + README + BANNER + entry + index + testing + vault → version-bump in lockstep → grep stale numbers across .md tree → commit → push. Repeat.

## Classifications
- **PORTED** (≥80% features) — exists and works, no action needed
- **PARTIAL** (20-80%) — exists but incomplete, needs development
- **REAL GAP** (<20%) — doesn't exist, needs creation

## Rules
- No delegation. No empty responses. Show results after every action.
- Resolved → vault/achievements.md with file:line evidence.
- Battleship = active gaps ONLY. No checkmarks, no stale items.
- Memory: preferences, env facts, tool quirks, conventions only. NOT PRs, commits, SHAs, TODOs, version numbers.
- Skills = reusable patterns only. session_search for recall.
- No dates. No pacing. Work until done.

## Next Cell Selection
Priority order: P0 > P1 > P2 > P3 > P4, then old T-gaps. Within same priority, the one with existing C source but missing binary goes first (easiest win).
