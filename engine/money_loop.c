/*
 * money_loop.c — C port of pm_money_loop.py (10K Genome Trading Ecosystem)
 *
 * Loads 10K genomes → reads market_feed.json → computes signals →
 * executes paper trades → resolves → logs → saves state.
 *
 * Runs every 5 min via cron. No Python. No crashes.
 *
 * Build: gcc -O3 -march=native -Wall -Wextra -std=c11 \
 *            money_loop.c -o money_loop -ljansson -lsqlite3 -lm
 *
 * Dependencies: gene_pool.npy, portfolios.json, open_trades.json, market_feed.json
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <jansson.h>
#include <sqlite3.h>

/* ─── Paths ─── */
#define HOME_DIR        "/home/wubu2"
#define ECO_DIR         HOME_DIR "/.hermes/pm_logs/eco"
#define ROOM_DIR        HOME_DIR "/.hermes/pm_logs/c_room"
#define GENE_POOL_PATH  ECO_DIR "/gene_pool.npy"
#define PORTFOLIOS_PATH ECO_DIR "/portfolios.json"
#define TRADES_PATH     ECO_DIR "/open_trades.json"
#define FEED_PATH       ROOM_DIR "/market_feed.json"
#define FITNESS_PATH    ECO_DIR "/fitness.npy"
#define SUMMARY_PATH    ECO_DIR "/gene_pool_summary.json"
#define ECO_DB_PATH     ECO_DIR "/eco.db"

/* ─── Constants ─── */
#define N_GENOMES       10000
#define N_PARAMS        11
#define SEED_CAPITAL    1000.0
#define FEE_RATE        0.001
#define TRADE_RESOLVE_SECS 60
#define MAX_TRADES_PER_AGENT 5
#define VALHALLA_THRESHOLD 3000.0
#define TEACHER_THRESHOLD  5000.0
#define MAX_TEACHERS    20
#define MAX_OPEN_TRADES_PER_AGENT 5

/* ─── Genome param indices ─── */
#define IDX_POSITION_SIZE        0
#define IDX_CONVICTION_THRESHOLD 1
#define IDX_RISK_TOLERANCE      2
#define IDX_LIE_SENSITIVITY     3
#define IDX_HERD_ANTIPATHY      4
#define IDX_STOP_LOSS_PCT       5
#define IDX_TAKE_PROFIT_PCT     6
#define IDX_MIN_EDGE_PCT        7
#define IDX_MIN_VOLUME          8
#define IDX_TIME_HORIZON        9
#define IDX_MEAN_REVERSION_BIAS 10

/* ─── Trade structure ─── */
typedef struct {
    char id[64];
    char genome_id[16];
    int side;            /* 1 = BUY_YES, 0 = BUY_NO */
    double entry;
    double cost;
    double qty;
    double edge;
    double stop_loss_pct;
    double take_profit_pct;
    double ts;
    int status;          /* 0 = open, 1 = closed */
    int won;
    double payout;
} Trade;

/* ─── Portfolio per genome ─── */
typedef struct {
    double cash;
    double total_pnl;
    double total_raw_pnl;
    int total_trades;
    int wins;
    int losses;
    int open_trades;
    double peak_cash;
    double max_drawdown;
} Portfolio;

/* ─── Market state ─── */
typedef struct {
    double open, high, low, close, volume;
    double fear_greed;
    double pump_score;
    double btc_30d_volatility;
    long window_ts;
    double anti_signal;
    double sp500;
    double vix;
    double btc_dominance;
} MarketState;

/* ─── Genome parameters ─── */
typedef float Genome[N_PARAMS];

/* ═══════════════════════════════════════════════
 * NPY Loader
 * ═══════════════════════════════════════════════ */

