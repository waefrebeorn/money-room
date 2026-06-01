/**
 * genome_distiller.c — Bridge paper training → live state v2
 * Uses offsetof from types.h for CORRECT struct layout.
 * 
 * Build: gcc -O2 -o genome_distiller genome_distiller.c -lm -I.
 * Usage: ./genome_distiller [--backup]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stddef.h>
#include <dirent.h>
#include "types.h"

#define PAPER_STATE "/home/wubu2/.hermes/pm_logs/c_room/room_state_paper.bin"
#define LIVE_STATE  "/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"
#define PAPER_AGENT_OFF 50172   /* offsetof(RoomState,agents) with PAPER_MODE */
#define LIVE_AGENT_OFF  200172  /* offsetof(RoomState,agents) LIVE mode */

typedef struct {
    int id;
    double capital;
    double win_rate;
    int trades;
    int wins;
    double params[11];
    double fitness;
} Agent;

static int cmp_agent(const void *a, const void *b) {
    const Agent *aa=(const Agent*)a, *bb=(const Agent*)b;
    if(aa->fitness>bb->fitness)return -1; if(aa->fitness<bb->fitness)return 1; return 0;
}

int main(int argc, char **argv) {
    srand(time(NULL)^getpid());
    int do_backup = 0;
    for(int i=1;i<argc;i++) if(strcmp(argv[i],"--backup")==0) do_backup=1;
    
    printf("\n  ╔══════════════════════════════════════════════╗\n");
    printf("  ║   GENOME DISTILLER v2 — Paper → Live        ║\n");
    printf("  ╚══════════════════════════════════════════════╝\n\n");
    
    printf("[DISTILLER] sizeof(Genome)=%zu sizeof(AgentState)=%zu\n", sizeof(Genome), sizeof(AgentState));
    printf("[DISTILLER] agent array offset=%zu sizeof(RoomState)=%zu\n", offsetof(RoomState, agents), sizeof(RoomState));
    
    /* ── Read paper state ── */
    int fd = open(PAPER_STATE, O_RDONLY);
    if(fd<0){perror("open paper");return 1;}
    struct stat st; fstat(fd,&st);
    size_t fsize = st.st_size;
    unsigned char *paper = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if(paper==MAP_FAILED){perror("mmap paper");close(fd);return 1;}
    close(fd);
    
    /* Validate */
    uint32_t magic; memcpy(&magic, paper + offsetof(RoomState,magic), 4);
    printf("[DISTILLER] Magic: 0x%08X (expected 0x%08X)\n", magic, STATE_MAGIC);
    
    int cycle; memcpy(&cycle, paper + offsetof(RoomState,cycle), 4);
    printf("[DISTILLER] Paper state: cycle=%d file_size=%zu\n", cycle, fsize);
    
    /* ── Extract agents from paper state ── */
    const int AGENT_OFF = PAPER_AGENT_OFF;  /* paper state layout */
    const int ASIZE = sizeof(AgentState);
    int max_extract = (fsize - AGENT_OFF) / ASIZE;
    if(max_extract > 2500) max_extract = 2500;
    
    Agent *agents = malloc(max_extract * sizeof(Agent));
    int n_agents = 0;
    
    for(int i=0; i<max_extract; i++) {
        int base = AGENT_OFF + i * ASIZE;
        if(base + (int)sizeof(AgentState) > (int)fsize) break;
        
        AgentState *as = (AgentState*)(paper + base);
        if(!as->alive) continue;
        if(as->trades <= 0) continue;  // no experience
        
        agents[n_agents].id = i;
        agents[n_agents].capital = as->capital;
        agents[n_agents].win_rate = as->win_rate_ema;
        agents[n_agents].trades = as->trades;
        agents[n_agents].wins = as->wins;
        
        /* Genome params */
        double *p = agents[n_agents].params;
        p[0] = as->genome.position_size;
        p[1] = as->genome.conviction_threshold;
        p[2] = as->genome.risk_tolerance;
        p[3] = as->genome.lie_sensitivity;
        p[4] = as->genome.herd_antipathy;
        p[5] = as->genome.stop_loss_pct;
        p[6] = as->genome.take_profit_pct;
        p[7] = as->genome.min_edge_pct;
        p[8] = as->genome.time_horizon;
        p[9] = as->genome.mean_reversion_bias;
        p[10] = as->genome.learning_rate;
        
        /* Fitness */
        agents[n_agents].fitness = as->capital + (as->win_rate_ema - 0.5) * 10000.0;
        n_agents++;
    }
    
    munmap(paper, fsize);
    printf("[DISTILLER] Found %d valid agents in paper state (out of %d slots)\n", n_agents, max_extract);
    
    if(n_agents == 0) {
        printf("  ⚠ No valid agents found in paper state (all went bankrupt).\n");
        printf("  ⚠ Falling back to multi_market trained genomes...\n");
        free(agents);
        
        // Fallback: load trained genomes from multi_market/*.bin
        const char *mm_dir = "/home/wubu2/money-room/data/multi_market";
        DIR *mm_d = opendir(mm_dir);
        if (!mm_d) {
            printf("  ❌ Cannot open %s for fallback.\n", mm_dir);
            return 1;
        }
        
        struct dirent *mm_e;
        Agent *fallback = malloc(max_extract * sizeof(Agent));
        n_agents = 0;
        
        while ((mm_e = readdir(mm_d)) != NULL && n_agents < max_extract) {
            size_t nlen = strlen(mm_e->d_name);
            if (nlen < 5 || strcmp(mm_e->d_name + nlen - 4, ".bin") != 0) continue;
            if (strcmp(mm_e->d_name, ".") == 0 || strcmp(mm_e->d_name, "..") == 0) continue;
            
            char mm_path[512];
            snprintf(mm_path, sizeof(mm_path), "%s/%s", mm_dir, mm_e->d_name);
            
            FILE *mm_f = fopen(mm_path, "rb");
            if (!mm_f) continue;
            
            // Strip .bin suffix for market name
            char mname[32];
            size_t nl = strlen(mm_e->d_name);
            memcpy(mname, mm_e->d_name, nl - 4);
            mname[nl - 4] = '\0';
            
            Genome g;
            int mtype = MARKET_CRYPTO;
            if (fread(&g, sizeof(Genome), 1, mm_f) == 1) {
                fread(&mtype, sizeof(int), 1, mm_f);
                if (mtype < 0 || mtype >= N_MARKET_TYPES) mtype = MARKET_CRYPTO;
                
                fallback[n_agents].id = n_agents;
                fallback[n_agents].capital = 50.0f;
                fallback[n_agents].win_rate = 0.5;
                fallback[n_agents].trades = 0;
                fallback[n_agents].wins = 0;
                double *p = fallback[n_agents].params;
                p[0] = g.position_size;
                p[1] = g.conviction_threshold;
                p[2] = g.risk_tolerance;
                p[3] = g.lie_sensitivity;
                p[4] = g.herd_antipathy;
                p[5] = g.stop_loss_pct;
                p[6] = g.take_profit_pct;
                p[7] = g.min_edge_pct;
                p[8] = g.time_horizon;
                p[9] = g.mean_reversion_bias;
                p[10] = g.learning_rate;
                fallback[n_agents].fitness = 50.0;
                n_agents++;
            }
            fclose(mm_f);
        }
        closedir(mm_d);
        
        if (n_agents == 0) {
            printf("  ❌ No trained genomes found in %s/*.bin\n", mm_dir);
            free(fallback);
            return 1;
        }
        
        printf("  ✅ Fallback: loaded %d trained genomes from multi_market/\n", n_agents);
        agents = fallback;
    }
    
    /* Sort by fitness */
    qsort(agents, n_agents, sizeof(Agent), cmp_agent);
    
    /* Show top 5 */
    printf("[DISTILLER] Top 5 agents:\n");
    for(int i=0;i<(n_agents<5?n_agents:5);i++)
        printf("  #%d: cap=$%.0f WR=%.1f%% trades=%d fitness=%.0f\n",
               i+1, agents[i].capital, agents[i].win_rate*100, agents[i].trades, agents[i].fitness);
    
    /* Show bottom 3 */
    printf("[DISTILLER] Bottom 3 agents:\n");
    for(int i=(n_agents>3?n_agents-3:0);i<n_agents;i++)
        printf("  #%d: cap=$%.0f WR=%.1f%% trades=%d fitness=%.0f\n",
               i+1, agents[i].capital, agents[i].win_rate*100, agents[i].trades, agents[i].fitness);
    
    /* ── Bridge to live state ── */
    if(do_backup){
        char buf[512]; snprintf(buf,sizeof(buf),"%s.bak_%ld",LIVE_STATE,time(NULL));
        FILE *s=fopen(LIVE_STATE,"r"); if(s){FILE*d=fopen(buf,"w");int c;if(d){while((c=fgetc(s))!=EOF)fputc(c,d);fclose(d);}fclose(s);}
        printf("[DISTILLER] ✅ Live state backed up\n");
    }
    
    fd = open(LIVE_STATE, O_RDWR);
    if(fd<0){perror("open live");free(agents);return 1;}
    struct stat lst; fstat(fd,&lst);
    unsigned char *live = mmap(NULL, lst.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(live==MAP_FAILED){perror("mmap live");close(fd);free(agents);return 1;}
    close(fd);
    
    int live_max = (lst.st_size - LIVE_AGENT_OFF) / ASIZE;
    if(live_max > 10000) live_max = 10000;
    printf("[DISTILLER] Live state: %d agent slots available\n", live_max);
    
    /* Copy top N paper agents into live state */
    int elite = n_agents<100 ? n_agents : 100;
    float start_cap = 50.0f;
    
    for(int i=0;i<live_max;i++) {
        int src_idx = i < n_agents ? i : (i - n_agents) % elite;
        int base = LIVE_AGENT_OFF + i * ASIZE;
        AgentState *live_as = (AgentState*)(live + base);
        
        /* Reset capital */
        live_as->capital = start_cap;
        live_as->starting_capital = start_cap;
        live_as->peak_capital = start_cap;
        live_as->trades = 0;
        live_as->wins = 0;
        live_as->losses = 0;
        live_as->total_pnl = 0;
        live_as->win_rate_ema = 0.5f;
        live_as->alive = true;
        
        /* Copy genome params with mutation for clones */
        Genome *g = &live_as->genome;
        float mut = (i >= n_agents) ? 0.05f : 0.0f;  // mutate clones
        
        g->position_size = fminf(fmaxf(agents[src_idx].params[0] + ((float)rand()/RAND_MAX-0.5)*2*mut, 0.01f), 0.50f);
        g->conviction_threshold = fminf(fmaxf(agents[src_idx].params[1] + ((float)rand()/RAND_MAX-0.5)*2*mut, 0.05f), 0.95f);
        g->risk_tolerance = fminf(fmaxf(agents[src_idx].params[2] + ((float)rand()/RAND_MAX-0.5)*2*mut, 0.0f), 1.0f);
        g->lie_sensitivity = fminf(fmaxf(agents[src_idx].params[3] + ((float)rand()/RAND_MAX-0.5)*2*mut, 0.1f), 0.98f);
        g->herd_antipathy = fminf(fmaxf(agents[src_idx].params[4] + ((float)rand()/RAND_MAX-0.5)*2*mut, 0.0f), 1.0f);
        g->stop_loss_pct = fminf(fmaxf(agents[src_idx].params[5] + ((float)rand()/RAND_MAX-0.5)*2*mut, 0.01f), 0.25f);
        g->take_profit_pct = fminf(fmaxf(agents[src_idx].params[6] + ((float)rand()/RAND_MAX-0.5)*2*mut, 0.01f), 0.60f);
        g->min_edge_pct = fminf(fmaxf(agents[src_idx].params[7] + ((float)rand()/RAND_MAX-0.5)*2*mut, 1.0f), 100.0f);
        g->time_horizon = fminf(fmaxf(agents[src_idx].params[8] + ((float)rand()/RAND_MAX-0.5)*2*mut, 0.1f), 10.0f);
        g->mean_reversion_bias = fminf(fmaxf(agents[src_idx].params[9] + ((float)rand()/RAND_MAX-0.5)*2*mut, -1.0f), 1.0f);
        g->learning_rate = fminf(fmaxf(agents[src_idx].params[10] + ((float)rand()/RAND_MAX-0.5)*2*mut, 0.001f), 0.1f);
    }
    
    munmap(live, lst.st_size);
    free(agents);
    
    printf("[DISTILLER] ✅ Live state seeded with %d paper agents + %d mutated clones\n",
           n_agents<live_max?n_agents:live_max, n_agents<live_max?live_max-n_agents:0);
    printf("[DISTILLER]    Total agents: %d, Starting capital: $%.0f\n", live_max, start_cap*live_max);
    printf("\n  ✅ GENOME DISTILLATION COMPLETE\n\n");
    return 0;
}
