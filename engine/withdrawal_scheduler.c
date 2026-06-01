/*
 * withdrawal_scheduler.c — C41: Automated profit withdrawal schedule
 *
 * Reads room_state.bin capital state and manages virtual withdrawal
 * tracking. Completes the C row (Capital/Money Engine) at 50/50.
 *
 * Withdrawal rules (configurable in SQLite):
 *   - threshold_pct: % profit above base before withdrawal triggers
 *   - withdrawal_pct: % of excess profits to withdraw each time
 *   - cooldown_cycles: min cycles between withdrawals
 *
 * Build: gcc -O3 -march=native withdrawal_scheduler.c -o withdrawal_scheduler -lsqlite3 -lm
 * Usage: ./withdrawal_scheduler status        — show current withdrawal state
 *        ./withdrawal_scheduler schedule       — compute next withdrawal plan
 *        ./withdrawal_scheduler history [N]    — show last N withdrawals
 *        ./withdrawal_scheduler config         — show current config
 *        ./withdrawal_scheduler config set <key> <val> — set config
 *        ./withdrawal_scheduler withdraw <amt> — record manual withdrawal
 *        ./withdrawal_scheduler reset          — clear all withdrawal records
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sqlite3.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#define DB_DIR      "/home/wubu2/.hermes/pm_logs/c_room"
#define DB_PATH     DB_DIR "/withdrawals.db"
#define STATE_PATH  DB_DIR "/room_state.bin"
#define SNAPSHOT    DB_DIR "/room_snapshot.json"

#define INITIAL_SEED  50.0f  /* $50 seed capital */

/* ─── RoomState fields we need (partial struct) ─── */
/* We mmap the file and read specific offsets.
 * From types.h layout (after MAGIC=4, last_updated=8, cycle=4):
 * N_FEATURES=76, agents first appear much later.
 * We cheat: read from the known float offsets in the struct.
 *
 * Offset to room_capital (float) in RoomState (approximate based on struct layout):
 * Actually, we'll parse room_snapshot.json for current capital
 * and use the binary for verification.
 */

/* ─── Config defaults ─── */
#define DEF_THRESHOLD_PCT   20.0    /* Withdraw when profit exceeds 20% of base */
#define DEF_WITHDRAWAL_PCT  50.0    /* Withdraw 50% of excess profits */
#define DEF_COOLDOWN_CYCLES 10000   /* Min cycles between withdrawals */
#define DEF_BASE_CAPITAL    50.0    /* $50 seed */

/* ─── DB helpers ─── */
static sqlite3 *open_db(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", DB_DIR);
    system(cmd);

    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return NULL;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    const char *sql =
        "CREATE TABLE IF NOT EXISTS withdrawals ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  amount REAL NOT NULL,"
        "  capital_before REAL,"
        "  capital_after REAL,"
        "  cycle INTEGER,"
        "  reason TEXT,"
        "  ts INTEGER DEFAULT (unixepoch())"
        ");"
        "CREATE TABLE IF NOT EXISTS config ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS withdrawal_log ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  cycle INTEGER,"
        "  current_capital REAL,"
        "  total_withdrawn REAL,"
        "  net_capital REAL,"
        "  profit_pct REAL,"
        "  next_withdrawal REAL,"
        "  ts INTEGER DEFAULT (unixepoch())"
        ");"
        "INSERT OR IGNORE INTO config VALUES ('threshold_pct', '20.0');"
        "INSERT OR IGNORE INTO config VALUES ('withdrawal_pct', '50.0');"
        "INSERT OR IGNORE INTO config VALUES ('cooldown_cycles', '10000');"
        "INSERT OR IGNORE INTO config VALUES ('base_capital', '50.0');";
    char *err = NULL;
    sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) { fprintf(stderr, "DB init error: %s\n", err); sqlite3_free(err); }
    return db;
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