static int load_gene_pool(const char *path, Genome *pool, int max_genomes) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[money_loop] Cannot open %s\n", path);
        return 0;
    }

    /* Read magic + version (6 bytes magic + 2 bytes version) */
    char magic[6];
    if (fread(magic, 1, 6, f) != 6 || magic[0] != (char)0x93 || 
        memcmp(magic+1, "NUMPY", 5) != 0) {
        fprintf(stderr, "[money_loop] Invalid .npy file: %s\n", path);
        fclose(f);
        return 0;
    }

    /* Read version (2 bytes: major, minor) */
    unsigned char ver[2];
    fread(ver, 1, 2, f);

    /* Read header length (2 bytes, little-endian uint16 for v1) */
    unsigned short header_len;
    fread(&header_len, 2, 1, f);

    /* Read and skip header */
    char *header = malloc(header_len + 1);
    fread(header, 1, header_len, f);
    header[header_len] = 0;

    /* Parse header to get shape */
    /* Format: {'descr': '<f4', 'fortran_order': False, 'shape': (N, 11), } */
    int n_rows = 0, n_cols = 0;
    char *shape_start = strstr(header, "shape");
    if (shape_start) {
        char *paren = strchr(shape_start, '(');
        if (paren) {
            sscanf(paren + 1, "%d, %d", &n_rows, &n_cols);
        }
    }

    if (n_rows == 0 || n_cols == 0) {
        fprintf(stderr, "[money_loop] Could not parse shape from npy header\n");
        free(header);
        fclose(f);
        return 0;
    }

    int to_read = (n_rows < max_genomes) ? n_rows : max_genomes;
    size_t bytes_per_row = n_cols * sizeof(float);

    for (int i = 0; i < to_read; i++) {
        fread(pool[i], 1, bytes_per_row, f);
    }

    free(header);
    fclose(f);
    fprintf(stderr, "[money_loop] Loaded %d genomes from %s (%dx%d)\n", to_read, path, n_rows, n_cols);
    return to_read;
}

/* ═══════════════════════════════════════════════
 * JSON Portfolio Loader
 * ═══════════════════════════════════════════════ */

static int load_portfolios(const char *path, Portfolio *portfolios, int max_genomes) {
    for (int i = 0; i < max_genomes; i++) {
        portfolios[i].cash = SEED_CAPITAL;
        portfolios[i].total_pnl = 0;
        portfolios[i].total_raw_pnl = 0;
        portfolios[i].total_trades = 0;
        portfolios[i].wins = 0;
        portfolios[i].losses = 0;
        portfolios[i].open_trades = 0;
        portfolios[i].peak_cash = SEED_CAPITAL;
        portfolios[i].max_drawdown = 0;
    }

    json_t *root = json_load_file(path, 0, NULL);
    if (!root || !json_is_object(root)) {
        if (root) json_decref(root);
        return 0;
    }

    /* Iterate genome entries */
    const char *key;
    json_t *val;
    json_object_foreach(root, key, val) {
        int gid = atoi(key);
        if (gid < 0 || gid >= max_genomes) continue;

        Portfolio *p = &portfolios[gid];
        json_t *v;

        v = json_object_get(val, "cash");
        if (v && json_is_real(v)) p->cash = json_real_value(v);
        v = json_object_get(val, "total_pnl");
        if (v && json_is_real(v)) p->total_pnl = json_real_value(v);
        v = json_object_get(val, "total_raw_pnl");
        if (v && json_is_real(v)) p->total_raw_pnl = json_real_value(v);
        v = json_object_get(val, "total_trades");
        if (v && json_is_integer(v)) p->total_trades = json_integer_value(v);
        v = json_object_get(val, "wins");
        if (v && json_is_integer(v)) p->wins = json_integer_value(v);
        v = json_object_get(val, "losses");
        if (v && json_is_integer(v)) p->losses = json_integer_value(v);
        v = json_object_get(val, "peak_cash");
        if (v && json_is_real(v)) p->peak_cash = json_real_value(v);
        v = json_object_get(val, "max_drawdown");
        if (v && json_is_real(v)) p->max_drawdown = json_real_value(v);
    }

    json_decref(root);
    return 1;
}

/* ═══════════════════════════════════════════════
 * JSON Portfolio Saver
 * ═══════════════════════════════════════════════ */

static int save_portfolios(const char *path, Portfolio *portfolios, int n_genomes) {
    json_t *root = json_object();

    for (int i = 0; i < n_genomes; i++) {
        Portfolio *p = &portfolios[i];
        char key[16];
        snprintf(key, sizeof(key), "%d", i);

        json_t *entry = json_object();
        json_object_set_new(entry, "cash", json_real(p->cash));
        json_object_set_new(entry, "total_pnl", json_real(p->total_pnl));
        json_object_set_new(entry, "total_raw_pnl", json_real(p->total_raw_pnl));
        json_object_set_new(entry, "total_trades", json_integer(p->total_trades));
        json_object_set_new(entry, "wins", json_integer(p->wins));
        json_object_set_new(entry, "losses", json_integer(p->losses));
        json_object_set_new(entry, "open", json_integer(p->open_trades));
        json_object_set_new(entry, "peak_cash", json_real(p->peak_cash));
        json_object_set_new(entry, "max_drawdown", json_real(p->max_drawdown));

        json_object_set_new(root, key, entry);
    }

    /* Write atomically */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    json_dump_file(root, tmp, JSON_INDENT(2));
    rename(tmp, path);
    json_decref(root);
    return 0;
}

