/*
 * nested_ht_infer.h — Inference header for nested trained models.
 * Loads trained weights and runs prediction on live candle data.
 * 
 * Usage:
 *   #include "nested_ht_infer.h"
 *   NestedModelCollection *models = load_nested_weights("weights.json");
 *   double pred = nested_predict(models, 4, candle_features, cascade_pred);
 *
 * Integrates with C room engine for live inference.
 */

#ifndef NESTED_HT_INFER_H
#define NESTED_HT_INFER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ─── Config ─── */
#define MAX_FEATURES_NESTED 17    /* 12 base + 5 macro */
#define HIDDEN_NESTED       16

/* ─── LR Model ─── */
typedef struct {
    int d;            /* feature dimension */
    double *w;        /* weights [d] */
    double b;         /* bias */
    double *mean;     /* standardization mean [d] */
    double *std;      /* standardization std [d] */
} LRModel;

LRModel *lr_load(const char *json_str, int *offset);
double lr_predict_raw(LRModel *m, double *x);
double lr_predict(LRModel *m, double *x);

/* ─── MLP Model ─── */
typedef struct {
    int d, h;           /* input dim, hidden dim */
    double *W1, *b1;    /* [d*h], [h] */
    double *W2;         /* [h] */
    double b2;          /* scalar */
    double *mean, *std; /* standardization [d] */
} MLPModel;

MLPModel *mlp_load(const char *json_str, int *offset);
double mlp_predict_raw(MLPModel *m, double *x);
double mlp_predict(MLPModel *m, double *x);

/* ─── Nested Model Collection ─── */
typedef struct {
    int n_levels;
    LRModel **lr_models;     /* [n_levels] */
    MLPModel **mlp_models;   /* [n_levels] */
    int *res_minutes;        /* [n_levels] */
    char **res_names;        /* [n_levels] */
    double *cascade_buffer;  /* reusable prediction buffer */
} NestedModelCollection;

NestedModelCollection *load_nested_weights(const char *path);
void nested_free(NestedModelCollection *c);
double nested_predict(NestedModelCollection *c, int level, double *features, double cascade_in);
void standardize_x(double *x, double *mean, double *std, int d);

/* ─── Feature computation (inline for speed) ─── */
static inline int compute_features_nested(
    double *feats,               /* output [12] */
    double open, double high, double low, double close, double volume,
    double prev_close, double prev_volume,
    int idx, double cascade_pred)
{
    if(idx < 10) return 0;
    
    /* Returns: 1,3,5,10,20 periods — would need price history buffer */
    /* For live use, caller should maintain a ring buffer of prices */
    
    feats[0] = 0; /* will be computed by caller with ring buffer */
    feats[1] = 0;
    feats[2] = 0;
    feats[3] = 0;
    feats[4] = 0;
    feats[5] = 0; /* volatility */
    feats[6] = (close > 0.001) ? (high - low) / close : 0.0; /* HL range */
    feats[7] = 1.0; /* volume ratio */
    feats[8] = (prev_volume > 0.001) ? volume / prev_volume : 1.0; /* vol momentum */
    feats[9] = (high - low > 0.001) ? (close - low) / (high - low) : 0.5; /* pos in range */
    feats[10] = (prev_close > 0.001) ? (open - prev_close) / prev_close : 0.0; /* gap */
    feats[11] = cascade_pred;
    
    return 1;
}

/* ═══════════════════════════════════════════
 *  IMPLEMENTATION
 * ═══════════════════════════════════════════ */

double sigmoid_n(double z) {
    if(z < -100) return 0;
    if(z > 100) return 1;
    return 1.0 / (1.0 + exp(-z));
}

void standardize_x(double *x, double *mean, double *std, int d) {
    for(int i=0; i<d; i++) {
        if(std[i] > 1e-10)
            x[i] = (x[i] - mean[i]) / std[i];
    }
}

/* ─── LR ─── */
double lr_predict_raw(LRModel *m, double *x) {
    double z = m->b;
    for(int i=0; i<m->d; i++) z += m->w[i] * x[i];
    return sigmoid_n(z);
}