/* ─── Get config value ─── */
static double get_config(sqlite3 *db, const char *key, double def) {
    sqlite3_stmt *stmt = NULL;
    double val = def;
    sqlite3_prepare_v2(db, "SELECT value FROM config WHERE key=?;", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        val = atof((const char*)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return val;
}

/* ─── Set config value ─── */
static int set_config(sqlite3 *db, const char *key, const char *value) {
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO config VALUES (?,?);", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        printf("[config] %s = %s\n", key, value);
        return 0;
    }
    return 1;
}

/* ─── Read current capital from room_state.bin ─── */
/* We mmap the binary and seek to room_capital.
 * From types.h, RoomState layout (approximately):
 *   uint32_t magic       (offset 0, 4B)
 *   int64_t last_updated (offset 4, 8B)  
 *   int cycle            (offset 12, 4B)
 *   MarketTick current_market (offset 16, ~248B depending on members)
 *   FeatureVector features   (offset ~264, 76*4=304B)
 *   int vote_count       (offset ~568, 4B)
 *   VoteRecord votes[MAX_AGENTS] (offset ~572, huge)
 *   ...
 *   float room_capital   (very far offset)
 *
 * Instead of computing the offset, we cheat by reading from room_snapshot.json
 * or computing from the binary via known struct sizes.
 *
 * Actually the simplest approach: read the last struct fields by
 * scanning the binary for the float value near the end.
 */

static int read_capital_from_state(float *capital, float *peak, int *cycle, int *trades) {
    /* Try snapshot JSON first (it's small) */
    FILE *f = fopen(SNAPSHOT, "r");
    if (f) {
        /* Read entire file */
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        char *buf = malloc(sz + 1);
        if (buf && fread(buf, 1, sz, f) == (size_t)sz) {
            buf[sz] = '\0';

            /* Hack: fix trailing comma issues by brute-force */
            /* Find key:value pairs via strstr */
            char *c = buf;
            char c_room[64] = {0}, c_peak[64] = {0};
            char c_cycle[64] = {0}, c_trades[64] = {0};

            char *p = strstr(c, "\"capital\":");
            if (p) {
                p += 10;
                while (*p && (*p == ' ' || *p == ',' || *p == '\n')) p++;
                char *end = p;
                while (*end && *end != ',' && *end != '}' && *end != '\n' && *end != '\r') end++;
                size_t n = end - p;
                if (n > 0 && n < 60) { memcpy(c_room, p, n); c_room[n] = 0; }
            }

            p = strstr(buf, "\"capital_peak\":");
            if (p) {
                p += 15;
                while (*p && (*p == ' ' || *p == ',' || *p == '\n')) p++;
                char *end = p;
                while (*end && *end != ',' && *end != '}' && *end != '\n' && *end != '\r') end++;
                size_t n = end - p;
                if (n > 0 && n < 60) { memcpy(c_peak, p, n); c_peak[n] = 0; }
            }

            p = strstr(buf, "\"trades_total\":");
            if (p) {
                p += 15;
                while (*p && (*p == ' ' || *p == ',')) p++;
                char *end = p;
                while (*end && *end != ',' && *end != '}' && *end != '\n') end++;
                size_t n = end - p;
                if (n > 0 && n < 60) { memcpy(c_trades, p, n); c_trades[n] = 0; }
            }

            p = strstr(buf, "\"cycle\":");
            if (p) {
                p += 8;
                while (*p && (*p == ' ' || *p == ',')) p++;
                char *end = p;
                while (*end && *end != ',' && *end != '}' && *end != '\n') end++;
                size_t n = end - p;
                if (n > 0 && n < 60) { memcpy(c_cycle, p, n); c_cycle[n] = 0; }
            }

            free(buf);

            if (c_room[0]) *capital = atof(c_room);
            if (c_peak[0]) *peak = atof(c_peak);
            if (c_cycle[0]) *cycle = atoi(c_cycle);
            if (c_trades[0]) *trades = atoi(c_trades);

            fclose(f);
            return (c_room[0] != 0) ? 0 : -1;
        }
        free(buf);
        fclose(f);
    }

    /* Fallback: try reading mmap'd binary directly */
    int fd = open(STATE_PATH, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return -1;

    /* RoomState struct layout from types.h:
     * The fields near the end are:
     * ... RoomTrade room_trade (big) ...
     * float prev_room_capital
     * volatile int writing
     * float nested_prediction
     * FeatureImportance feat_importance
     *
     * Since the struct is complex and version-dependent, 
     * we read the last 8 floats from the file (room_capital and room_capital_peak
     * should be in the last ~1KB)
     */
    float *floats = (float*)((char*)map + st.st_size - 256);
    int n_floats = 64;
    /* Search for a float between 1 and 1e9 that could be capital */
    for (int i = 0; i < n_floats - 1; i++) {
        float v = floats[i];
        float v2 = floats[i+1];
        if (v > 1.0f && v < 1e9f && v2 > 1.0f && v2 < 1e9f && v2 >= v) {
            *capital = v;
            *peak = v2;
            munmap(map, st.st_size);
            return 0;
        }
    }

    munmap(map, st.st_size);
    return -1;
}

/* ─── Compute scheduled withdrawal amount ─── */
static double compute_withdrawal(double capital, double base, 
                                  double threshold_pct, double withdrawal_pct) {
    if (capital <= base) return 0.0;
    double profit = capital - base;
    double profit_pct = (profit / base) * 100.0;
    if (profit_pct < threshold_pct) return 0.0;
    double excess = profit - (base * threshold_pct / 100.0);
    if (excess <= 0) return 0.0;
    return excess * (withdrawal_pct / 100.0);
}

/* ─── Command: status ─── */
static int cmd_status(void) {
    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "Cannot open DB.\n"); return 1; }

    float capital = INITIAL_SEED, peak = INITIAL_SEED;
    int cycle = 0, trades = 0;
    int rc = read_capital_from_state(&capital, &peak, &cycle, &trades);

    double base = get_config(db, "base_capital", DEF_BASE_CAPITAL);
    double threshold = get_config(db, "threshold_pct", DEF_THRESHOLD_PCT);
    double wd_pct = get_config(db, "withdrawal_pct", DEF_WITHDRAWAL_PCT);
    double cooldown = get_config(db, "cooldown_cycles", DEF_COOLDOWN_CYCLES);

    /* Total withdrawn */
    sqlite3_stmt *stmt = NULL;
    double total_wd = 0;
    sqlite3_prepare_v2(db, "SELECT COALESCE(SUM(amount), 0) FROM withdrawals;", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) total_wd = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);

    /* Last withdrawal cycle */
    int last_wd_cycle = 0;
    sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(cycle), 0) FROM withdrawals;", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) last_wd_cycle = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    /* Withdrawal count */
    int wd_count = 0;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM withdrawals;", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) wd_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    double net_capital = capital - total_wd;
    double profit = capital - base;
    double profit_pct = base > 0 ? (profit / base) * 100.0 : 0.0;
    double next_wd = compute_withdrawal(capital, base, threshold, wd_pct);

    int cycles_since_wd = (last_wd_cycle > 0) ? (cycle - last_wd_cycle) : cycle;
    bool can_withdraw = (next_wd > 0 && cycles_since_wd >= cooldown);

    printf("\n━━━ Withdrawal Status ━━━\n");
    if (rc == 0) {
        printf("  Room capital:     $%.2f\n", capital);
        printf("  Peak capital:     $%.2f\n", peak);
        printf("  Cycle:            %d\n", cycle);
        printf("  Room trades:      %d\n", trades);
    } else {
        printf("  ! Room state not available (using defaults)\n");
        printf("  Estimated capital: $%.2f\n", capital);
    }
    printf("  ─────────────────────────\n");
    printf("  Base capital:     $%.2f\n", base);
    printf("  Profit:           $%.2f (%.1f%%)\n", profit, profit_pct);
    printf("  Threshold:        %.0f%% profit\n", threshold);
    printf("  Withdrawal %%:     %.0f%% of excess\n", wd_pct);
    printf("  Cooldown:         %.0f cycles\n", cooldown);
    printf("  ─────────────────────────\n");
    printf("  Total withdrawn:  $%.2f (%d withdrawals)\n", total_wd, wd_count);
    printf("  Net capital:      $%.2f\n", net_capital);
    printf("  Next withdrawal:  $%.2f %s\n", next_wd,
           can_withdraw ? "✅ READY" : "⏳ waiting");
    if (!can_withdraw && next_wd > 0) {
        printf("    (need %d more cycles, %d elapsed)\n", 
               (int)cooldown, cycles_since_wd);
    }

    sqlite3_close(db);
    return 0;
}