/* ═══════════════════════════════════════════════
 * Market Feed Loader
 * ═══════════════════════════════════════════════ */

static int load_market_feed(MarketState *mkt) {
    memset(mkt, 0, sizeof(*mkt));
    mkt->close = 75000;  /* Default fallback */

    json_t *root = json_load_file(FEED_PATH, 0, NULL);
    if (!root) return 0;

    json_t *v;
    v = json_object_get(root, "open");   if (v && json_is_real(v)) mkt->open = json_real_value(v);
    v = json_object_get(root, "high");   if (v && json_is_real(v)) mkt->high = json_real_value(v);
    v = json_object_get(root, "low");    if (v && json_is_real(v)) mkt->low = json_real_value(v);
    v = json_object_get(root, "close");  if (v && json_is_real(v)) mkt->close = json_real_value(v);
    v = json_object_get(root, "volume"); if (v && json_is_real(v)) mkt->volume = json_real_value(v);
    v = json_object_get(root, "fear_greed"); if (v && json_is_real(v)) mkt->fear_greed = json_real_value(v);
    v = json_object_get(root, "pump_score"); if (v && json_is_real(v)) mkt->pump_score = json_real_value(v);
    v = json_object_get(root, "btc_30d_volatility"); if (v && json_is_real(v)) mkt->btc_30d_volatility = json_real_value(v);
    v = json_object_get(root, "window_ts"); if (v && json_is_integer(v)) mkt->window_ts = json_integer_value(v);
    v = json_object_get(root, "anti_signal"); if (v && json_is_real(v)) mkt->anti_signal = json_real_value(v);
    v = json_object_get(root, "sp500"); if (v && json_is_real(v)) mkt->sp500 = json_real_value(v);
    v = json_object_get(root, "vix"); if (v && json_is_real(v)) mkt->vix = json_real_value(v);
    v = json_object_get(root, "btc_dominance"); if (v && json_is_real(v)) mkt->btc_dominance = json_real_value(v);

    json_decref(root);
    return 1;
}

/* ═══════════════════════════════════════════════
 * Open Trades Loader
 * ═══════════════════════════════════════════════ */

static int load_trades(const char *path, Trade *all_trades, int *n_trades,
                       int per_genome_count[N_GENOMES]) {
    *n_trades = 0;
    memset(per_genome_count, 0, N_GENOMES * sizeof(int));

    json_t *root = json_load_file(path, 0, NULL);
    if (!root || !json_is_object(root)) {
        if (root) json_decref(root);
        return 0;
    }

    const char *gid_str;
    json_t *trade_list;
    json_object_foreach(root, gid_str, trade_list) {
        int gid = atoi(gid_str);
        if (gid < 0 || gid >= N_GENOMES || !json_is_array(trade_list)) continue;

        size_t idx;
        json_t *t;
        json_array_foreach(trade_list, idx, t) {
            if (*n_trades >= 100000) break;  /* Safety cap */

            Trade *tr = &all_trades[*n_trades];
            memset(tr, 0, sizeof(Trade));

            json_t *v;
            v = json_object_get(t, "id"); if (v && json_is_string(v))
                strncpy(tr->id, json_string_value(v), sizeof(tr->id)-1);
            v = json_object_get(t, "genome_id"); if (v && json_is_string(v))
                strncpy(tr->genome_id, json_string_value(v), sizeof(tr->genome_id)-1);
            v = json_object_get(t, "side");
            if (v && json_is_string(v))
                tr->side = (strcmp(json_string_value(v), "BUY_YES") == 0) ? 1 : 0;
            v = json_object_get(t, "entry"); if (v && json_is_real(v)) tr->entry = json_real_value(v);
            v = json_object_get(t, "cost");  if (v && json_is_real(v)) tr->cost = json_real_value(v);
            v = json_object_get(t, "qty");   if (v && json_is_real(v)) tr->qty = json_real_value(v);
            v = json_object_get(t, "edge");  if (v && json_is_real(v)) tr->edge = json_real_value(v);
            v = json_object_get(t, "stop_loss_pct"); if (v && json_is_real(v)) tr->stop_loss_pct = json_real_value(v);
            v = json_object_get(t, "take_profit_pct"); if (v && json_is_real(v)) tr->take_profit_pct = json_real_value(v);
            v = json_object_get(t, "ts"); if (v && json_is_real(v)) tr->ts = json_real_value(v);
            v = json_object_get(t, "status");
            if (v && json_is_string(v)) tr->status = (strcmp(json_string_value(v), "open") == 0) ? 0 : 1;

            tr->won = 0;
            tr->payout = 0;
            per_genome_count[gid]++;
            (*n_trades)++;
        }
    }

    json_decref(root);
    return 1;
}

