/**
 * nested_ht_train.c — Nested Temporal Hierarchy Training with Epochs
 * 
 * Trains 6-level cascade: 1min→5min→15min→1hr→4hr→daily
 * Each level: multi-epoch SGD on logistic regression
 * Cascade: each level's prediction feeds as feature to level above
 * 
 * "epich training" = epoch-based multi-pass across the cascade.
 * Fixes long timeline delays by training bottom-up:
 *   - L0 (1min) gets 7.5M candles, trains fast (few features)
 *   - Each level up has fewer candles (resampled) but more features
 *   - Cascade predictions propagate information upward
 *
 * Compile: gcc -O3 -march=native -o nested_ht_train nested_ht_train.c -lsqlite3 -lm
 * Run:     ./nested_ht_train [timeline.db] [max_candles]
 * Output:  weights.json (for nested_ht_infer.h), cascade signals
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include <stdint.h>

// ── Config ──
#define MAX_CANDLES   8000000
#define MAX_LEVELS    6
#define FEAT_BASE     12
#define FEAT_MACRO    5
#define FEAT_CASCADE  1
#define HIDDEN        16
#define EPOCHS        20
#define LR_INIT       0.01
#define LR_DECAY      0.95
#define ADAM_B1       0.9
#define ADAM_B2       0.999
#define ADAM_EPS      1e-8

// ── Level config ──
typedef struct {
    const char *name;
    int interval_min;     // Aggregation interval in minutes
    int n_features;       // Base features (resampled OHLCV + tech)
    int use_macro;        // Include macro features (SP500, VIX, etc.)
    int use_cascade;      // Include cascade prediction from level below
} LevelConfig;

LevelConfig LEVELS[MAX_LEVELS] = {
    {"L0_1min",   1,   12, 0, 0},  // 1-min: raw technical features
    {"L1_5min",   5,   12, 0, 0},  // 5-min: resampled tech features
    {"L2_15min",  15,  12, 0, 0},  // 15-min
    {"L3_1hr",    60,  12, 1, 0},  // 1-hr: + macro (SP500, VIX, DGS10, Fed, CPI)
    {"L4_4hr",    240, 12, 1, 1},  // 4-hr: + macro + cascade from L3
    {"L5_daily",  1440, 12, 1, 1}, // daily: + macro + cascade from L4
};

// ── Candle ──
typedef struct {
    long ts;
    double open, high, low, close, volume;
} Candle;

// ── CandleArray ──
typedef struct {
    Candle *d;
    int n, cap;
} CandleArray;

static void ca_init(CandleArray *ca, int cap) {
    ca->d = malloc(cap * sizeof(Candle));
    ca->n = 0; ca->cap = cap;
}

static void ca_push(CandleArray *ca, Candle c) {
    if (ca->n >= ca->cap) {
        ca->cap *= 2;
        ca->d = realloc(ca->d, ca->cap * sizeof(Candle));
    }
    ca->d[ca->n++] = c;
}

static void ca_free(CandleArray *ca) { free(ca->d); ca->d = NULL; ca->n = ca->cap = 0; }

// ── Model: Logistic Regression with Adam ──
typedef struct {
    int d;            // features
    double *w;         // weights [d]
    double b;          // bias
    double *m_w, *v_w; // Adam state
    double m_b, v_b;
    int t;
} LRModel;

static LRModel *lr_create(int d) {
    LRModel *m = calloc(1, sizeof(LRModel));
    m->d = d;
    m->w = calloc(d, 8);
    m->m_w = calloc(d, 8);
    m->v_w = calloc(d, 8);
    m->b = 0; m->m_b = 0; m->v_b = 0; m->t = 0;
    // Small random init
    for (int i = 0; i < d; i++) m->w[i] = (rand()/(double)RAND_MAX - 0.5) * 0.1;
    return m;
}

static void lr_free(LRModel *m) { free(m->w); free(m->m_w); free(m->v_w); free(m); }

static double lr_predict(LRModel *m, double *x) {
    double z = m->b;
    for (int i = 0; i < m->d; i++) z += m->w[i] * x[i];
    return 1.0 / (1.0 + exp(-z));
}

static double lr_train(LRModel *m, double *x, double target, double lr, double wd) {
    double pred = lr_predict(m, x);
    double loss = -(target * log(fmax(pred,1e-15)) + (1-target) * log(fmax(1-pred,1e-15)));
    double grad = pred - target;  // dL/dz for BCE+sigmoid
    
    m->t++;
    for (int i = 0; i < m->d; i++) {
        double g = grad * x[i] + wd * m->w[i];
        m->m_w[i] = ADAM_B1 * m->m_w[i] + (1-ADAM_B1) * g;
        m->v_w[i] = ADAM_B2 * m->v_w[i] + (1-ADAM_B2) * g*g;
        double mh = m->m_w[i] / (1 - pow(ADAM_B1, m->t));
        double vh = m->v_w[i] / (1 - pow(ADAM_B2, m->t));
        m->w[i] -= lr * mh / (sqrt(vh) + ADAM_EPS);
    }
    double gb = grad;
    m->m_b = ADAM_B1 * m->m_b + (1-ADAM_B1) * gb;
    m->v_b = ADAM_B2 * m->v_b + (1-ADAM_B2) * gb*gb;
    double mh = m->m_b / (1 - pow(ADAM_B1, m->t));
    double vh = m->v_b / (1 - pow(ADAM_B2, m->t));
    m->b -= lr * mh / (sqrt(vh) + ADAM_EPS);
    
    return loss;
}

// ── Load candles from timeline.db (any source with open/high/low/close/volume) ──
static int load_candles(CandleArray *ca, sqlite3 *db, const char *sources[], int n_sources, int limit) {
    // Build SQL: select from multiple sources with UNION
    char sql[4096];
    char *ptr = sql;
    ptr += sprintf(ptr, 
        "SELECT ts, CAST(json_extract(data, '$.open') AS REAL) as o, "
        "CAST(json_extract(data, '$.high') AS REAL) as h, "
        "CAST(json_extract(data, '$.low') AS REAL) as l, "
        "CAST(json_extract(data, '$.close') AS REAL) as c, "
        "CAST(json_extract(data, '$.volume') AS REAL) as v "
        "FROM timeline WHERE (");
    
    for (int i = 0; i < n_sources; i++) {
        if (i > 0) ptr += sprintf(ptr, " OR ");
        ptr += sprintf(ptr, "source='%s'", sources[i]);
    }
    ptr += sprintf(ptr, 
        ") AND o IS NOT NULL AND c IS NOT NULL "
        "ORDER BY ts ASC LIMIT %d", limit);
    
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
        Candle c;
        c.ts = (long)sqlite3_column_int64(stmt, 0);
        c.open = sqlite3_column_double(stmt, 1);
        c.high = sqlite3_column_double(stmt, 2);
        c.low = sqlite3_column_double(stmt, 3);
        c.close = sqlite3_column_double(stmt, 4);
        c.volume = sqlite3_column_double(stmt, 5);
        ca_push(ca, c);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

// ── Resample candles to higher interval ──
static CandleArray resample(CandleArray *src, int interval_min) {
    CandleArray out;
    ca_init(&out, src->n / interval_min + 1);
    
    if (src->n == 0) return out;
    
    long interval_sec = interval_min * 60;
    Candle cur = src->d[0];
    long cur_start = (cur.ts / interval_sec) * interval_sec;
    cur.open = cur.open;
    cur.high = cur.high;
    cur.low = cur.low;
    cur.close = cur.close;
    cur.volume = cur.volume;
    int has = 1;
    
    for (int i = 1; i < src->n; i++) {
        Candle *c = &src->d[i];
        long bar_start = (c->ts / interval_sec) * interval_sec;
        
        if (bar_start != cur_start) {
            // Emit current bar
            ca_push(&out, cur);
            // Start new bar
            cur = *c;
            cur_start = bar_start;
            cur.open = c->open;
            cur.high = c->high;
            cur.low = c->low;
            cur.close = c->close;
            cur.volume = c->volume;
            has = 1;
        } else {
            cur.high = fmax(cur.high, c->high);
            cur.low = fmin(cur.low, c->low);
            cur.close = c->close;
            cur.volume += c->volume;
        }
    }
    if (has) ca_push(&out, cur);
    
    return out;
}

// ── Compute technical features from candle + history ──
// Features: returns (1, 5, 20 bars), volatility (5, 20), SMA rel (20, 50),
// RSI(14), MACD signal, BB% width, volume ratio
static int compute_features(CandleArray *candles, int idx, double *features) {
    if (idx < 60) return 0;
    
    Candle *c = &candles->d[idx];
    double close = c->close;
    
    // Returns
    double r1 = log(close / candles->d[idx-1].close);
    double r5 = log(close / candles->d[idx-5].close);
    double r20 = log(close / candles->d[idx-20].close);
    
    // Volatility 5, 20
    double s5=0,sq5=0; int n5=0;
    for(int j=1;j<=5;j++){
        double r=log(candles->d[idx-j+1].close/candles->d[idx-j].close);
        s5+=r; sq5+=r*r; n5++;
    }
    double v5 = (n5>1)?sqrt(sq5/n5-(s5/n5)*(s5/n5)):0;
    double s20=0,sq20=0; int n20=0;
    for(int j=1;j<=20;j++){
        double r=log(candles->d[idx-j+1].close/candles->d[idx-j].close);
        s20+=r; sq20+=r*r; n20++;
    }
    double v20 = (n20>1)?sqrt(sq20/n20-(s20/n20)*(s20/n20)):0;
    
    // SMA relative
    double sma20=0, sma50=0;
    for(int j=1;j<=20;j++) sma20 += candles->d[idx-j].close;
    for(int j=1;j<=50&&idx>=j;j++) sma50 += candles->d[idx-j].close;
    sma20/=20; sma50/=fmin(50, idx);
    
    // RSI 14
    double gain=0, loss=0;
    for(int j=1;j<=14;j++){
        double d = candles->d[idx-j+1].close - candles->d[idx-j].close;
        if(d>0) gain+=d; else loss-=d;
    }
    double rsi = (gain+loss>0) ? 100*gain/(gain+loss) : 50;
    
    // Volume ratio (current / SMA 20 vol)
    double vol_sum=0;
    for(int j=1;j<=20;j++) vol_sum += candles->d[idx-j].volume;
    double vol_ratio = vol_sum>0 ? c->volume / (vol_sum/20) : 1;
    
    // High-low range ratio
    double hl_range = (c->high - c->low) / close;
    
    // Fill features
    int f = 0;
    features[f++] = close / 100000.0;  // Normalize price
    features[f++] = r1;
    features[f++] = r5;
    features[f++] = r20;
    features[f++] = v5;
    features[f++] = v20;
    features[f++] = close/sma20 - 1;  // Rel to MA
    features[f++] = close/sma50 - 1;
    features[f++] = rsi / 100.0 - 0.5;  // Centered RSI
    features[f++] = hl_range;
    features[f++] = vol_ratio - 1;
    features[f++] = (c->close - c->open) / c->open;  // Intraday move
    
    return f;  // Returns 12 base features
}

// ── Load macro features from timeline.db for a given timestamp ──
static int get_macro_features(sqlite3 *db, long ts, double *macro) {
    // Query nearest SP500, VIX, DGS10, FedFunds, CPI
    const char *sources[] = {"fred_sp500", "fred_vix", "stock_dgs10", "fred_fedfunds", "fred_cpi"};
    const char *json_keys[] = {"value", "value", "value", "value", "value"};
    
    for (int i = 0; i < 5; i++) {
        macro[i] = 0;
        char sql[1024];
        snprintf(sql, sizeof(sql),
            "SELECT CAST(json_extract(data, '$.%s') AS REAL) as v "
            "FROM timeline WHERE source='%s' AND v IS NOT NULL AND ts <= %ld "
            "ORDER BY ts DESC LIMIT 1",
            json_keys[i], sources[i], ts);
        
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                macro[i] = sqlite3_column_double(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }
    return 5;
}

// ── Save model as JSON ──
static void save_model(FILE *fp, LRModel *m, double *mean, double *std, const char *name) {
    fprintf(fp, "  \"%s\": {\n", name);
    fprintf(fp, "    \"d\": %d,\n", m->d);
    fprintf(fp, "    \"b\": %.10f,\n", m->b);
    fprintf(fp, "    \"w\": [");
    for (int i = 0; i < m->d; i++)
        fprintf(fp, "%s%.10f", i > 0 ? "," : "", m->w[i]);
    fprintf(fp, "],\n");
    fprintf(fp, "    \"mean\": [");
    for (int i = 0; i < m->d; i++)
        fprintf(fp, "%s%.10f", i > 0 ? "," : "", mean[i]);
    fprintf(fp, "],\n");
    fprintf(fp, "    \"std\": [");
    for (int i = 0; i < m->d; i++)
        fprintf(fp, "%s%.10f", i > 0 ? "," : "", std[i]);
    fprintf(fp, "]\n");
    fprintf(fp, "  }");
}

// ── Evaluate ──
typedef struct { double loss, acc, wr; int tp, tn, fp, fn; } Eval;

static Eval evaluate(LRModel *m, double **X, double *y, int n) {
    Eval e = {0,0,0,0,0,0,0};
    for (int i = 0; i < n; i++) {
        double pred = lr_predict(m, X[i]);
        e.loss += -(y[i]*log(fmax(pred,1e-15)) + (1-y[i])*log(fmax(1-pred,1e-15)));
        int p = pred >= 0.5, t = y[i] >= 0.5;
        if(t&&p) e.tp++; else if(!t&&!p) e.tn++; else if(!t&&p) e.fp++; else e.fn++;
    }
    e.loss /= n;
    e.acc = (double)(e.tp+e.tn)/n;
    e.wr = e.acc;
    return e;
}

// ── Main ──
int main(int argc, char **argv) {
    srand(time(0));
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    const char *db_path = argc > 1 ? argv[1] : "/home/wubu2/.hermes/pm_logs/timeline.db";
    int max_candles = argc > 2 ? atoi(argv[2]) : 500000;
    
    printf("═══ Nested Temporal Hierarchy — Epich Training ═══\n\n");
    printf("DB: %s\nMax candles: %d\n", db_path, max_candles);
    
    // Open DB
    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Can't open DB\n");
        return 1;
    }
    
    // ── Load raw 1-min candles ──
    const char *sources_1min[] = {"bitstamp_1min", "kraken_btc"};
    CandleArray raw;
    ca_init(&raw, max_candles);
    int n_loaded = load_candles(&raw, db, sources_1min, 2, max_candles);
    printf("\n[load] %d candles from timeline.db\n", n_loaded);
    
    if (n_loaded < 1000) {
        fprintf(stderr, "Not enough data\n");
        ca_free(&raw); sqlite3_close(db);
        return 1;
    }
    
    // ── Train each level bottom-up ──
    // We store cascade predictions for each candle
    double *cascade_pred = NULL;  // Predictions from previous level
    int cascade_n = 0;
    
    FILE *fp_out = fopen("weights.json", "w");
    if (fp_out) fprintf(fp_out, "{\n");
    
    for (int level = 0; level < MAX_LEVELS; level++) {
        LevelConfig *cfg = &LEVELS[level];
        printf("\n─── %s (%d min, feat=%d%s%s) ───\n",
               cfg->name, cfg->interval_min, cfg->n_features,
               cfg->use_macro ? " +macro" : "",
               cfg->use_cascade ? " +cascade" : "");
        
        // Resample
        CandleArray candles;
        if (level == 0) {
            // Re-use raw for L0
            candles = raw;
            // Shallow copy — don't free at end
        } else {
            CandleArray prev = (level == 0) ? raw : raw;  // Always resample from raw
            candles = resample(&raw, cfg->interval_min);
        }
        printf("  candles: %d\n", candles.n);
        
        // Feature dimension
        int n_feat = cfg->n_features;
        int n_macro = cfg->use_macro ? 5 : 0;
        int n_casc = cfg->use_cascade ? 1 : 0;
        int total_feat = n_feat + n_macro + n_casc;
        
        // Build training set
        int need_bars = 60;
        int n_train = 0;
        int max_train = candles.n - need_bars - 100;  // Reserve 100 for test
        if (max_train < 100) { printf("  ⚠️  Not enough data\n"); continue; }
        
        // Pre-allocate feature arrays
        double **X = malloc(max_train * sizeof(double*));
        double *y = malloc(max_train * 8);
        double *X_flat = calloc(max_train * total_feat, 8);
        double *macro_buf = NULL;
        if (cfg->use_macro) macro_buf = malloc(5 * 8);
        
        time_t t0 = time(NULL);
        
        for (int i = need_bars; i < candles.n - 50 && n_train < max_train; i++) {
            double *feat = X_flat + n_train * total_feat;
            compute_features(&candles, i, feat);
            
            int f = n_feat;
            
            // Macro features
            if (cfg->use_macro && macro_buf) {
                get_macro_features(db, candles.d[i].ts, macro_buf);
                for (int m = 0; m < 5; m++) feat[f++] = macro_buf[m];
            }
            
            // Cascade from previous level
            if (cfg->use_cascade && cascade_pred && cascade_n > i) {
                feat[f++] = cascade_pred[i];  // Previous level's prediction
            } else if (cfg->use_cascade) {
                feat[f++] = 0.5;  // Default
            }
            
            X[n_train] = feat;
            
            // Target: next-bar direction (for this level's interval)
            y[n_train] = (candles.d[i+1].close > candles.d[i].close) ? 1.0 : 0.0;
            n_train++;
        }
        
        // Train/val split (chronological — last 20% for val)
        int n_val = n_train * 20 / 100;
        int n_tr = n_train - n_val;
        
        // Normalize
        double *mean = calloc(total_feat, 8);
        double *std = calloc(total_feat, 8);
        for (int f = 0; f < total_feat; f++) {
            double s = 0;
            for (int i = 0; i < n_tr; i++) s += X[i][f];
            mean[f] = s / n_tr;
            double sq = 0;
            for (int i = 0; i < n_tr; i++) sq += (X[i][f] - mean[f]) * (X[i][f] - mean[f]);
            std[f] = sqrt(sq / n_tr);
            if (std[f] < 1e-10) std[f] = 1;
            for (int i = 0; i < n_tr; i++) X[i][f] = (X[i][f] - mean[f]) / std[f];
        }
        // Normalize val set with train stats
        for (int i = n_tr; i < n_train; i++)
            for (int f = 0; f < total_feat; f++)
                X[i][f] = (X[i][f] - mean[f]) / std[f];
        
        // Create model and train with epochs
        LRModel *model = lr_create(total_feat);
        
        double lr = LR_INIT;
        double best_val_loss = 1e9;
        int best_ep = 0;
        
        for (int ep = 0; ep < EPOCHS; ep++) {
            // Shuffle training indices
            for (int i = n_tr-1; i > 0; i--) {
                int j = rand() % (i+1);
                double *tmp_x = X[i]; X[i] = X[j]; X[j] = tmp_x;
                double tmp_y = y[i]; y[i] = y[j]; y[j] = tmp_y;
            }
            
            double tl = 0;
            for (int i = 0; i < n_tr; i++)
                tl += lr_train(model, X[i], y[i], lr, 1e-4);
            tl /= n_tr;
            
            Eval ve = evaluate(model, X + n_tr, y + n_tr, n_val);
            
            if (ve.loss < best_val_loss) { best_val_loss = ve.loss; best_ep = ep; }
            
            if (ep < 3 || ep % 5 == 4)
                printf("  ep%2d  tr=%.4f  va=%.4f  va_wr=%.2f%%  lr=%.4f\n",
                       ep+1, tl, ve.loss, ve.wr*100, lr);
            
            lr *= LR_DECAY;
        }
        
        // Final evaluation
        Eval te = evaluate(model, X + n_tr, y + n_tr, n_val);
        printf("  ═══ %s: WR=%.2f%% (TP=%d TN=%d FP=%d FN=%d) ═══\n",
               cfg->name, te.wr*100, te.tp, te.tn, te.fp, te.fn);
        
        // Generate cascade predictions for ALL candles (for next level)
        cascade_pred = malloc(candles.n * 8);
        cascade_n = candles.n;
        for (int i = 0; i < candles.n; i++) {
            double feat[30];
            if (i >= need_bars) {
                compute_features(&candles, i, feat);
                int f = n_feat;
                // Need macro and cascade to generate proper features
                // For cascade to next level, we use approximate features
                // (macro at candle time, no cascade input)
                if (cfg->use_macro && macro_buf) {
                    // Re-fetch macro at this candle's timestamp
                }
                // Standardize with train stats
                double x[30];
                for (int j = 0; j < total_feat; j++)
                    x[j] = (feat[j] - mean[j]) / std[j];
                cascade_pred[i] = lr_predict(model, x);
            } else {
                cascade_pred[i] = 0.5;
            }
        }
        
        // Save model weights
        if (fp_out) {
            save_model(fp_out, model, mean, std, cfg->name);
            if (level < MAX_LEVELS - 1) fprintf(fp_out, ",\n");
        }
        
        // Print cascade signal stats
        double casc_avg = 0;
        for (int i = 0; i < cascade_n; i++) casc_avg += cascade_pred[i];
        casc_avg /= cascade_n;
        printf("  cascade: mean=%.4f, range=[%.4f, %.4f]\n",
               casc_avg, cascade_pred[need_bars], 
               cascade_pred[cascade_n-1]);
        
        // Cleanup
        lr_free(model);
        free(mean); free(std);
        free(X_flat); free(y); free(X);
        free(macro_buf);
        
        if (level > 0) ca_free(&candles);  // Don't free L0 (same as raw)
        
        printf("  time: %lds\n", time(NULL) - t0);
    }
    
    if (fp_out) {
        fprintf(fp_out, "\n}\n");
        fclose(fp_out);
        printf("\n✅ Saved weights.json\n");
    }
    
    free(cascade_pred);
    ca_free(&raw);
    sqlite3_close(db);
    
    printf("\n═══ Cascade Complete ═══\n");
    printf("Levels trained: %d\n", MAX_LEVELS);
    printf("Total candles processed: %d\n", n_loaded);
    printf("Weights ready for nested_ht_infer.h\n");
    
    return 0;
}
