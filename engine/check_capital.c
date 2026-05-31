#include <stdio.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "types.h"

int main() {
    int fd = open("/home/wubu2/.hermes/pm_logs/c_room/room_state_paper.bin", O_RDONLY);
    struct stat st; fstat(fd, &st);
    unsigned char *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    
    int agent_off = offsetof(RoomState, agents);
    printf("Agent offset: %d, AgentSize: %zu\n", agent_off, sizeof(AgentState));
    
    for(int i=0; i<5; i++) {
        AgentState *as = (AgentState*)(map + agent_off + i * sizeof(AgentState));
        printf("\nAgent %d:\n", i);
        printf("  alive=%d capital=%f start_cap=%f\n", as->alive, as->capital, as->starting_capital);
        printf("  trades=%d wins=%d losses=%d\n", as->trades, as->wins, as->losses);
        printf("  total_pnl=%f win_rate_ema=%f\n", as->total_pnl, as->win_rate_ema);
        printf("  genome.pos_size=%f conv_thresh=%f risk_tol=%f\n", 
               as->genome.position_size, as->genome.conviction_threshold, as->genome.risk_tolerance);
        printf("  genome.time_horiz=%f reversion=%f\n", 
               as->genome.time_horizon, as->genome.mean_reversion_bias);
    }
    munmap(map, st.st_size);
    return 0;
}