double lr_predict(LRModel *m, double *x) {
    double *xs = malloc(m->d * sizeof(double));
    memcpy(xs, x, m->d * sizeof(double));
    standardize_x(xs, m->mean, m->std, m->d);
    double result = lr_predict_raw(m, xs);
    free(xs);
    return result;
}

LRModel *lr_load(const char *json, int *offset) {
    /* Format: {"d":N,"b":B,"w":[W0,W1,...],"mean":[...],"std":[...]} */
    LRModel *m = calloc(1, sizeof(LRModel));
    
    const char *p = json + *offset;
    /* Skip to "d" */
    p = strstr(p, "\"d\"");
    if(!p) goto fail;
    p = strchr(p, ':') + 1;
    m->d = atoi(p);
    
    p = strstr(p, "\"b\"");
    if(!p) goto fail;
    p = strchr(p, ':') + 1;
    m->b = atof(p);
    
    /* Parse weight array */
    p = strstr(p, "\"w\":[");
    if(!p) goto fail;
    p += 5;
    m->w = calloc(m->d, sizeof(double));
    for(int i=0; i<m->d; i++) {
        m->w[i] = atof(p);
        p = strchr(p, ',');
        if(!p && i < m->d-1) goto fail;
        if(p) p++;
    }
    
    /* Parse mean */
    p = strstr(p, "\"mean\":[");
    if(!p) goto fail;
    p += 8;
    m->mean = calloc(m->d, sizeof(double));
    for(int i=0; i<m->d; i++) {
        m->mean[i] = atof(p);
        p = strchr(p, ',');
        if(p) p++;
    }
    
    /* Parse std */
    p = strstr(p, "\"std\":[");
    if(!p) goto fail;
    p += 7;
    m->std = calloc(m->d, sizeof(double));
    for(int i=0; i<m->d; i++) {
        m->std[i] = atof(p);
        p = strchr(p, ',');
        if(!p && i < m->d-1) goto fail;
        if(p) p++;
    }
    
    *offset = p - json;
    return m;
    
fail:
    fprintf(stderr, "LR load failed at offset %d\n", *offset);
    free(m->w); free(m); return NULL;
}

/* ─── MLP ─── */
double mlp_predict_raw(MLPModel *m, double *x) {
    /* Hidden layer */
    double *h = calloc(m->h, sizeof(double));
    for(int j=0; j<m->h; j++) {
        double z = m->b1[j];
        for(int k=0; k<m->d; k++) z += x[k] * m->W1[k * m->h + j];
        h[j] = z > 0 ? z : 0; /* ReLU */
    }
    /* Output */
    double z = m->b2;
    for(int j=0; j<m->h; j++) z += h[j] * m->W2[j];
    free(h);
    return sigmoid_n(z);
}

double mlp_predict(MLPModel *m, double *x) {
    double *xs = malloc(m->d * sizeof(double));
    memcpy(xs, x, m->d * sizeof(double));
    standardize_x(xs, m->mean, m->std, m->d);
    double result = mlp_predict_raw(m, xs);
    free(xs);
    return result;
}

MLPModel *mlp_load(const char *json, int *offset) {
    MLPModel *m = calloc(1, sizeof(MLPModel));
    const char *p = json + *offset;
    
    p = strstr(p, "\"d\""); if(!p) goto fail;
    p = strchr(p, ':') + 1; m->d = atoi(p);
    
    p = strstr(p, "\"h\""); if(!p) goto fail;
    p = strchr(p, ':') + 1; m->h = atoi(p);
    
    p = strstr(p, "\"b2\""); if(!p) goto fail;
    p = strchr(p, ':') + 1; m->b2 = atof(p);
    
    m->W1 = calloc(m->d * m->h, sizeof(double));
    m->b1 = calloc(m->h, sizeof(double));
    m->W2 = calloc(m->h, sizeof(double));
    
    p = strstr(p, "\"W1\":["); if(!p) goto fail;
    p += 6;
    for(int i=0; i<m->d*m->h; i++) {
        m->W1[i] = atof(p); p = strchr(p,','); if(p) p++;
    }
    
    p = strstr(p, "\"b1\":["); if(!p) goto fail;
    p += 6;
    for(int i=0; i<m->h; i++) {
        m->b1[i] = atof(p); p = strchr(p,','); if(p) p++;
    }
    
    p = strstr(p, "\"W2\":["); if(!p) goto fail;
    p += 6;
    for(int i=0; i<m->h; i++) {
        m->W2[i] = atof(p); p = strchr(p,','); if(p) p++;
    }
    
    p = strstr(p, "\"mean\":["); if(!p) goto fail;
    p += 8;
    m->mean = calloc(m->d, sizeof(double));
    for(int i=0; i<m->d; i++) {
        m->mean[i] = atof(p); p = strchr(p,','); if(p) p++;
    }
    
    p = strstr(p, "\"std\":["); if(!p) goto fail;
    p += 7;
    m->std = calloc(m->d, sizeof(double));
    for(int i=0; i<m->d; i++) {
        m->std[i] = atof(p); p = strchr(p,','); if(p) p++;
    }
    
    *offset = p - json;
    return m;
    
fail:
    fprintf(stderr, "MLP load failed at offset %d\n", *offset);
    return NULL;
}

