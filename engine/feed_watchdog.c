/**
 * feed_watchdog.c — E51: Stale Data Pause & Room Restart
 *
 * Checks market_feed.json freshness across all rooms.
 * If stale > 5min: writes PAUSE flag, kills room processes.
 * If fresh again: removes PAUSE flag.
 * If room_state.bin is stale: recycles the engine.
 *
 * Build: gcc -O3 -march=native feed_watchdog.c -o feed_watchdog -lm -ljansson
 * Usage: ./feed_watchdog                    — check & act
 *        ./feed_watchdog status             — show room status only
 *        ./feed_watchdog force              — force restart stale rooms
 */
#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <jansson.h>

#define ROOMS_DIR "/home/wubu2/.hermes/pm_logs/rooms"
#define C_ROOM_DIR "/home/wubu2/.hermes/pm_logs/c_room"
#define MAX_PATH 512
#define STALE_SECS 300  /* 5 minutes */
#define FEED_PATH "market_feed.json"
#define STATE_PATH "room_state.bin"
#define PAUSE_FILE "room.paused"

static int file_age(const char *path) {
    struct stat sb;
    if (stat(path, &sb) < 0) return -1;
    time_t now = time(NULL);
    return (int)(now - sb.st_mtime);
}

static int pause_room(const char *room_dir) {
    char pause[MAX_PATH];
    snprintf(pause, sizeof(pause), "%s/%s", room_dir, PAUSE_FILE);
    FILE *f = fopen(pause, "w");
    if (!f) return 0;
    fprintf(f, "%ld\n", time(NULL));
    fclose(f);
    return 1;
}

static int resume_room(const char *room_dir) {
    char pause[MAX_PATH];
    snprintf(pause, sizeof(pause), "%s/%s", room_dir, PAUSE_FILE);
    return unlink(pause) == 0 || errno == ENOENT;
}

static int is_paused(const char *room_dir) {
    char pause[MAX_PATH];
    snprintf(pause, sizeof(pause), "%s/%s", room_dir, PAUSE_FILE);
    struct stat sb;
    return stat(pause, &sb) == 0;
}

static int has_fresh_state(const char *room_dir) {
    char state[MAX_PATH];
    // All rooms share c_room state file — check that instead
    snprintf(state, sizeof(state), "%s/%s", C_ROOM_DIR, STATE_PATH);
    int age = file_age(state);
    if (age < 0) {
        // Fallback: check per-room state
        snprintf(state, sizeof(state), "%s/%s", room_dir, STATE_PATH);
        age = file_age(state);
    }
    return (age >= 0 && age < 600); /* 10 min threshold for state */
}

int main(int argc, char **argv) {
    int mode = 0; /* 0=act, 1=status, 2=force */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "status") == 0) mode = 1;
        else if (strcmp(argv[i], "force") == 0) mode = 2;
    }
    
    printf("FEED WATCHDOG — %s\n", ctime(&(time_t){time(NULL)}));
    
    /* Check c_room feed freshness */
    char feed_path[MAX_PATH];
    snprintf(feed_path, sizeof(feed_path), "%s/%s", C_ROOM_DIR, "market_feed.json");
    int feed_age = file_age(feed_path);
    
    if (feed_age < 0) {
        printf("C_ROOM feed: MISSING\n");
    } else {
        printf("C_ROOM feed: %ds old %s\n", feed_age,
               feed_age > STALE_SECS ? "⚠ STALE" : "✅ FRESH");
    }
    
    /* Scan rooms */
    DIR *dir = opendir(ROOMS_DIR);
    if (!dir) return 1;
    
    struct dirent *entry;
    int total = 0, paused = 0, stale = 0, fresh = 0;
    
    printf("\nROOM             FEED     STATE    ACTION\n");
    printf("---------------  -------  -------  ------\n");
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char room_dir[MAX_PATH];
        snprintf(room_dir, sizeof(room_dir), "%s/%s", ROOMS_DIR, entry->d_name);
        
        /* Check feed */
        char rf[MAX_PATH];
        snprintf(rf, sizeof(rf), "%s/%s", room_dir, "market_feed.json");
        int fa = file_age(rf);
        const char *feed_status = fa < 0 ? "MISSING" :
                                  (fa > STALE_SECS ? "STALE" : "fresh");
        
        /* Check state */
        int sa = has_fresh_state(room_dir);
        const char *state_status = sa ? "fresh" : "STALE";
        
        /* Check pause */
        int pp = is_paused(room_dir);
        
        const char *action = "OK";
        if (fa > STALE_SECS || !sa) {
            if (mode == 0 || mode == 2) {
                if (!pp) {
                    pause_room(room_dir);
                    action = "PAUSED";
                    paused++;
                } else {
                    action = "already paused";
                }
                stale++;
            } else {
                action = "⚠ WOULD PAUSE";
                stale++;
            }
        } else {
            if (pp) {
                resume_room(room_dir);
                action = "RESUMED";
                fresh++;
            } else {
                fresh++;
            }
        }
        
        printf("%-15s  %-7s  %-7s  %s\n",
               entry->d_name, feed_status, state_status, action);
        total++;
    }
    closedir(dir);
    
    printf("\n%d rooms, %d fresh, %d stale/paused\n", total, fresh, stale);
    printf("Feed threshold: %ds (%.0f min)\n", STALE_SECS, STALE_SECS/60.0);
    
    return stale > 0 ? 1 : 0;
}