/* ─── Command: schedule ─── */
static int cmd_schedule(void) {
    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "Cannot open DB.\n"); return 1; }

    float capital = INITIAL_SEED, peak = INITIAL_SEED;
    int cycle = 0, trades = 0;
    read_capital_from_state(&capital, &peak, &cycle, &trades);

    double base = get_config(db, "base_capital", DEF_BASE_CAPITAL);
    double threshold = get_config(db, "threshold_pct", DEF_THRESHOLD_PCT);
    double wd_pct = get_config(db, "withdrawal_pct", DEF_WITHDRAWAL_PCT);

    printf("\n━━━ Withdrawal Schedule ━━━\n");
    printf("  Base: $%.2f | Threshold: %.0f%% | Withdrawal: %.0f%% of excess\n\n", base, threshold, wd_pct);
    printf("  %12s %12s %10s %10s %s\n", "Capital", "Profit%", "Excess", "Withdraw", "Status");

    /* Project at various capital levels */
    double levels[] = {50, 60, 75, 100, 150, 200, 500, 1000, 5000, 10000, 50000, 100000};
    int n_levels = sizeof(levels) / sizeof(levels[0]);
    for (int i = 0; i < n_levels; i++) {
        double cap = levels[i];
        double profit = cap - base;
        double pct = base > 0 ? (profit / base) * 100.0 : 0;
        double wd = compute_withdrawal(cap, base, threshold, wd_pct);
        double excess = profit - (base * threshold / 100.0);
        if (excess < 0) excess = 0;
        char status[32] = "—";
        if (cap <= capital) snprintf(status, sizeof(status), "← current");
        else if (wd > 0) snprintf(status, sizeof(status), "✅ trigger");
        printf("  $%10.0f %10.0f%% $%8.0f $%8.0f  %s\n", cap, pct, excess, wd, status);
    }

    /* Next milestone to trigger withdrawal */
    if (capital < base * (1 + threshold/100.0)) {
        double needed = base * (1 + threshold/100.0) - capital;
        printf("\n  Next trigger: $%.2f more profit needed (at $%.2f capital)\n",
               needed, base * (1 + threshold/100.0));
    } else {
        double next_wd = compute_withdrawal(capital, base, threshold, wd_pct);
        if (next_wd > 0) {
            printf("\n  ✅ Current withdrawal: $%.2f ready\n", next_wd);
            printf("  Use: %s withdraw %.2f\n", "withdrawal_scheduler", next_wd);
        }
    }

    sqlite3_close(db);
    return 0;
}

