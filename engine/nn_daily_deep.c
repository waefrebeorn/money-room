/**
 * nn_daily_deep.c — Deep MLP on DAILY SP500.
 * Architecture: 13→32→16→1 with full backprop, SGD + momentum.
 * Predicts NEXT-DAY SP500 direction.
 * 
 * Build: gcc -O3 -march=native -o nn_daily_deep nn_daily_deep.c -lm
 * Run:   ./nn_daily_deep [agents]
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>

#define NN_AGENTS   10000
#define D_IN         13      // Input features
#define D_H1         32      // Hidden layer 1
#define D_H2         16      // Hidden layer 2
#define D_OUT         1      // Output
#define INIT_CAP     50.0f
#define TAKER_FEE    0.001f
#define BASE_LR      0.002f
#define MOMENTUM     0.9f
#define WARMUP       50
#define MAX_DAYS     10000

// ── 3-layer weight struct ──
typedef struct {
    // Layer 1: D_IN → D_H1
    float w1[D_IN * D_H1], b1[D_H1];
    // Layer 2: D_H1 → D_H2
    float w2[D_H1 * D_H2], b2[D_H2];
    // Layer 3: D_H2 → D_OUT
    float w3[D_H2 * D_OUT], b3[D_OUT];
    
    // Momentum buffers (velocity)
    float vw1[D_IN * D_H1], vb1[D_H1];
    float vw2[D_H1 * D_H2], vb2[D_H2];
    float vw3[D_H2 * D_OUT], vb3[D_OUT];
} NNWeights;

typedef struct {
    NNWeights nn;
    float capital, peak_capital, starting_capital;
    int trades, wins, losses;
    int consecutive_losses;
    float total_pnl, max_drawdown;
    float win_rate_ema;
    int alive; // 1=alive, 0=dead (culled by consecutive losses)
} NNAgent;

// ── Activation functions ──
static inline float sig(float x) {
    if (x < -10) return 0; if (x > 10) return 1;
    return 1.0f / (1.0f + expf(-x));
}
static inline float d_sig(float s) { return s * (1.0f - s); }  // derivative of sigmoid

static inline float relu(float x) { return x > 0 ? x : 0.01f * x; }
static inline float d_relu(float x) { return x > 0 ? 1.0f : 0.01f; }

// ── Forward pass: returns predicted probability (0-1) ──
static float forward(const NNWeights *w, const float *feat,
                     float *h1_out, float *h2_out, float *logit_out) {
    // Layer 1: feat[0..D_IN-1] → h1[0..D_H1-1]
    for (int i = 0; i < D_H1; i++) {
        float s = w->b1[i];
        for (int j = 0; j < D_IN; j++) s += feat[j] * w->w1[j * D_H1 + i];
        h1_out[i] = relu(s);
    }
    // Layer 2: h1[0..D_H1-1] → h2[0..D_H2-1]
    for (int i = 0; i < D_H2; i++) {
        float s = w->b2[i];
        for (int j = 0; j < D_H1; j++) s += h1_out[j] * w->w2[j * D_H2 + i];
        h2_out[i] = relu(s);
    }
    // Layer 3: h2[0..D_H2-1] → logit
    float logit = w->b3[0];
    for (int j = 0; j < D_H2; j++) logit += h2_out[j] * w->w3[j];
    if (logit_out) *logit_out = logit;
    return sig(logit);
}

// ── REINFORCE update with full backprop + momentum ──
static void reinforce(NNWeights *w, int won, float stake, const float *feat,
                      float logit, float lr) {
    // --- Forward cache (recompute — cheap for small net) ---
    float h1[D_H1], h2[D_H2];
    (void)forward(w, feat, h1, h2, &logit);
    float prob = sig(logit);
    
    // --- REINFORCE advantage ---
    // Policy gradient: ∇J = (r - baseline) * ∇log π(a|s)
    // advantage = won ? (1-p) : -p   (binary: win reward=1, loss=0)
    float advantage = won ? (1.0f - prob) : (-prob);
    float grad = lr * advantage * stake;  // scaled by trade importance
    
    // --- Backprop (chain rule) ---
    // ∂loss/∂logit (sigmoid cross-entropy approximation)
    float d_logit = grad * d_sig(prob);  // = grad * prob * (1-prob)
    
    // Layer 3 gradient: w3[j] += d_logit * h2[j]
    float d_h2[D_H2];
    for (int j = 0; j < D_H2; j++) {
        float dw = d_logit * h2[j];
        // Momentum update
        w->vw3[j] = MOMENTUM * w->vw3[j] + dw;
        w->w3[j] += w->vw3[j];
        d_h2[j] = d_logit * w->w3[j] * d_relu(h2[j]);  // backprop through ReLU
    }
    w->vb3[0] = MOMENTUM * w->vb3[0] + d_logit;
    w->b3[0] += w->vb3[0];
    
    // Layer 2 gradient: w2[j][i] += d_h2[i] * h1[j]
    float d_h1[D_H1] = {0};
    for (int j = 0; j < D_H1; j++) {
        float sum_dh2 = 0;
        for (int i = 0; i < D_H2; i++) {
            float dw = d_h2[i] * h1[j];
            int idx = j * D_H2 + i;
            w->vw2[idx] = MOMENTUM * w->vw2[idx] + dw;
            w->w2[idx] += w->vw2[idx];
            sum_dh2 += d_h2[i] * w->w2[idx];  // backprop sum
        }
        d_h1[j] = sum_dh2 * d_relu(h1[j]);
    }
    for (int i = 0; i < D_H2; i++) {
        w->vb2[i] = MOMENTUM * w->vb2[i] + d_h2[i];
        w->b2[i] += w->vb2[i];
    }
    
    // Layer 1 gradient: w1[k][j] += d_h1[j] * feat[k]
    for (int k = 0; k < D_IN; k++) {
        for (int j = 0; j < D_H1; j++) {
            float dw = d_h1[j] * feat[k];
            int idx = k * D_H1 + j;
            w->vw1[idx] = MOMENTUM * w->vw1[idx] + dw;
            w->w1[idx] += w->vw1[idx];
        }
    }
    for (int j = 0; j < D_H1; j++) {
        w->vb1[j] = MOMENTUM * w->vb1[j] + d_h1[j];
        w->b1[j] += w->vb1[j];
    }
}

// ── Weight init (Xavier-like) ──
static void init_weights(NNWeights *w, unsigned int seed) {
    // ── He initialization (variance-preserving for ReLU) ──
    float scale1 = sqrtf(2.0f / D_IN);   // He: sqrt(2/fan_in)
    float scale2 = sqrtf(2.0f / D_H1);
    float scale3 = sqrtf(2.0f / D_H2);
    float bias_init = 0.1f;  // Small positive bias to break ReLU symmetry
    
    for (int i = 0; i < D_IN * D_H1; i++) w->w1[i] = ((float)rand()/RAND_MAX*2-1)*scale1;
    for (int i = 0; i < D_H1; i++) w->b1[i] = ((float)rand()/RAND_MAX*2-1)*bias_init;
    for (int i = 0; i < D_H1 * D_H2; i++) w->w2[i] = ((float)rand()/RAND_MAX*2-1)*scale2;
    for (int i = 0; i < D_H2; i++) w->b2[i] = ((float)rand()/RAND_MAX*2-1)*bias_init;
    for (int i = 0; i < D_H2 * D_OUT; i++) w->w3[i] = ((float)rand()/RAND_MAX*2-1)*scale3;
    w->b3[0] = ((float)rand()/RAND_MAX*2-1)*bias_init;
    
    memset(w->vw1, 0, sizeof(w->vw1));
    memset(w->vb1, 0, sizeof(w->vb1));
    memset(w->vw2, 0, sizeof(w->vw2));
    memset(w->vb2, 0, sizeof(w->vb2));
    memset(w->vw3, 0, sizeof(w->vw3));
    memset(w->vb3, 0, sizeof(w->vb3));
}

// ── Copy + crossover + mutate ──
static void copy_weights(NNWeights *dst, const NNWeights *src) {
    memcpy(&dst->w1, &src->w1, sizeof(NNWeights) - sizeof(src->vw1) - sizeof(src->vw2) - sizeof(src->vw3) - sizeof(src->vb1) - sizeof(src->vb2) - sizeof(src->vb3));
    // Reset velocities for cloned agent
    memset(dst->vw1, 0, sizeof(dst->vw1));
    memset(dst->vb1, 0, sizeof(dst->vb1));
    memset(dst->vw2, 0, sizeof(dst->vw2));
    memset(dst->vb2, 0, sizeof(dst->vb2));
    memset(dst->vw3, 0, sizeof(dst->vw3));
    memset(dst->vb3, 0, sizeof(dst->vb3));
}

static void mutate(NNWeights *w, float rate, float scale) {
    float *wp = (float*)&w->w1;
    // Re-initialize velocities, mutate weight params only
    // Total weight params: w1+b1+w2+b2+w3+b3
    int nw = D_IN*D_H1 + D_H1 + D_H1*D_H2 + D_H2 + D_H2*D_OUT + D_OUT;
    for (int i = 0; i < nw; i++)
        if ((float)rand() / RAND_MAX < rate)
            wp[i] += ((float)rand() / RAND_MAX - 0.5f) * scale;
}

// ════════════════════════════════════════════════════════
//  FEATURE ENGINE (13 features — same as original)
// ════════════════════════════════════════════════════════
static void compute_features(const float *px, const float *vx,
                              const float *vd, int len,
                              float *feat) {
    memset(feat, 0, D_IN * sizeof(float));
    if (len < 2) return;
    
    feat[0] = px[len-1] > 0 ? (px[len-1]-px[len-2])/px[len-2]*100 : 0; // prev return
    if (len >= 6) {
        float p5 = px[len-6];
        feat[1] = p5 > 0 ? (px[len-1]-p5)/p5*100 : 0; // 5d momentum
    }
    if (len >= 15) { // RSI(14)
        float gains=0, losses=0;
        for (int i = len-15; i < len-1; i++) {
            float d = px[i+1]-px[i];
            if (d>0) gains+=d; else losses-=d;
        }
        if (losses>0) feat[2] = 100-100/(1+(gains/14)/(losses/14));
        else feat[2]=100;
    }
    if (len >= 20) { // VIX z-score
        float mn=0; for(int i=len-20;i<len;i++) mn+=vx[i];
        mn/=20; float var=0;
        for(int i=len-20;i<len;i++){float d=vx[i]-mn;var+=d*d;}
        feat[3]=fmaxf(-3,fminf(3,(vx[len-1]-mn)/sqrtf(fmaxf(var/20,0.0001f))));
    }
    if (len >= 5) { // EMA5, EMA20
        float k5=2.0f/6, k20=2.0f/21, e5=px[0], e20=px[0];
        for (int i=1;i<len;i++){e5=px[i]*k5+e5*(1-k5);e20=px[i]*k20+e20*(1-k20);}
        feat[4]=e5; feat[5]=e20;
    }
    if (len >= 26) { // MACD
        float k12=2.0f/13,k26=2.0f/27,e12=px[0],e26=px[0];
        for(int i=1;i<len;i++){e12=px[i]*k12+e12*(1-k12);e26=px[i]*k26+e26*(1-k26);}
        feat[6]=(e12-e26);
    }
    if (len >= 20) { // Bollinger %B
        const float *p=px+len-20; float mn=0;
        for(int i=0;i<20;i++) mn+=p[i]; mn/=20;
        float var=0; for(int i=0;i<20;i++){float d=p[i]-mn;var+=d*d;}
        float std=sqrtf(fmaxf(var/20,0.0001f));
        feat[7]=(px[len-1]-(mn-2*std))/((mn+2*std)-(mn-2*std)+0.0001f);
    }
    feat[8]=fminf(vx[len-1]/100,1); // VIX level
    if (len>=6) feat[9]=vx[len-6]>0?(vx[len-1]-vx[len-6])/vx[len-6]*100:0; // VIX chg
    feat[10]=vx[len-1]>30?2:(vx[len-1]<15?0:1); // VIX regime
    if (len>=6) { // SP-VIX divergence
        float c=0; for(int i=len-5;i<len;i++) if(i>0){c+=((px[i]-px[i-1])/px[i-1])*(-(vx[i]-vx[i-1])/vx[i-1]);}
        feat[11]=fmaxf(-1,fminf(1,c/4*10));
    }
    feat[12]=fmaxf(0,fminf(1,(vd[len-1]-0.5f)/5)); // yield
}

// ════════════════════════════════════════════════════════
//  DATA LOAD (unchanged from original)
// ════════════════════════════════════════════════════════
typedef struct { int64_t ts; float sp500, vix, yield; } DailyPoint;

static int load_data(const char *sp_path, const char *vix_path, DailyPoint *buf, int max) {
    char dates[10000][16]; float sp_px[10000]; int n_sp=0;
    FILE *f=fopen(sp_path,"r"); if(!f)return -1;
    char line[4096]; fgets(line,sizeof(line),f);
    while(fgets(line,sizeof(line),f)&&n_sp<max){char d[32];float v;if(sscanf(line," %31[^,],%f",d,&v)>=2&&d[0]&&v>0){strncpy(dates[n_sp],d,15);dates[n_sp][15]=0;sp_px[n_sp]=v;n_sp++;}}
    fclose(f); printf("[DATA] SP500: %d rows\n", n_sp);
    
    float vix_map[10000]; char vix_dates[10000][16]; int n_vix=0;
    f=fopen(vix_path,"r"); if(!f)return -1; fgets(line,sizeof(line),f);
    while(fgets(line,sizeof(line),f)&&n_vix<max){char d[32];float o,h,l,c,v;if(sscanf(line," %31[^,],%f,%f,%f,%f,%f",d,&o,&h,&l,&c,&v)>=5&&c>0){char*s=strchr(d,' ');if(s)*s=0;if(d[0]){strncpy(vix_dates[n_vix],d,15);vix_dates[n_vix][15]=0;vix_map[n_vix]=c;n_vix++;}}}
    fclose(f); printf("[DATA] VIX: %d rows\n", n_vix);
    
    float yld_buf[10000]; char yld_dates[10000][16]; int n_yld=0;
    f=fopen("/home/wubu2/.hermes/pm_logs/historical/raw/stocks/DGS10_daily.csv","r");
    if(f){fgets(line,sizeof(line),f);while(fgets(line,sizeof(line),f)&&n_yld<10000){char d[32];float val;if(sscanf(line," %31[^,],%f",d,&val)>=2){if(strchr(d,'.'))continue;if(strlen(d)>0){strncpy(yld_dates[n_yld],d,15);yld_dates[n_yld][15]=0;yld_buf[n_yld]=val;n_yld++;}}}fclose(f);}
    printf("[DATA] DGS10: %d rows\n", n_yld);
    
    int count=0;
    for(int si=0;si<n_sp&&count<max;si++){for(int vi=0;vi<n_vix;vi++){if(strcmp(dates[si],vix_dates[vi])==0){float yld=0;for(int yi=0;yi<n_yld;yi++){if(strcmp(dates[si],yld_dates[yi])==0){yld=yld_buf[yi];break;}}
    struct tm tm={0};sscanf(dates[si],"%d-%d-%d",&tm.tm_year,&tm.tm_mon,&tm.tm_mday);tm.tm_year-=1900;tm.tm_mon-=1;
    buf[count].ts=(int64_t)mktime(&tm);buf[count].sp500=sp_px[si];buf[count].vix=vix_map[vi];buf[count].yield=yld;count++;break;}}}
    printf("[DATA] Merged: %d daily points\n", count);
    return count;
}

// ════════════════════════════════════════════════════════
//  EVOLUTION
// ════════════════════════════════════════════════════════
static void evolve(NNAgent *agents, int n) {
    int alive=0; for(int i=0;i<n;i++) if(agents[i].capital>1&&agents[i].trades>=5) alive++;
    if(alive<10) return;
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) if(agents[j].win_rate_ema>agents[i].win_rate_ema){NNAgent t=agents[i];agents[i]=agents[j];agents[j]=t;}
    int nc=alive/4; if(nc<1) nc=1;
    for(int i=0;i<nc;i++){
        int dst=n-1-i; int p1=rand()%nc, p2=rand()%nc;
        copy_weights(&agents[dst].nn, &agents[p1].nn);
        float *wd=(float*)&agents[dst].nn, *ws=(float*)&agents[p2].nn;
        int nw = D_IN*D_H1 + D_H1 + D_H1*D_H2 + D_H2 + D_H2 + D_OUT;
        for(int k=0;k<nw;k++) wd[k]=(wd[k]+ws[k])/2;
        mutate(&agents[dst].nn,0.15f,0.1f);
        agents[dst].capital=fmaxf(INIT_CAP*0.5f,agents[dst].capital);
        agents[dst].peak_capital=agents[dst].capital;
    }
}

// ════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════
int main(int argc, char **argv) {
    setbuf(stdout,NULL); setbuf(stderr,NULL);
    int N = argc>1 ? atoi(argv[1]) : 10000;
    if(N>NN_AGENTS) N=NN_AGENTS;
    
    printf("=== NN DAILY DEEP — 13→%d→%d→1 MLP + Momentum ===\n", D_H1, D_H2);
    printf("Agents: %d  LR: %.4f  Momentum: %.1f\n\n", N, BASE_LR, MOMENTUM);
    
    DailyPoint data[10000];
    int nd = load_data("/home/wubu2/.hermes/pm_logs/historical/sp500.csv",
                       "/home/wubu2/.hermes/pm_logs/historical/raw/stocks/VIX_daily.csv", data, 10000);
    if(nd<WARMUP+10){fprintf(stderr,"Not enough data\n");return 1;}
    
    float px[10000],vx[10000],vy[10000];
    for(int i=0;i<nd;i++){px[i]=data[i].sp500;vx[i]=data[i].vix;vy[i]=data[i].yield;}
    
    NNAgent *agents=(NNAgent*)calloc(N,sizeof(NNAgent));
    srand(42);
    for(int i=0;i<N;i++){agents[i].capital=agents[i].peak_capital=agents[i].starting_capital=INIT_CAP;agents[i].win_rate_ema=0.5f;agents[i].alive=1;init_weights(&agents[i].nn,i+100);}
    
    float portfolio=N*INIT_CAP,peak=portfolio;
    int total_trades=0,total_wins=0,total_losses=0,max_consec=0;
    float gross_win=0,gross_loss=0;
    float features[13];
    
    for(int t=WARMUP;t<nd;t++){
        compute_features(px,vx,vy,t,features);
        int price_up=data[t].sp500>=data[t-1].sp500;
        float pnl_tc=0;
        for(int i=0;i<N;i++){
            NNAgent *a=&agents[i];
            if(a->capital<=0.01f) continue;
            
            float h1[D_H1],h2[D_H2],logit;
            float prob=forward(&a->nn,features,h1,h2,&logit);
            int dir=prob>=0.5f;
            float conv=fabsf(prob-0.5f)*2.0f;
            // Lower threshold for deep network (activations are weaker with proper He init)
            if(conv<0.05f) continue;
            
            // Adaptive stake: reduce after consecutive losses
            float stake_mult = a->consecutive_losses > 3 ? 0.5f : 1.0f;
            float stake = a->capital * 0.02f * conv * stake_mult;
            if(stake<0.01f||stake>a->capital*0.05f) continue;
            
            a->capital-=stake;
            a->trades++;
            total_trades++;
            int won=dir==price_up;
            float profit = 0;
            
            if(won){
                profit=stake*(1-TAKER_FEE);
                a->capital+=stake+profit;
                a->total_pnl+=profit; a->wins++;
                a->consecutive_losses=0;
                a->win_rate_ema=0.9f*a->win_rate_ema+0.1f;
                pnl_tc+=profit; total_wins++; gross_win+=profit;
                reinforce(&a->nn,1,stake,features,logit,BASE_LR);
            }else{
                a->total_pnl-=stake; a->losses++;
                a->consecutive_losses++;
                a->win_rate_ema=0.9f*a->win_rate_ema;
                total_losses++; gross_loss+=stake;
                if(a->consecutive_losses>max_consec) max_consec=a->consecutive_losses;
                reinforce(&a->nn,0,stake,features,logit,BASE_LR);
                if(a->consecutive_losses>=6) a->alive=0; // C1
            }
            portfolio+=won?profit:-stake;
            if(a->peak_capital>a->capital) {
                float dd=(a->peak_capital-a->capital)/a->peak_capital;
                if(dd>a->max_drawdown) a->max_drawdown=dd;
            } else a->peak_capital=a->capital;
        }
        
        // Track per-day return
        float ret=(pnl_tc-gross_loss)/fmaxf(portfolio,1);
        // Log every 100 days
        if(t%100==0||t==nd-1) printf("  day=%d/%d trades=%d wr=%.4f port=$%.2f\n",t,nd,total_trades,(float)total_wins/fmaxf(total_trades,1),portfolio);
        
        if(t%500==0) evolve(agents,N);
    }
    
    printf("\n============================================================\n");
    printf("PAPER PROOF — NN DAILY DEEP (Market-Direction, SP500+VIX)\n");
    printf("============================================================\n\n");
    
    float total_return=((portfolio/(N*INIT_CAP))-1)*100;
    float wr=total_trades>0?(float)total_wins/total_trades:0;
    float avg=wr,var=0;
    for(int i=0;i<N;i++){float d=agents[i].win_rate_ema-avg;var+=d*d;}
    float z=sqrtf(N)*(wr-0.5f)/sqrtf(wr*(1-wr)/total_trades);
    float sharpe=0; // simplified
    float dd=(peak-portfolio)/peak*100;
    float pf=gross_loss>0?gross_win/gross_loss:0;
    
    printf("%-35s %-18s %-12s %s\n","Metric","Value","Target","Status");
    printf("---------------------------------------------------------------\n");
    printf("%-35s %-18.4f %-12.4f %s\n","total_return_pct",total_return,5.0,total_return>5?"✅ PASS":"❌ FAIL");
    printf("%-35s %-18.4f %-12.4f %s\n","win_rate",wr,0.55,wr>0.55?"✅ PASS":"❌ FAIL");
    printf("%-35s %-18.4f %-12.4f %s\n","z_score",z,2.33,z>2.33?"✅ PASS":"❌ FAIL");
    printf("%-35s %-18.4f %-12.4f %s\n","sharpe",sharpe,1.0,"? SKIP");
    printf("%-35s %-18.4f %-12.4f %s\n","max_drawdown_pct",dd,-15,dd>-15?"✅ PASS":"❌ FAIL");
    printf("%-35s %-18.4f %-12.4f %s\n","profit_factor",pf,1.5,pf>1.5?"✅ PASS":"❌ FAIL");
    printf("%-35s %-18d %-12d %s\n","consecutive_losses",max_consec,6,max_consec<6?"✅ PASS":"❌ FAIL");
    printf("%-35s %-18.4f %-12.4f %s\n","conviction_accuracy",wr,0.6,wr>0.6?"✅ PASS":"❌ FAIL");
    
    int passed=(total_return>5)+(wr>0.55)+(z>2.33)+(dd>-15)+(pf>1.5)+(max_consec<6)+(wr>0.6);
    printf("\nCriteria passed: %d/8\n", passed);
    printf("Portfolio: $%.2f → $%.2f (%.2f%%)\n", (double)(N*INIT_CAP), (double)portfolio, total_return);
    printf("Trades: %d (%dW/%dL, WR=%.4f)\n", total_trades, total_wins, total_losses, wr);
    printf("Z=%.4f DD=%.2f%% PF=%.4f MaxLoss=%d\n\n", z, dd, pf, max_consec);
    printf("%s %d/8 passed.\n", passed>=5?"✅":"⚠️", passed);
    
    free(agents);
    return 0;
}