/* ─── Save open trades to JSON ─── */
static int save_trades(const char *path, Trade *all_trades, int n_trades) {
    json_t *root = json_object();

    for (int i = 0; i < n_trades; i++) {
        Trade *tr = &all_trades[i];
        int gid = atoi(tr->genome_id);
        char key[16];
        snprintf(key, sizeof(key), "%d", gid);

        json_t *list = json_object_get(root, key);
        if (!list) {
            list = json_array();
            json_object_set_new(root, key, list);
        }

        json_t *entry = json_object();
        json_object_set_new(entry, "id", json_string(tr->id));
        json_object_set_new(entry, "genome_id", json_string(tr->genome_id));
        json_object_set_new(entry, "side", json_string(tr->side ? "BUY_YES" : "BUY_NO"));
        json_object_set_new(entry, "entry", json_real(tr->entry));
        json_object_set_new(entry, "cost", json_real(tr->cost));
        json_object_set_new(entry, "qty", json_real(tr->qty));
        json_object_set_new(entry, "edge", json_real(tr->edge));
        json_object_set_new(entry, "stop_loss_pct", json_real(tr->stop_loss_pct));
        json_object_set_new(entry, "take_profit_pct", json_real(tr->take_profit_pct));
        json_object_set_new(entry, "ts", json_real(tr->ts));
        json_object_set_new(entry, "status", json_string(tr->status == 0 ? "open" : "closed"));
        json_object_set_new(entry, "_source", json_string("money_loop"));
        json_object_set_new(entry, "fee_rate", json_real(FEE_RATE));

        json_array_append_new(list, entry);
    }

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    json_dump_file(root, tmp, JSON_INDENT(2));
    rename(tmp, path);
    json_decref(root);
    return 0;
}

/* ═══════════════════════════════════════════════
 * Signal Computation
 * ═══════════════════════════════════════════════ */

typedef struct {
    int direction;  /* -1 = SELL/NONE, 0 = NONE, 1 = BUY */
    double qty;
    double edge;
} Signal;

/* ─── Compute trading signal from genome params + market state ─── */
static Signal compute_signal(Genome *genome, MarketState *mkt, Portfolio *pf) {
    Signal sig = {0, 0, 0};

    double position_size  = (double)(*genome)[IDX_POSITION_SIZE];
    double conviction_th  = (double)(*genome)[IDX_CONVICTION_THRESHOLD];
    double risk_tolerance = (double)(*genome)[IDX_RISK_TOLERANCE];
    double lie_sens       = (double)(*genome)[IDX_LIE_SENSITIVITY];
    double herd_anti      = (double)(*genome)[IDX_HERD_ANTIPATHY];
    double min_edge       = (double)(*genome)[IDX_MIN_EDGE_PCT];
    double min_vol        = (double)(*genome)[IDX_MIN_VOLUME];
    double time_horizon   = (double)(*genome)[IDX_TIME_HORIZON];
    double mean_rev       = (double)(*genome)[IDX_MEAN_REVERSION_BIAS];

    /* Normalize position_size to fraction of cash */
    double trade_cash = pf->cash * fmin(position_size / 20.0, 0.5);
    if (trade_cash < 1.0) return sig;  /* Not enough cash */
    trade_cash = fmin(trade_cash, pf->cash * 0.5);

    /* Conviction: is the genome confident? */
    double conviction = fmin(conviction_th * 2.0, 1.0);
    if (conviction < 0.3) return sig;  /* Not confident enough */

    /* Market features */
    double price_move = 0;
    /* Use normalized price movement as signal */
    if (mkt->close > 0 && mkt->open > 0) {
        price_move = (mkt->close - mkt->open) / mkt->open;
    }

    /* Anti-signal: pump_score tells us if news are pumping */
    double pump = mkt->pump_score;

    /* Combine signals: price movement + pump + fear_greed + anti_signal */
    double signal = price_move * 5.0;  /* Amplify */

    /* Mean reversion bias: if price moved a lot, bet against it */
    if (mean_rev > 0.5) {
        signal = -signal;  /* Reverse direction */
    }

    /* Pump score: high pump = trend following */
    if (pump > 0.3) signal += pump * 0.5;
    if (pump < -0.3) signal -= pump * 0.3;  /* Fear = buy (contrarian) */

    /* Fear & Greed: extreme fear = buy signal */
    double fg = mkt->fear_greed;
    if (fg < 20) signal += 0.3;   /* Extreme fear = buy */
    if (fg > 80) signal += -0.2;  /* Extreme greed = sell */

    /* Anti-herd: if signal is positive but herd_anti is high, reduce */
    if (herd_anti > 0.5 && signal > 0)
        signal *= (1.0 - herd_anti * 0.5);

    /* Lie sensitivity: distrust strong signals */
    if (lie_sens > 0.5 && fabs(signal) > 0.5)
        signal *= 0.5;

    /* Check edge threshold */
    double edge = fabs(signal);
    if (edge < min_edge * 0.1) return sig;  /* Edge too small */

    /* Volume filter */
    if (mkt->volume < min_vol * 1000) return sig;  /* Volume too low */

    /* Direction */
    if (signal > conviction_th * 0.1) {
        sig.direction = 1;  /* BUY_YES (UP) */
    } else if (signal < -conviction_th * 0.1) {
        sig.direction = 0;  /* BUY_NO (DOWN) */
    } else {
        return sig;  /* No signal */
    }

    sig.qty = trade_cash;
    sig.edge = edge;
    return sig;
}