/* ─── Command: history ─── */
static int cmd_history(int limit) {
    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "Cannot open DB.\n"); return 1; }

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, amount, capital_before, capital_after, cycle, reason, ts "
        "FROM withdrawals ORDER BY id DESC LIMIT ?;",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, limit);

    printf("\n━━━ Withdrawal History (last %d) ━━━\n", limit);
    printf("%-4s %10s %12s %12s %8s  %s\n", "ID", "Amount", "Cap Before", "Cap After", "Cycle", "Reason");
    int cnt = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%-4d $%8.2f $%10.2f $%10.2f %8d  %s\n",
               sqlite3_column_int(stmt, 0),
               sqlite3_column_double(stmt, 1),
               sqlite3_column_double(stmt, 2),
               sqlite3_column_double(stmt, 3),
               sqlite3_column_int(stmt, 4),
               (const char*)sqlite3_column_text(stmt, 5));
        cnt++;
    }
    sqlite3_finalize(stmt);

    if (!cnt) printf("  (no withdrawals recorded)\n");

    /* Summary */
    sqlite3_prepare_v2(db, "SELECT COUNT(*), COALESCE(SUM(amount),0) FROM withdrawals;", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("\n  Total: %d withdrawals, $%.2f withdrawn\n",
               sqlite3_column_int(stmt, 0), sqlite3_column_double(stmt, 1));
    }
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    return 0;
}

