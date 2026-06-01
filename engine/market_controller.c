/*
 * market_controller.c — C port of pm_market_controller.py
 *
 * Q-controller (tabular Q-learning) + market dynamics enrichment.
 * Used by feed_bridge.c for market_feed.json enrichment.
 *
 * Build: gcc -O3 -march=native -c market_controller.c -ljansson -lsqlite3 -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <jansson.h>
#include <sqlite3.h>

#define HOME_DIR        "/home/wubu2"
#define Q_STATE_PATH    HOME_DIR "/.hermes/pm_logs/c_room/q_controller_state.json"
#define N_ACTIONS       5
#define Q_LR            0.1
#define Q_GAMMA         0.9
#define EXPLORE_INIT    0.3
#define EXPLORE_MIN     0.05
#define EXPLORE_DECAY   0.9999

/* ─── Q-table ─── */
typedef struct {
    json_t *q_table;    /* dict: state_str → [q_val_0..q_val_4] */
    double exploration;
    double total_reward;
    int steps;
} QController;

/* ─── Market state ─── */
typedef struct {
    int regime;         /* 0=ranging, 1=bear, 2=bull */
    double volatility;
    double sentiment;   /* -1.0 to 1.0 */
} MarketState;

/* ─── Enrichment output ─── */
typedef struct {
    int regime;
    double volatility;
    double sentiment;
    double pid_p, pid_i, pid_d;
    double momentum;
    double strategy_weights[5];
    double nested_prediction;
} MarketEnrichment;

/* ─── Load Q-controller state from JSON ─── */
static void qctrl_load(QController *qc) {
    memset(qc, 0, sizeof(*qc));
    qc->exploration = EXPLORE_INIT;

    json_t *root = json_load_file(Q_STATE_PATH, 0, NULL);
    if (!root) {
        qc->q_table = json_object();
        return;
    }

    qc->q_table = json_object_get(root, "q_table");
    if (qc->q_table) json_incref(qc->q_table);
    else {
        qc->q_table = json_object();
    }

    json_t *v = json_object_get(root, "exploration");
    qc->exploration = (v && json_is_real(v)) ? json_real_value(v) : EXPLORE_INIT;
    v = json_object_get(root, "total_reward");
    qc->total_reward = (v && json_is_real(v)) ? json_real_value(v) : 0;
    v = json_object_get(root, "steps");
    qc->steps = (v && json_is_integer(v)) ? json_integer_value(v) : 0;

    json_decref(root);
}

/* ─── Save Q-controller state ─── */
static void qctrl_save(QController *qc) {
    json_t *root = json_object();
    json_object_set(root, "q_table", qc->q_table);
    json_object_set_new(root, "exploration", json_real(qc->exploration));
    json_object_set_new(root, "total_reward", json_real(qc->total_reward));
    json_object_set_new(root, "steps", json_integer(qc->steps));
    json_object_set_new(root, "updated_at", json_real((double)time(NULL)));

    json_dump_file(root, Q_STATE_PATH, JSON_INDENT(2));
    json_decref(root);
}

/* ─── Discretize market state → integer key ─── */
static int discretize_state(MarketState *ms) {
    int vol_bucket = (int)(ms->volatility * 10);
    if (vol_bucket < 0) vol_bucket = 0;
    if (vol_bucket > 9) vol_bucket = 9;

    int sent_bucket = (int)((ms->sentiment + 1.0) * 5);
    if (sent_bucket < 0) sent_bucket = 0;
    if (sent_bucket > 9) sent_bucket = 9;

    return ms->regime * 100 + vol_bucket * 10 + sent_bucket;
}

/* ─── Get Q-values for a state ─── */
static double *get_q_values(QController *qc, int state) {
    char key[16];
    snprintf(key, sizeof(key), "%d", state);

    json_t *vals = json_object_get(qc->q_table, key);
    if (!vals) {
        /* Initialize with zeros */
        vals = json_array();
        for (int i = 0; i < N_ACTIONS; i++)
            json_array_append_new(vals, json_real(0.0));
        json_object_set_new(qc->q_table, key, vals);
    }

    /* Return as static buffer */
    static double qbuf[N_ACTIONS];
    for (int i = 0; i < N_ACTIONS; i++) {
        json_t *v = json_array_get(vals, i);
        qbuf[i] = (v && json_is_real(v)) ? json_real_value(v) : 0.0;
    }
    return qbuf;
}

/* ─── ε-greedy action selection ─── */
static int choose_action(QController *qc, MarketState *ms) {
    int state = discretize_state(ms);
    double *q_vals = get_q_values(qc, state);

    if ((double)rand() / RAND_MAX < qc->exploration) {
        return rand() % N_ACTIONS;
    }

    /* Greedy: choose action with max Q-value */
    int best_action = 0;
    double max_q = q_vals[0];
    for (int i = 1; i < N_ACTIONS; i++) {
        if (q_vals[i] > max_q) {
            max_q = q_vals[i];
            best_action = i;
        }
    }

    qc->exploration = fmax(EXPLORE_MIN, qc->exploration * EXPLORE_DECAY);
    return best_action;
}