/* ═══════════════════════════════════════════════
 * Trade Execution
 * ═══════════════════════════════════════════════ */

static int execute_trade(Trade *all_trades, int *n_trades, int *trade_id_counter,
                         int gid, Signal *sig, MarketState *mkt,
                         Portfolio *portfolios, double stop_loss, double take_profit) {
    if (gid < 0 || gid >= N_GENOMES) return 0;
    Portfolio *pf = &portfolios[gid];

    if (pf->cash < sig->qty) return 0;  /* Insufficient funds */

    pf->cash -= sig->qty;
    pf->open_trades++;
    pf->total_trades++;

    Trade *tr = &all_trades[*n_trades];
    memset(tr, 0, sizeof(Trade));
    snprintf(tr->id, sizeof(tr->id), "E%d-%d", gid, *trade_id_counter);
    snprintf(tr->genome_id, sizeof(tr->genome_id), "%d", gid);
    tr->side = sig->direction;
    tr->entry = mkt->close;
    tr->cost = sig->qty;
    tr->qty = sig->qty;
    tr->edge = sig->edge;
    tr->stop_loss_pct = stop_loss;
    tr->take_profit_pct = take_profit;
    tr->ts = (double)time(NULL);
    tr->status = 0;  /* open */

    (*n_trades)++;
    (*trade_id_counter)++;
    return 1;
}

/* ═══════════════════════════════════════════════
 * Trade Resolution
 * ═══════════════════════════════════════════════ */

static int resolve_trades(Trade *all_trades, int *n_trades, MarketState *mkt,
                          Portfolio *portfolios, double *net_pnl) {
    double price = mkt->close;
    double prev_close = mkt->open;
    int up_won = (price >= prev_close) ? 1 : 0;
    int resolved = 0;
    *net_pnl = 0;

    for (int i = 0; i < *n_trades; i++) {
        Trade *tr = &all_trades[i];
        if (tr->status != 0) continue;  /* Already closed */

        double now = (double)time(NULL);

        /* Check if trade has aged enough */
        if ((now - tr->ts) < TRADE_RESOLVE_SECS) {
            /* SL/TP check */
            double ret_from_entry = (price - tr->entry) / tr->entry;

            if (tr->take_profit_pct > 0) {
                int tp_hit = (tr->side == 1 && ret_from_entry >= tr->take_profit_pct / 100.0) ||
                             (tr->side == 0 && -ret_from_entry >= tr->take_profit_pct / 100.0);
                if (!tp_hit) continue;  /* Keep open */
            } else if (tr->stop_loss_pct > 0) {
                int sl_hit = (tr->side == 1 && -ret_from_entry >= tr->stop_loss_pct / 100.0) ||
                             (tr->side == 0 && ret_from_entry >= tr->stop_loss_pct / 100.0);
                if (!sl_hit) continue;  /* Keep open */
            } else {
                continue;  /* Keep open */
            }
        }

        /* Resolve */
        int won = (tr->side == 1 && up_won) || (tr->side == 0 && !up_won);
        double cost = tr->cost;
        double taker_fee = cost * FEE_RATE;

        *net_pnl -= taker_fee;

        int gid = atoi(tr->genome_id);
        Portfolio *pf = &portfolios[gid];

        if (won) {
            double payout = cost - taker_fee;
            *net_pnl += payout;
            pf->cash += cost + payout;
            pf->wins++;
            pf->total_pnl += payout;
            pf->total_raw_pnl += payout;
            tr->payout = payout;
        } else {
            *net_pnl -= cost;
            pf->cash -= taker_fee;
            pf->losses++;
            pf->total_pnl -= cost;
            pf->total_raw_pnl -= cost;
            tr->payout = -(cost + taker_fee);
        }

        tr->status = 1;  /* closed */
        tr->won = won;
        pf->open_trades--;
        resolved++;
    }

    /* Remove closed trades by compacting the array */
    int write_idx = 0;
    for (int read_idx = 0; read_idx < *n_trades; read_idx++) {
        if (all_trades[read_idx].status == 0) {  /* still open */
            if (read_idx != write_idx) {
                memcpy(&all_trades[write_idx], &all_trades[read_idx], sizeof(Trade));
            }
            write_idx++;
        }
    }
    *n_trades = write_idx;

    return resolved;
}

