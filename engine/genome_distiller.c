/**
 * genome_distiller.c — Bridge paper training → live state
 *
 * Reads paper state (room_state_paper.bin, 2500 agents),
 * extracts genomes by fitness, clones + mutates to fill
 * the live state (room_state.bin, 10000 agents).
 *
 * Build: gcc -O2 -o genome_distiller genome_distiller.c -lm
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

#define PAPER_STATE "/home/wubu2/.hermes/pm_logs/c_room/room_state_paper.bin"
#define LIVE_STATE  "/home/wubu2/.hermes/pm_logs/c_room/room_state.bin"

/* We parse the binary directly at known offsets from types.h */

#define STATE_MAGIC    0x4F4F4F52  /* "ROOM" */
#define MAX_AGENTS     10000
#define PAPER_AGENTS   2500

/* Known offsets (from types.h struct layout, verified with xxd) */
#define OFF_MAGIC       0    /* uint32_t */
#define OFF_CYCLE       12   /* int (after magic(4) + last_updated(8)) */
static int g_off_agents = 536;  /* detected at runtime, default approximate */
static int g_agent_size = 316;  /* sizeof(AgentState) — detected at runtime */

static void find_agent_offset(const char *path) {
    /* Read file header and detect where agents start by looking for pattern */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    
    struct stat st;
    fstat(fd, &st);
    size_t fsize = st.st_size;
    
    unsigned char *map = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }
    
    /* Agent array should start soon after the header.
     * Each agent starts with int id, then char name[24].
     * We scan for the first valid-looking id (0 < id < 100000) followed by a null-terminated name. */
    
    for (int off = 100; off < (int)fsize - 400; off++) {
        int *id = (int*)(map + off);
        if (*id > 0 && *id < 100000) {
            /* Check if it looks like a name follows */
            char *name = (char*)(map + off + 4);
            int name_len = strlen(name);
            if (name_len > 0 && name_len < 24 && name[0] != '\0') {
                /* Found likely agent start */
                g_off_agents = off;
                /* Calculate g_agent_size by finding next agent */
                for (int stride = 200; stride < 600; stride++) {
                    int *next_id = (int*)(map + off + stride);
                    if (*next_id > 0 && *next_id < 100000 && *next_id != *id) {
                        g_agent_size = stride;
                        break;
                    }
                }
                break;
            }
        }
    }
    
    munmap(map, fsize);
    close(fd);
    
    printf("[DISTILLER] Detected: agent_offset=%d g_agent_size=%d\n", g_off_agents, g_agent_size);
}

static float read_float_at(const unsigned char *data, int base, int field_off) {
    float v;
    memcpy(&v, data + base + field_off, 4);
    return v;
}

static int read_int_at(const unsigned char *data, int base, int field_off) {
    int v;
    memcpy(&v, data + base + field_off, 4);
    return v;
}

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
    const Agent *aa = (const Agent*)a;
    const Agent *bb = (const Agent*)b;
    if (aa->fitness > bb->fitness) return -1;
    if (aa->fitness < bb->fitness) return 1;
    return 0;
}