/* ─── Q-learning update: Q(s,a) += lr * (reward + gamma * max Q(s') - Q(s,a)) ─── */
static double apply_reward(QController *qc, MarketState *ms, int action, double reward) {
    int state = discretize_state(ms);
    double *q_vals = get_q_values(qc, state);

    /* Find max Q for next state (same state since we don't have next state) */
    double max_q_next = q_vals[0];
    for (int i = 1; i < N_ACTIONS; i++) {
        if (q_vals[i] > max_q_next) max_q_next = q_vals[i];
    }

    double old_q = q_vals[action];
    double td_target = reward + Q_GAMMA * max_q_next;
    double td_error = td_target - old_q;
    double new_q = old_q + Q_LR * td_error;

    /* Update the saved Q-value */
    char key[16];
    snprintf(key, sizeof(key), "%d", state);
    json_t *vals = json_object_get(qc->q_table, key);
    if (vals) {
        json_array_set_new(vals, action, json_real(new_q));
    }

    qc->total_reward += reward;
    qc->steps++;
    qctrl_save(qc);

    return new_q;
}

/* ─── Compute market enrichment from candles ─── */
static MarketEnrichment compute_enrichment(double *closes, int n_candles, QController *qc) {
    MarketEnrichment me = {0};
    me.nested_prediction = 0.5;

    if (n_candles < 2) {
        me.volatility = 0.02;
        return me;
    }

    /* Volatility (20-period) */
    int n_ret = n_candles - 1;
    if (n_ret > 20) n_ret = 20;
    double sum_sq = 0;
    for (int i = 1; i <= n_ret && i < n_candles; i++) {
        double ret = (closes[i-1] - closes[i]) / closes[i];
        sum_sq += ret * ret;
    }
    me.volatility = sqrt(sum_sq / n_ret);

    /* Regime detection (10 vs 20 period) */
    int n_recent = (n_candles < 10) ? n_candles : 10;
    int n_older  = (n_candles < 20) ? n_candles / 2 : 10;
    double recent_sum = 0, older_sum = 0;
    for (int i = 0; i < n_recent; i++) recent_sum += closes[i];
    for (int i = n_recent; i < n_recent + n_older && i < n_candles; i++) older_sum += closes[i];

    if (n_recent > 0 && n_older > 0) {
        double recent_avg = recent_sum / n_recent;
        double older_avg  = older_sum / n_older;
        double trend = (recent_avg - older_avg) / older_avg;
        if (trend > 0.02)      me.regime = 2;  /* Bull */
        else if (trend < -0.02) me.regime = 1; /* Bear */
        else                     me.regime = 0; /* Ranging */
    }

    /* Sentiment */
    double ret = (closes[0] - closes[n_candles-1]) / closes[n_candles-1];
    me.sentiment = fmax(-1.0, fmin(1.0, ret * 10));

    /* PID signals */
    me.pid_p = ret * 100;
    double avg5 = 0;
    int n5 = (n_candles < 5) ? n_candles : 5;
    for (int i = 0; i < n5; i++) avg5 += closes[i];
    avg5 /= n5;
    me.pid_i = avg5 / closes[n_candles-1] - 1;
    me.pid_d = (n_candles > 1) ? (closes[0] - closes[1]) / closes[1] * 100 : 0;

    /* Momentum sentiment index */
    me.momentum = ret * 100;

    /* Strategy weights via Q-controller */
    MarketState ms = {me.regime, me.volatility, me.sentiment};
    int action = choose_action(qc, &ms);
    double strategy_map[5][5] = {
        {0.4, 0.2, 0.1, 0.1, 0.2},  /* aggro */
        {0.3, 0.3, 0.1, 0.1, 0.2},  /* momentum */
        {0.1, 0.1, 0.4, 0.2, 0.2},  /* reversion */
        {0.1, 0.1, 0.1, 0.5, 0.2},  /* defensive */
        {0.2, 0.2, 0.2, 0.2, 0.2},  /* neutral */
    };
    for (int i = 0; i < 5; i++)
        me.strategy_weights[i] = strategy_map[action][i];

    return me;
}

