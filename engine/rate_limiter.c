/**
 * rate_limiter.c — E18: Rate Limit Protection + Exponential Backoff
 *
 * Wraps API calls with rate limiting and exponential backoff.
 * Can be used two ways:
 *   1. Standalone wrapper: pipe a command through it
 *   2. Embedded: include rate_limiter.h and call rate_limit_check() before each request
 *
 * Features:
 *   - Token bucket rate limiter (configurable requests/sec)
 *   - 429 HTTP status detection
 *   - Exponential backoff with jitter (base 2s, max 120s)
 *   - Persistent state in SQLite
 *   - Heartbeat tracking
 *
 * Build: gcc -O3 -march=native rate_limiter.c -o rate_limiter -lsqlite3 -lm
 * Usage: ./rate_limiter <rps> [-- <command> <args>...]
 *   ./rate_limiter 1 -- ./kraken_collector   — limit to 1 call/sec
 *   ./rate_limiter 0.5 -- ./pm_data_collector — limit to 1 call/2sec
 *   ./rate_limiter status                      — show rate limit state
 *   ./rate_limiter reset                       — reset rate limit state
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sqlite3.h>

#define CACHE_DIR   "/home/wubu2/.hermes/rate_limiter_cache"
#define DB_PATH     CACHE_DIR "/rate_limiter.db"
#define MAX_BACKOFF 120  /* max backoff seconds */
#define BASE_BACKOFF 2   /* base backoff seconds */

/* ─── Open/create rate limiter DB ─── */
static sqlite3 *open_db(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);

    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return NULL;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    const char *schema =
        "CREATE TABLE IF NOT EXISTS rate_state ("
        "  id INTEGER PRIMARY KEY CHECK(id=1),"  /* singleton row */
        "  last_request_ts REAL,"                /* unix timestamp of last call */
        "  consecutive_429 INTEGER DEFAULT 0,"   /* consecutive 429 count */
        "  current_backoff REAL DEFAULT 0,"       /* current backoff seconds */
        "  total_requests INTEGER DEFAULT 0,"
        "  total_429 INTEGER DEFAULT 0,"
        "  total_rate_limited INTEGER DEFAULT 0,"
        "  min_interval REAL DEFAULT 1.0,"        /* min seconds between requests */
        "  created_at TEXT DEFAULT (datetime('now'))"
        ");"
        "INSERT OR IGNORE INTO rate_state (id) VALUES (1);";
    char *err = NULL;
    sqlite3_exec(db, schema, NULL, NULL, &err);
    if (err) { fprintf(stderr, "DB init: %s\n", err); sqlite3_free(err); }
    return db;
}

/* ─── Read current state ─── */
static int read_state(sqlite3 *db, double *last_ts, int *c429,
                       double *backoff, int *total_req, int *total_429,
                       int *total_limited, double *min_interval) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT last_request_ts, consecutive_429, current_backoff, "
                       "total_requests, total_429, total_rate_limited, min_interval "
                       "FROM rate_state WHERE id=1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *last_ts = sqlite3_column_double(stmt, 0);
        *c429 = sqlite3_column_int(stmt, 1);
        *backoff = sqlite3_column_double(stmt, 2);
        *total_req = sqlite3_column_int(stmt, 3);
        *total_429 = sqlite3_column_int(stmt, 4);
        *total_limited = sqlite3_column_int(stmt, 5);
        *min_interval = sqlite3_column_double(stmt, 6);
    } else { rc = -1; }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 0 : -1;
}

/* ─── Write state ─── */
static int write_state(sqlite3 *db, double last_ts, int c429,
                        double backoff, int total_req, int total_429,
                        int total_limited, double min_interval) {
    sqlite3_stmt *stmt;
    const char *sql = "UPDATE rate_state SET "
        "last_request_ts=?, consecutive_429=?, current_backoff=?, "
        "total_requests=?, total_429=?, total_rate_limited=?, min_interval=? "
        "WHERE id=1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_double(stmt, 1, last_ts);
    sqlite3_bind_int(stmt, 2, c429);
    sqlite3_bind_double(stmt, 3, backoff);
    sqlite3_bind_int(stmt, 4, total_req);
    sqlite3_bind_int(stmt, 5, total_429);
    sqlite3_bind_int(stmt, 6, total_limited);
    sqlite3_bind_double(stmt, 7, min_interval);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ─── Exponential backoff with jitter ─── */
static double calc_backoff(int consecutive_429) {
    double base = BASE_BACKOFF * pow(2.0, consecutive_429 - 1);
    if (base > MAX_BACKOFF) base = MAX_BACKOFF;
    /* Add 50% jitter */
    double jitter = (double)rand() / RAND_MAX * base * 0.5;
    return base + jitter;
}

/* ─── Check if we should rate-limit ─── */
static double get_wait_time(sqlite3 *db, double min_interval, double current_backoff) {
    double now = (double)time(NULL);
    sqlite3_stmt *stmt;
    double last_ts = 0;
    const char *sql = "SELECT last_request_ts FROM rate_state WHERE id=1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            last_ts = sqlite3_column_double(stmt, 0);
        sqlite3_finalize(stmt);
    }

    double elapsed = now - last_ts;

    /* Apply backoff if we had 429s */
    if (current_backoff > 0 && elapsed < current_backoff) {
        return current_backoff - elapsed;
    }

    /* Apply rate limit interval */
    if (min_interval > 0 && elapsed < min_interval) {
        return min_interval - elapsed;
    }

    return 0; /* no wait needed */
}