/* ─── Nested Model Collection ─── */
NestedModelCollection *load_nested_weights(const char *path) {
    FILE *f = fopen(path, "r");
    if(!f) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(len + 1);
    fread(json, 1, len, f);
    json[len] = 0;
    fclose(f);
    
    NestedModelCollection *c = calloc(1, sizeof(NestedModelCollection));
    int offset = 0;
    const char *p = json;
    
    /* Count levels */
    p = strstr(p, "\"n_levels\"");
    if(!p) goto fail;
    p = strchr(p, ':') + 1;
    c->n_levels = atoi(p);
    
    c->lr_models = calloc(c->n_levels, sizeof(LRModel*));
    c->mlp_models = calloc(c->n_levels, sizeof(MLPModel*));
    c->res_minutes = calloc(c->n_levels, sizeof(int));
    c->res_names = calloc(c->n_levels, sizeof(char*));
    
    /* Load each level */
    for(int i=0; i<c->n_levels; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "\"level_%d\": {", i);
        const char *found = strstr(p, buf);
        if(!found) continue;
        p = found;
        
        /* Parse resolution info */
        const char *rp = strstr(p, "\"res_min\"");
        if(rp) { rp = strchr(rp, ':') + 1; c->res_minutes[i] = atoi(rp); }
        
        /* Skip "lr" block */
        const char *lp = strstr(p, "\"lr\": {");
        if(lp) {
            int lo = lp - json;
            c->lr_models[i] = lr_load(json, &lo);
        }
        
        /* Skip "mlp" block */
        const char *mp = strstr(p, "\"mlp\": {");
        if(mp) {
            int mo = mp - json;
            c->mlp_models[i] = mlp_load(json, &mo);
        }
    }
    
    c->cascade_buffer = calloc(4096, sizeof(double));
    free(json);
    return c;
    
fail:
    free(json); free(c);
    return NULL;
}

void nested_free(NestedModelCollection *c) {
    if(!c) return;
    for(int i=0; i<c->n_levels; i++) {
        if(c->lr_models[i]) { free(c->lr_models[i]->w); free(c->lr_models[i]->mean); free(c->lr_models[i]->std); free(c->lr_models[i]); }
        if(c->mlp_models[i]) {
            free(c->mlp_models[i]->W1); free(c->mlp_models[i]->b1);
            free(c->mlp_models[i]->W2); free(c->mlp_models[i]->mean); free(c->mlp_models[i]->std);
            free(c->mlp_models[i]);
        }
        free(c->res_names[i]);
    }
    free(c->lr_models); free(c->mlp_models);
    free(c->res_minutes); free(c->res_names);
    free(c->cascade_buffer);
    free(c);
}

double nested_predict(NestedModelCollection *c, int level, double *features, double cascade_in) {
    if(level >= c->n_levels || !c->mlp_models[level]) return 0.5;
    
    /* Inject cascade prediction as feature[11] */
    double feats[17];
    memcpy(feats, features, c->mlp_models[level]->d * sizeof(double));
    if(c->mlp_models[level]->d > 12)
        feats[11] = cascade_in; /* cascade feature slot */
    
    return mlp_predict(c->mlp_models[level], feats);
}

#endif /* NESTED_HT_INFER_H */