/* ─── CLI: apply_reward (reads from market_feed.json) ─── */
static int cli_apply_reward(void) {
    char path[256];
    snprintf(path, sizeof(path), HOME_DIR "/.hermes/pm_logs/c_room/market_feed.json");

    json_t *feed = json_load_file(path, 0, NULL);
    if (!feed) {
        fprintf(stderr, "[qctrl] Cannot load market_feed.json\n");
        return 1;
    }

    MarketState ms;
    json_t *v;
    v = json_object_get(feed, "market_regime");
    ms.regime = (v && json_is_integer(v)) ? json_integer_value(v) : 1;
    v = json_object_get(feed, "market_volatility");
    ms.volatility = (v && json_is_real(v)) ? json_real_value(v) : 0.02;

    /* Sentiment from sentiment_indexes */
    v = json_object_get(feed, "sentiment_indexes");
    if (v) {
        json_t *fg = json_object_get(v, "fear_greed_signal");
        ms.sentiment = (fg && json_is_real(fg)) ? json_real_value(fg) : 0;
    } else {
        ms.sentiment = 0;
    }

    v = json_object_get(feed, "q_action");
    int action = (v && json_is_integer(v)) ? json_integer_value(v) : 0;
    v = json_object_get(feed, "q_reward");
    double reward = (v && json_is_real(v)) ? json_real_value(v) : 0.0;

    json_decref(feed);

    QController qc;
    qctrl_load(&qc);
    double new_q = apply_reward(&qc, &ms, action, reward);
    printf("[qctrl] Q-update: state(%d,%.4f,%.4f) action=%d reward=%.4f Q=%.4f\n",
           ms.regime, ms.volatility, ms.sentiment, action, reward, new_q);
    json_decref(qc.q_table);
    return 0;
}

/* ─── CLI: enrich (compute market dynamics for feed_bridge) ─── */
static int cli_enrich(void) {
    /* Load candles from timeline.db */
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT json_extract(data, '$.close') as c FROM timeline "
        "WHERE source='kraken_btc' AND json_extract(data, '$.close') IS NOT NULL "
        "ORDER BY ts DESC LIMIT 120");

    /* Use sqlite3 to query */
    sqlite3 *db = NULL;
    double closes[120];
    int n = 0;
    if (sqlite3_open(HOME_DIR "/.hermes/pm_logs/timeline.db", &db) == SQLITE_OK) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW && n < 120) {
                closes[n++] = sqlite3_column_double(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    /* If no kraken_btc in timeline, try bitstamp_1min */
    if (n < 2) {
        snprintf(sql, sizeof(sql),
            "SELECT json_extract(data, '$.close') as c FROM timeline "
            "WHERE source='bitstamp_1min' AND json_extract(data, '$.pair')='BTC' "
            "AND json_extract(data, '$.close') IS NOT NULL "
            "ORDER BY ts DESC LIMIT 120");
        if (sqlite3_open(HOME_DIR "/.hermes/pm_logs/timeline.db", &db) == SQLITE_OK) {
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW && n < 120) {
                    closes[n++] = sqlite3_column_double(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }
    }

    QController qc;
    qctrl_load(&qc);
    MarketEnrichment me = compute_enrichment(closes, n, &qc);

    json_t *out = json_object();
    json_object_set_new(out, "regime", json_integer(me.regime));
    json_object_set_new(out, "volatility", json_real(me.volatility));
    json_object_set_new(out, "sentiment", json_real(me.sentiment));
    json_object_set_new(out, "nested_prediction", json_real(me.nested_prediction));

    json_t *pid = json_object();
    json_object_set_new(pid, "p", json_real(me.pid_p));
    json_object_set_new(pid, "i", json_real(me.pid_i));
    json_object_set_new(pid, "d", json_real(me.pid_d));
    json_object_set_new(pid, "signal", json_real(me.pid_p + me.pid_i * 100 + me.pid_d));
    json_object_set_new(out, "pid_signals", pid);

    json_t *sent = json_object();
    json_object_set_new(sent, "momentum", json_real(me.momentum));
    json_object_set_new(sent, "volume_trend", json_real(0.0));
    json_object_set_new(sent, "divergence", json_real(0.0));
    json_object_set_new(out, "sentiment_indexes", sent);

    json_t *weights = json_array();
    for (int i = 0; i < 5; i++)
        json_array_append_new(weights, json_real(me.strategy_weights[i]));
    json_object_set_new(out, "strategy_weights", weights);

    char *dump = json_dumps(out, JSON_INDENT(2));
    printf("%s\n", dump);
    free(dump);
    json_decref(out);
    json_decref(qc.q_table);

    return 0;
}

/* ─── Main ─── */
int main(int argc, char **argv) {
    srand(time(NULL));

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command>\n", argv[0]);
        fprintf(stderr, "  apply_reward  — read market_feed.json, do Q-update\n");
        fprintf(stderr, "  enrich        — compute market dynamics from timeline.db\n");
        return 1;
    }

    if (strcmp(argv[1], "apply_reward") == 0) return cli_apply_reward();
    if (strcmp(argv[1], "enrich") == 0) return cli_enrich();

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