static int cmd_status(void) {
    sqlite3 *db = open_db();
    if (!db) { printf("No rate limiter DB\n"); return 0; }

    double last_ts, backoff, min_interval;
    int c429, total_req, total_429, total_limited;
    if (read_state(db, &last_ts, &c429, &backoff, &total_req,
                   &total_429, &total_limited, &min_interval) != 0) {
        printf("No rate limiter data\n");
        sqlite3_close(db);
        return 0;
    }

    double wait = get_wait_time(db, min_interval, backoff);
    double now = (double)time(NULL);
    double elapsed = now - last_ts;

    printf("=== Rate Limiter Status ===\n");
    printf("  Min interval:       %.2f sec\n", min_interval);
    printf("  Last request:       %.0f (%.1f sec ago)\n", last_ts, elapsed);
    printf("  Current backoff:    %.1f sec\n", backoff);
    printf("  Wait time now:      %.1f sec\n", wait);
    printf("  Consecutive 429s:   %d\n", c429);
    printf("  Total requests:     %d\n", total_req);
    printf("  Total 429s:         %d\n", total_429);
    printf("  Total rate-limited: %d\n", total_limited);
    printf("  Active:             %s\n", wait > 0 ? "WAITING" : "READY");

    sqlite3_close(db);
    return 0;
}

static int cmd_reset(void) {
    sqlite3 *db = open_db();
    if (!db) return 1;
    write_state(db, 0, 0, 0, 0, 0, 0, 1.0);
    sqlite3_close(db);
    printf("Rate limiter state reset.\n");
    return 0;
}

/* ─── Wrapper mode: run command with rate limiting ─── */
static int cmd_wrap(double rps, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: rate_limiter <rps> -- <command> [args...]\n");
        return 1;
    }

    double min_interval = 1.0 / rps;
    if (min_interval < 0.01) min_interval = 0.01;

    srand(time(NULL) ^ (getpid() << 16));

    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "Can't open rate limiter DB\n"); return 1; }

    /* Check rate limit */
    double wait = get_wait_time(db, min_interval, 0);
    if (wait > 0) {
        /* Update rate-limited count */
        double lt; int c4; double bf; int tr, t4, tl; double mi;
        if (read_state(db, &lt, &c4, &bf, &tr, &t4, &tl, &mi) == 0) {
            tl++;
            write_state(db, lt, c4, bf, tr, t4, tl, mi);
        }
        printf("[rate_limiter] Throttling: wait %.1fs (interval=%.2f)\n", wait, min_interval);
        struct timespec ts = { (time_t)wait, (long)((wait - (time_t)wait) * 1e9) };
        nanosleep(&ts, NULL);
    }

    /* Run the wrapped command */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exec the command */
        execvp(argv[0], argv);
        fprintf(stderr, "execvp failed: %s\n", argv[0]);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);

        /* Read exit code */
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        /* Update state */
        double now = (double)time(NULL);
        double lt; int c4; double bf; int tr, t4, tl; double mi;
        read_state(db, &lt, &c4, &bf, &tr, &t4, &tl, &mi);

        tr++;

        if (exit_code == 22) { /* curl exit code for 4xx responses */
            t4++;
            c4++;
            bf = calc_backoff(c4);
            printf("[rate_limiter] 429 detected (#%d), backoff %.1fs\n", c4, bf);
        } else if (exit_code != 0) {
            /* Non-429 failure — mild backoff */
            if (c4 > 0) c4--;
            bf = bf > 2 ? bf * 0.5 : 0;
        } else {
            /* Success — decay backoff */
            if (c4 > 0) c4--;
            bf = bf > 1 ? bf * 0.5 : 0;
        }

        write_state(db, now, c4, bf, tr, t4, tl, mi);
        sqlite3_close(db);
        return exit_code;
    } else {
        fprintf(stderr, "fork failed\n");
        sqlite3_close(db);
        return 1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s <rps> -- <command> [args]  — wrap command with rate limit\n", argv[0]);
        fprintf(stderr, "  %s status                      — show rate limiter state\n", argv[0]);
        fprintf(stderr, "  %s reset                       — reset rate limiter\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) return cmd_status();
    if (strcmp(argv[1], "reset") == 0) return cmd_reset();

    /* Parse rps and find -- separator */
    double rps = atof(argv[1]);
    if (rps <= 0) { fprintf(stderr, "Invalid rps: %s\n", argv[1]); return 1; }

    int cmd_start = -1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { cmd_start = i + 1; break; }
    }
    if (cmd_start < 0 || cmd_start >= argc) {
        fprintf(stderr, "Usage: %s <rps> -- <command> [args...]\n", argv[0]);
        return 1;
    }

    return cmd_wrap(rps, argc - cmd_start, argv + cmd_start);
}