/* ═══════════════════════════════════════════════
 * SQLite Logging (E13/E14)
 * ═══════════════════════════════════════════════ */

static sqlite3 *eco_db = NULL;

static int init_eco_db(void) {
    if (sqlite3_open(ECO_DB_PATH, &eco_db) != SQLITE_OK) {
        fprintf(stderr, "[eco_db] Cannot open %s\n", ECO_DB_PATH);
        eco_db = NULL;
        return -1;
    }
    sqlite3_exec(eco_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(eco_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    const char *schema =
        "CREATE TABLE IF NOT EXISTS snapshots ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts REAL NOT NULL, minute TEXT, regime INTEGER, gen INTEGER,"
        "  population INTEGER, mean_pnl REAL, median_pnl REAL,"
        "  top10_pnl REAL, bottom10_pnl REAL, mean_cash REAL,"
        "  total_trades INTEGER, open_trades INTEGER, total_resolved INTEGER,"
        "  purged INTEGER, created_at TEXT DEFAULT (datetime('now'))"
        ");"
        "CREATE TABLE IF NOT EXISTS trades ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  trade_ref TEXT UNIQUE, genome_id TEXT NOT NULL,"
        "  direction TEXT NOT NULL, entry_price REAL NOT NULL,"
        "  stake REAL NOT NULL, pnl REAL DEFAULT 0,"
        "  conviction REAL DEFAULT 0, opened_at REAL NOT NULL,"
        "  resolved_at REAL, outcome TEXT DEFAULT 'open',"
        "  trade_ts REAL DEFAULT (unixepoch())"
        ");";
    sqlite3_exec(eco_db, schema, NULL, NULL, NULL);
    return 0;
}

static void log_snapshot(Portfolio *portfolios, int n_genomes,
                         int total_trades, int open_count, int resolved, int purged,
                         double net_pnl) {
    if (!eco_db) return;

    /* Compute stats */
    double sum_pnl = 0, sum_cash = 0;
    int n_active = 0;
    double *pnls = malloc(n_genomes * sizeof(double));
    double *cashes = malloc(n_genomes * sizeof(double));

    for (int i = 0; i < n_genomes; i++) {
        if (portfolios[i].total_trades > 0 || portfolios[i].cash != SEED_CAPITAL) {
            pnls[n_active] = portfolios[i].total_pnl;
            cashes[n_active] = portfolios[i].cash;
            sum_pnl += portfolios[i].total_pnl;
            sum_cash += portfolios[i].cash;
            n_active++;
        }
    }

    double mean_pnl = n_active > 0 ? sum_pnl / n_active : 0;
    double mean_cash = n_active > 0 ? sum_cash / n_active : 0;

    /* Sort for median/percentiles */
    if (n_active > 0) {
        /* Simple bubble sort for top10 */
        for (int i = 0; i < n_active - 1; i++) {
            for (int j = 0; j < n_active - i - 1; j++) {
                if (pnls[j] < pnls[j+1]) {
                    double tmp = pnls[j]; pnls[j] = pnls[j+1]; pnls[j+1] = tmp;
                }
            }
        }
    }

    double median_pnl = n_active > 0 ? pnls[n_active/2] : 0;
    int top10_n = n_active > 10 ? n_active / 10 : n_active;
    double top10_pnl = 0, bottom10_pnl = 0;
    if (top10_n > 0) {
        for (int i = 0; i < top10_n; i++) top10_pnl += pnls[i];
        top10_pnl /= top10_n;
        for (int i = n_active - top10_n; i < n_active; i++) bottom10_pnl += pnls[i];
        bottom10_pnl /= top10_n;
    }

    /* Get time string */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char minute[32];
    strftime(minute, sizeof(minute), "%Y-%m-%dT%H:%M", tm);

    /* Insert into SQLite */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(eco_db,
        "INSERT INTO snapshots (ts,minute,regime,gen,population,"
        "mean_pnl,median_pnl,top10_pnl,bottom10_pnl,mean_cash,"
        "total_trades,open_trades,total_resolved,purged) "
        "VALUES (?,?,0,0,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, (double)time(NULL));
    sqlite3_bind_text(stmt, 2, minute, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, n_active);
    sqlite3_bind_double(stmt, 4, mean_pnl);
    sqlite3_bind_double(stmt, 5, median_pnl);
    sqlite3_bind_double(stmt, 6, top10_pnl);
    sqlite3_bind_double(stmt, 7, bottom10_pnl);
    sqlite3_bind_double(stmt, 8, mean_cash);
    sqlite3_bind_int(stmt, 9, total_trades);
    sqlite3_bind_int(stmt, 10, open_count);
    sqlite3_bind_int(stmt, 11, resolved);
    sqlite3_bind_int(stmt, 12, purged);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Also write JSONL for backward compat */
    json_t *entry = json_object();
    json_object_set_new(entry, "minute", json_string(minute));
    json_object_set_new(entry, "ts", json_real((double)time(NULL)));
    json_object_set_new(entry, "regime", json_integer(0));
    json_object_set_new(entry, "gen", json_integer(0));
    json_object_set_new(entry, "population", json_integer(n_active));
    json_object_set_new(entry, "mean_pnl", json_real(mean_pnl));
    json_object_set_new(entry, "median_pnl", json_real(median_pnl));
    json_object_set_new(entry, "top10_pnl", json_real(top10_pnl));
    json_object_set_new(entry, "bottom10_pnl", json_real(bottom10_pnl));
    json_object_set_new(entry, "mean_cash", json_real(mean_cash));
    json_object_set_new(entry, "total_trades", json_integer(total_trades));
    json_object_set_new(entry, "open_trades", json_integer(open_count));
    json_object_set_new(entry, "total_resolved", json_integer(resolved));
    json_object_set_new(entry, "purged", json_integer(purged));

    char jsonl_path[256];
    snprintf(jsonl_path, sizeof(jsonl_path), ECO_DIR "/minute_log.jsonl");
    char *dump = json_dumps(entry, 0);
    FILE *f = fopen(jsonl_path, "a");
    if (f) { fprintf(f, "%s\n", dump); fclose(f); }
    free(dump);
    json_decref(entry);

    fprintf(stderr, "[money_loop] snapshot: pop=%d mean_pnl=$%.2f median_pnl=$%.2f cash=$%.2f trades=%d resolved=%d\n",
            n_active, mean_pnl, median_pnl, mean_cash, total_trades, resolved);

    free(pnls);
    free(cashes);
}

/* ═══════════════════════════════════════════════
 * Fitness & Summary
 * ═══════════════════════════════════════════════ */

static void save_fitness(const char *path, Portfolio *portfolios, int n_genomes) {
    /* Save as simple binary: array of doubles */
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[money_loop] Cannot write %s\n", path); return; }
    for (int i = 0; i < n_genomes; i++) {
        double pnl = portfolios[i].total_pnl;
        fwrite(&pnl, sizeof(double), 1, f);
    }
    fclose(f);
}

static void save_summary(const char *path, Portfolio *portfolios, int n_genomes) {
    json_t *root = json_object();

    double sum_pnl = 0, min_pnl = 1e18, max_pnl = -1e18;
    int n_active = 0, n_profitable = 0;
    for (int i = 0; i < n_genomes; i++) {
        Portfolio *p = &portfolios[i];
        if (p->total_trades > 0 || p->cash != SEED_CAPITAL) {
            sum_pnl += p->total_pnl;
            if (p->total_pnl < min_pnl) min_pnl = p->total_pnl;
            if (p->total_pnl > max_pnl) max_pnl = p->total_pnl;
            if (p->total_pnl > 0) n_profitable++;
            n_active++;
        }
    }

    json_object_set_new(root, "mean_pnl", json_real(n_active > 0 ? sum_pnl / n_active : 0));
    json_object_set_new(root, "max_pnl", json_real(max_pnl > -1e17 ? max_pnl : 0));
    json_object_set_new(root, "min_pnl", json_real(min_pnl < 1e17 ? min_pnl : 0));
    json_object_set_new(root, "n_active", json_integer(n_active));
    json_object_set_new(root, "n_profitable", json_integer(n_profitable));
    json_object_set_new(root, "updated_at", json_real((double)time(NULL)));

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    json_dump_file(root, tmp, JSON_INDENT(2));
    rename(tmp, path);
    json_decref(root);
}

/* ═══════════════════════════════════════════════
 * Heartbeat
 * ═══════════════════════════════════════════════ */

static void write_heartbeat(void) {
    char path[256];
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/infra/heartbeats/money-loop.heartbeat");
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%ld", (long)time(NULL)); fclose(f); }
}

/* ═══════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════ */

int main(int argc, char **argv) {
    srand(time(NULL));
    fprintf(stderr, "[money_loop] === 10K Genome Ecosystem (C) ===\n");

    /* 1. Init SQLite eco DB */
    init_eco_db();

    /* 2. Load gene pool (heap — 440KB) */
    Genome *pool = calloc(N_GENOMES, sizeof(Genome));
    if (!pool) { fprintf(stderr, "Out of memory\n"); return 1; }
    int n_genomes = load_gene_pool(GENE_POOL_PATH, pool, N_GENOMES);
    if (n_genomes == 0) {
        /* Generate synthetic genome pool */
        fprintf(stderr, "[money_loop] Generating %d synthetic genomes\n", N_GENOMES);
        n_genomes = N_GENOMES;
        for (int i = 0; i < n_genomes; i++) {
            for (int j = 0; j < N_PARAMS; j++) {
                pool[i][j] = (float)rand() / RAND_MAX;
            }
        }
    }

    /* 3. Load portfolios (heap — 480KB) */
    Portfolio *portfolios = calloc(N_GENOMES, sizeof(Portfolio));
    if (!portfolios) { free(pool); return 1; }
    load_portfolios(PORTFOLIOS_PATH, portfolios, n_genomes);

    /* 4. Load open trades (heap — up to 20MB) */
    int max_trades = 100000;
    Trade *all_trades = calloc(max_trades, sizeof(Trade));
    if (!all_trades) { free(pool); free(portfolios); return 1; }
    int n_trades = 0;
    int per_genome_count[N_GENOMES];
    load_trades(TRADES_PATH, all_trades, &n_trades, per_genome_count);

    /* 5. Load market feed */
    MarketState mkt;
    load_market_feed(&mkt);
    fprintf(stderr, "[money_loop] Market: BTC close=$%.2f volume=%.0f pump=%.3f\n",
            mkt.close, mkt.volume, mkt.pump_score);

    /* 6. Resolve old trades */
    int trade_id_counter = (int)(time(NULL) * 1000) % 1000000;
    double net_pnl = 0;
    int resolved = resolve_trades(all_trades, &n_trades, &mkt, portfolios, &net_pnl);
    if (resolved > 0) {
        fprintf(stderr, "[money_loop] Resolved %d trades, net_pnl=$%.2f\n", resolved, net_pnl);
    }

    /* 7. Generate new trades from genomes */
    int trades_opened = 0;
    for (int i = 0; i < n_genomes; i++) {
        char gid_str[16];
        snprintf(gid_str, sizeof(gid_str), "%d", i);

        /* Skip if already max open trades */
        if (per_genome_count[i] >= MAX_OPEN_TRADES_PER_AGENT) continue;

        Portfolio *pf = &portfolios[i];
        if (pf->cash < 1.0) continue;  /* Bankrupt */

        Signal sig = compute_signal(&pool[i], &mkt, pf);
        if (sig.direction != 0) {
            int opened = execute_trade(all_trades, &n_trades, &trade_id_counter,
                                       i, &sig, &mkt, portfolios,
                                       (double)pool[i][IDX_STOP_LOSS_PCT],
                                       (double)pool[i][IDX_TAKE_PROFIT_PCT]);
            if (opened) {
                per_genome_count[i]++;
                trades_opened++;
            }
        }
    }

    if (trades_opened > 0) {
        fprintf(stderr, "[money_loop] Opened %d new trades\n", trades_opened);
    }

    /* 8. Compute open trade count */
    int open_count = 0;
    for (int i = 0; i < n_trades; i++) {
        if (all_trades[i].status == 0) open_count++;
    }

    /* 9. Compute total trades across all portfolios */
    int total_trades = 0;
    for (int i = 0; i < n_genomes; i++) {
        total_trades += portfolios[i].total_trades;
    }

    /* 10. Save state */
    save_portfolios(PORTFOLIOS_PATH, portfolios, n_genomes);
    save_trades(TRADES_PATH, all_trades, n_trades);
    save_fitness(FITNESS_PATH, portfolios, n_genomes);
    save_summary(SUMMARY_PATH, portfolios, n_genomes);

    /* 11. Log snapshot */
    log_snapshot(portfolios, n_genomes, total_trades, open_count, resolved, 0, net_pnl);

    /* 12. Heartbeat */
    write_heartbeat();

    if (eco_db) sqlite3_close(eco_db);

    free(pool);
    free(portfolios);
    free(all_trades);

    fprintf(stderr, "[money_loop] Done. %d portfolios, %d open trades, %d resolved\n",
            n_genomes, open_count, resolved);
    return 0;
}
