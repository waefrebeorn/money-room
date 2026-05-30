# DA#3: Foundation — Triple Devil's Advocate Review

> **Phase 1: CLAIM** — "The system is aware of its own state. All Python will be converted to C. The walkway is current."

## Phase 2: VERIFY

### Python→C Conversion Reality
```
Files to convert: 14 Python scripts
Total Python: ~4,100 lines
Current C engine: ~2,700 LOC (room_engine, features, vote, capital, darwin, bridge)

Realistic conversion priority:
1. Pipeline scripts (earnings_calendar, etf_holdings, options_chain, seasonality, news_rss)
   → 5 files, ~1,455 lines — each fetches HTTP APIs, formats JSON, writes files
2. Ecosystem scripts (market_controller, money_loop, teachers)
   → 3 files, ~1,202 lines — the CORE runtime. Most complex, highest value.
3. MCP server (mcp_server.py)
   → 367 lines — Python-specific (MCP protocol library). C would need MCP binding → huge effort
4. Scripts (ab_test, param_sweep, param_tuner, stipend_system, walkforward)
   → 5 files, ~1,076 lines — mostly data analysis, plotting, stats. Python-native work.
```

**DA Finding: Full conversion is ~4 weeks of work.** Not a single session task. The ecosystem scripts (pm_money_loop.py, pm_teachers.py, pm_market_controller.py) are already partially C — the C room_engine does the core prediction. The Python ecosystem is a wrapper that loads/saves state and manages teachers. Converting it gains marginal speed but costs huge effort.

**Smarter approach:** Convert the pipeline scripts FIRST (small, independent, HTTP-based) — those benefit most from C (no Python startup overhead per cron tick). Leave ecosystem Python running — it's already cron-driven and not the bottleneck.

### Walkway Freshness Check
```
state.md: ✅ Updated this session (87.3%)
plan.md: ✅ Updated this session
vault: ✅ Updated this session
README: ✅ Updated this session
goal-mantra.md: ❌ DOES NOT EXIST at ~/.hermes/
mind-palace/ files: Some exist (~/.hermes/mind-palace/state.md, plan.md)
```

### System Fragilities
| Issue | Severity | Assessment |
|-------|----------|------------|
| **No goal-mantra.md** | 🟡 MEDIUM | The walkway entry point is missing. Lead-in is unclear. |
| **35 cron jobs, no health check** | 🟡 MEDIUM | One silent failure = data pipeline dead for days |
| **Snapshot freshness unenforced** | 🟡 MEDIUM | Free delay mechanism doesn't exist — would need a timestamp filter on demo_snapshot.json |
| **No auto-documentation** | 🟢 LOW | Repo docs updated manually each session |

## Phase 3: RISK

| Risk | Severity | Assessment |
|------|----------|------------|
| **Conversion scope creep** | 🔴 HIGH | 14 files = 4,100 lines across database, HTTP, JSON, ecosystem. Doing all at once will stall. |
| **Ecosystem Python in C ≠ faster** | 🟡 MEDIUM | The Python ecosystem already calls C binaries for the engine. Converting the wrapper doesn't improve performance. |
| **MCP server in C** | 🔴 HIGH | Python MCP libraries exist. Writing a C MCP server from scratch is weeks of work for zero functional gain. |
| **Missing goal-mantra** | 🟢 LOW | walkway works without it — state.md is the entry point |
| **Cron health silent** | 🟡 MEDIUM | Add heartbeat check to auto-test-runner |

## Phase 4: MITIGATE

| Mitigation | Action | Priority |
|-----------|--------|----------|
| **Convert pipeline Pythons** | earnings_calendar.py, etf_holdings.py, options_chain.py, seasonality.py, news_rss.py → C | 🔴 NOW |
| **Keep ecosystem Python** | pm_market_controller.py, pm_money_loop.py, pm_teachers.py already C-backed. Wrapper stays Python. | ✅ Decision |
| **Keep MCP server Python** | MCP protocol binding is Python-native. Zero value in C conversion. | ✅ Decision |
| **Convert analysis scripts** | ab_test.py, param_sweep.py, param_tuner.py, walkforward.py → C after pipelines | 🟡 NEXT |
| **Add cron health badge** | Check heartbeat timestamps in auto-test-runner | 🟡 NEXT |