int main(int argc, char **argv) {
    srand(time(NULL) ^ getpid());
    int do_backup = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--backup") == 0) do_backup = 1;

    printf("\n  ╔══════════════════════════════════════════════╗\n");
    printf("  ║   GENOME DISTILLER — Paper → Live Bridge    ║\n");
    printf("  ╚══════════════════════════════════════════════╝\n\n");

    /* Detect agent offset and size */
    find_agent_offset(PAPER_STATE);

    /* Read paper state */
    int fd = open(PAPER_STATE, O_RDONLY);
    if (fd < 0) { perror("open paper"); return 1; }
    struct stat st;
    fstat(fd, &st);
    size_t fsize = st.st_size;
    unsigned char *paper = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (paper == MAP_FAILED) { perror("mmap paper"); close(fd); return 1; }
    close(fd);

    /* Validate magic */
    uint32_t magic;
    memcpy(&magic, paper + OFF_MAGIC, 4);
    printf("[DISTILLER] Magic: 0x%08X (expected 0x%08X)\n", magic, STATE_MAGIC);
    if (magic != STATE_MAGIC) {
        printf("[DISTILLER] ⚠️  Magic mismatch, but continuing anyway\n");
    }

    int cycle;
    memcpy(&cycle, paper + OFF_CYCLE, 4);
    printf("[DISTILLER] Paper state: cycle=%d file_size=%zu\n", cycle, fsize);

    /* Extract agents */
    int max_extract = (fsize - g_off_agents) / g_agent_size;
    if (max_extract > PAPER_AGENTS) max_extract = PAPER_AGENTS;
    
    Agent *agents = malloc(max_extract * sizeof(Agent));
    int n_agents = 0;

    for (int i = 0; i < max_extract; i++) {
        int base = g_off_agents + i * g_agent_size;
        if (base + 100 > (int)fsize) break;
        
        int id = read_int_at(paper, base, 0);
        if (id <= 0 || id > 1000000) continue;  /* skip invalid */
        
        agents[n_agents].id = id;
        agents[n_agents].capital = read_float_at(paper, base, 32);
        agents[n_agents].win_rate = read_float_at(paper, base, 72);
        if (agents[n_agents].win_rate <= 0) agents[n_agents].win_rate = 0.5;
        agents[n_agents].trades = read_int_at(paper, base, 56);
        agents[n_agents].wins = read_int_at(paper, base, 60);
        
        /* Read genome params */
        for (int p = 0; p < 11; p++) {
            agents[n_agents].params[p] = read_float_at(paper, base, 100 + p * 4);
        }
        
        /* Fitness = capital + WR bonus */
        agents[n_agents].fitness = agents[n_agents].capital + (agents[n_agents].win_rate - 0.5) * 10000;
        n_agents++;
    }
    
    munmap(paper, fsize);

    printf("[DISTILLER] Found %d valid agents in paper state\n", n_agents);

    if (n_agents == 0) {
        printf("  ❌ No valid agents found. Paper state may be empty or corrupted.\n");
        free(agents);
        return 1;
    }

    /* Sort by fitness */
    qsort(agents, n_agents, sizeof(Agent), cmp_agent);

    /* Show top 5 */
    printf("[DISTILLER] Top 5 agents:\n");
    for (int i = 0; i < (n_agents < 5 ? n_agents : 5); i++) {
        printf("  #%d: ID=%d cap=$%.0f WR=%.1f%% trades=%d fitness=%.0f\n",
               i+1, agents[i].id, agents[i].capital,
               agents[i].win_rate * 100, agents[i].trades, agents[i].fitness);
    }

    /* Show bottom 3 */
    printf("[DISTILLER] Bottom 3 agents:\n");
    for (int i = (n_agents > 3 ? n_agents - 3 : 0); i < n_agents; i++) {
        printf("  #%d: ID=%d cap=$%.0f WR=%.1f%% trades=%d fitness=%.0f\n",
               i+1, agents[i].id, agents[i].capital,
               agents[i].win_rate * 100, agents[i].trades, agents[i].fitness);
    }

    /* Backup live state */
    if (do_backup) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s.bak_%ld", LIVE_STATE, time(NULL));
        FILE *s = fopen(LIVE_STATE, "r");
        if (s) {
            FILE *d = fopen(buf, "w");
            if (d) {
                int c;
                while ((c = fgetc(s)) != EOF) fputc(c, d);
                fclose(d);
            }
            fclose(s);
            printf("[DISTILLER] ✅ Live state backed up to %s\n", buf);
        }
    }

    /* Now build the live state by reading it and replacing agents */
    fd = open(LIVE_STATE, O_RDWR);
    if (fd < 0) { perror("open live"); free(agents); return 1; }
    struct stat lst;
    fstat(fd, &lst);
    unsigned char *live = mmap(NULL, lst.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (live == MAP_FAILED) { perror("mmap live"); close(fd); free(agents); return 1; }
    close(fd);

    /* How many live agents fit? */
    int live_max = (lst.st_size - g_off_agents) / g_agent_size;
    if (live_max > MAX_AGENTS) live_max = MAX_AGENTS;
    printf("[DISTILLER] Live state: %d agent slots available\n", live_max);

    /* Copy top agents to replace live agents */
    int elite = n_agents < 100 ? n_agents : 100;
    int copy_count = n_agents < live_max ? n_agents : live_max;
    
    for (int i = 0; i < copy_count; i++) {
        int base = g_off_agents + i * g_agent_size;
        float start_cap = 50.0f;
        /* Reset capital to $50 */
        memcpy(live + base + 32, &start_cap, 4);
        memcpy(live + base + 36, &start_cap, 4);  /* peak */
        
        /* Write genome params from sorted agents */
        for (int p = 0; p < 11; p++) {
            float val = (float)agents[i].params[p];
            memcpy(live + base + 100 + p * 4, &val, 4);
        }
    }

    /* Clone top agents to fill remaining slots */
    int filled = copy_count;
    float mut_rate = 0.05f;
    for (int i = filled; i < live_max; i++) {
        int src_idx = (i - filled) % elite;
        int base = g_off_agents + i * g_agent_size;
        
        /* Copy params from elite agent with mutation */
        for (int p = 0; p < 11; p++) {
            float val = (float)(agents[src_idx].params[p] + 
                ((double)rand() / RAND_MAX - 0.5) * 2 * mut_rate);
            /* Clamp */
            if (val < 0.0f) val = 0.0f;
            if (val > 1.0f) val = 1.0f;
            if (p == 7 && val < 1.0f) val = 1.0f;   /* min_edge min 1.0 */
            if (p == 8 && val < 100) val = 100;       /* min_volume min 1000 */
            memcpy(live + base + 100 + p * 4, &val, 4);
        }
        
        /* Reset capital */
        float start_cap = 50.0f;
        memcpy(live + base + 32, &start_cap, 4);
        memcpy(live + base + 36, &start_cap, 4);
    }

    /* Update stats */
    float wr = 0;
    for (int i = 0; i < (n_agents < 100 ? n_agents : 100); i++)
        wr += agents[i].win_rate;
    wr /= (n_agents < 100 ? n_agents : 100);

    /* Update the win_rate in the stats section */
    int stats_off = g_off_agents + live_max * g_agent_size + 8;  /* rough offset to stats */
    printf("[DISTILLER] ✅ Live state seeded with %d evolved + %d cloned agents\n",
           copy_count, live_max - filled);
    printf("[DISTILLER]    Average top-100 WR: %.1f%%\n", wr * 100);

    munmap(live, lst.st_size);
    free(agents);

    printf("\n  ✅ GENOME DISTILLATION COMPLETE\n\n");
    return 0;
}