/* ─── Command: config ─── */
static int cmd_config(const char *action, const char *key, const char *value) {
    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "Cannot open DB.\n"); return 1; }

    if (!strcmp(action, "set") && key && value) {
        set_config(db, key, value);
    } else {
        sqlite3_stmt *stmt = NULL;
        sqlite3_prepare_v2(db, "SELECT key, value FROM config ORDER BY key;", -1, &stmt, NULL);
        printf("\n━━━ Withdrawal Config ━━━\n");
        printf("%-20s = %s\n", "Parameter", "Value");
        printf("────────────────────────────────\n");
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *k = (const char*)sqlite3_column_text(stmt, 0);
            const char *v = (const char*)sqlite3_column_text(stmt, 1);
            char label[32];
            snprintf(label, sizeof(label), "%-20s", k);
            printf("  %s = %s\n", label, v);
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return 0;
}

/* ─── Command: withdraw ─── */
static int cmd_withdraw(double amount) {
    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "Cannot open DB.\n"); return 1; }

    float capital = INITIAL_SEED, peak = INITIAL_SEED;
    int cycle = 0, trades = 0;
    read_capital_from_state(&capital, &peak, &cycle, &trades);

    double base = get_config(db, "base_capital", DEF_BASE_CAPITAL);
    double threshold = get_config(db, "threshold_pct", DEF_THRESHOLD_PCT);
    double wd_pct = get_config(db, "withdrawal_pct", DEF_WITHDRAWAL_PCT);
    double cooldown = get_config(db, "cooldown_cycles", DEF_COOLDOWN_CYCLES);

    /* Validate */
    double max_wd = compute_withdrawal(capital, base, threshold, wd_pct);
    if (amount <= 0) { printf("Amount must be positive.\n"); sqlite3_close(db); return 1; }

    if (amount > capital - base) {
        printf("Cannot withdraw $%.2f — only $%.2f is profit above $%.2f base.\n",
               amount, capital - base, base);
        printf("Maximum scheduled withdrawal: $%.2f\n", max_wd);
        sqlite3_close(db);
        return 1;
    }

    /* Check cooldown */
    int last_wd_cycle = 0;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(cycle), 0) FROM withdrawals;", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) last_wd_cycle = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    int cycles_since = cycle - last_wd_cycle;
    if (cycles_since < cooldown && last_wd_cycle > 0) {
        printf("Cooldown: need %d cycles since last withdrawal (%d elapsed).\n",
               (int)cooldown, cycles_since);
        printf("Use 'withdraw --force %.2f' to override.\n", amount);
        sqlite3_close(db);
        return 1;
    }

    /* Record withdrawal */
    double after = capital - amount;
    sqlite3_prepare_v2(db,
        "INSERT INTO withdrawals (amount, capital_before, capital_after, cycle, reason) "
        "VALUES (?,?,?,?,?);", -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, amount);
    sqlite3_bind_double(stmt, 2, capital);
    sqlite3_bind_double(stmt, 3, after);
    sqlite3_bind_int(stmt, 4, cycle);
    sqlite3_bind_text(stmt, 5, "manual", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        printf("\n✅ Withdrawal recorded: $%.2f\n", amount);
        printf("   Capital: $%.2f → $%.2f\n", capital, after);
        printf("   Cycle: %d\n", cycle);
    } else {
        printf("Error recording withdrawal.\n");
    }

    sqlite3_close(db);
    return 0;
}

/* ─── Command: reset ─── */
static int cmd_reset(void) {
    printf("Are you sure? This deletes ALL withdrawal records. (y/N): ");
    fflush(stdout);
    char resp[16];
    if (!fgets(resp, sizeof(resp), stdin)) return 1;
    if (resp[0] != 'y' && resp[0] != 'Y') { printf("Cancelled.\n"); return 0; }

    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "Cannot open DB.\n"); return 1; }
    sqlite3_exec(db, "DELETE FROM withdrawals;", NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM withdrawal_log;", NULL, NULL, NULL);
    printf("✅ All withdrawal records cleared.\n");
    sqlite3_close(db);
    return 0;
}

/* ─── Command: log (auto-run, records state snapshot) ─── */
static int cmd_log(void) {
    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "Cannot open DB.\n"); return 1; }

    float capital = INITIAL_SEED, peak = INITIAL_SEED;
    int cycle = 0, trades = 0;
    read_capital_from_state(&capital, &peak, &cycle, &trades);

    double base = get_config(db, "base_capital", DEF_BASE_CAPITAL);
    double threshold = get_config(db, "threshold_pct", DEF_THRESHOLD_PCT);
    double wd_pct = get_config(db, "withdrawal_pct", DEF_WITHDRAWAL_PCT);

    double total_wd = 0;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT COALESCE(SUM(amount), 0) FROM withdrawals;", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) total_wd = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);

    double net_capital = capital - total_wd;
    double profit_pct = base > 0 ? ((capital - base) / base) * 100.0 : 0;
    double next_wd = compute_withdrawal(capital, base, threshold, wd_pct);

    sqlite3_prepare_v2(db,
        "INSERT INTO withdrawal_log (cycle, current_capital, total_withdrawn, net_capital, profit_pct, next_withdrawal) "
        "VALUES (?,?,?,?,?,?);", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, cycle);
    sqlite3_bind_double(stmt, 2, capital);
    sqlite3_bind_double(stmt, 3, total_wd);
    sqlite3_bind_double(stmt, 4, net_capital);
    sqlite3_bind_double(stmt, 5, profit_pct);
    sqlite3_bind_double(stmt, 6, next_wd);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    printf("[withdrawal] log: cap=$%.2f withdrawn=$%.2f net=$%.2f profit=%.1f%% next=$%.2f\n",
           capital, total_wd, net_capital, profit_pct, next_wd);
    sqlite3_close(db);
    return 0;
}

static void print_usage(const char *p) {
    printf("Usage:\n");
    printf("  %s status              — show withdrawal state\n", p);
    printf("  %s schedule            — show withdrawal plan\n", p);
    printf("  %s history [N]         — show recent withdrawals\n", p);
    printf("  %s config              — show config\n", p);
    printf("  %s config set <k> <v>  — set config (threshold_pct, withdrawal_pct, cooldown_cycles, base_capital)\n", p);
    printf("  %s withdraw <amt>      — record a withdrawal\n", p);
    printf("  %s log                 — log current state snapshot\n", p);
    printf("  %s reset               — clear all records\n", p);
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    if (!strcmp(argv[1], "status"))
        return cmd_status();
    else if (!strcmp(argv[1], "schedule"))
        return cmd_schedule();
    else if (!strcmp(argv[1], "history"))
        return cmd_history(argc >= 3 ? atoi(argv[2]) : 10);
    else if (!strcmp(argv[1], "config")) {
        if (argc >= 4 && !strcmp(argv[2], "set"))
            return cmd_config("set", argv[3], argv[4]);
        else
            return cmd_config("show", NULL, NULL);
    } else if (!strcmp(argv[1], "withdraw") && argc >= 3)
        return cmd_withdraw(atof(argv[2]));
    else if (!strcmp(argv[1], "log"))
        return cmd_log();
    else if (!strcmp(argv[1], "reset"))
        return cmd_reset();
    else
        print_usage(argv[0]);
    return 0;
}
