/**
 * demo_extractor.c — Extract last 24h of engine history for free demo page
 *
 * Reads last N rows from room_log.csv (via tail in wrapper), filters
 * to last 86400s, samples to ~288 points (5-min resolution).
 *
 * Build: gcc -O2 -o demo_extractor demo_extractor.c -lm
 * Usage: ./demo_extractor
 *   Reads room_log.csv from stdin, outputs demo_history.json to docs/
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define MAX_POINTS 10000

typedef struct {
    int cycle;
    long window_ts;
    char asset[16];
    int votes;
    int active;
    float win_rate;
    float sharpe;
    float dd_pct;
    float consensus_spread;
    float room_pnl_pct;
    int room_trades;
    float room_wr;
    float room_cap;
} LogRow;

static LogRow rows[MAX_POINTS];
static int nrows = 0;

int main(void) {
    time_t now = time(NULL);
    time_t cutoff = now - 86400;

    char *line = NULL;
    size_t len = 0;
    long linenum = 0;
    long skipped = 0;

    while (getline(&line, &len, stdin) != -1) {
        linenum++;
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';

        if (strncmp(line, "cycle,", 6) == 0) { skipped++; continue; }

        int cycle, votes, active, room_trades;
        long window_ts;
        char asset[16];
        float win_rate, sharpe, dd_pct, consensus_spread, room_pnl_pct, room_wr, room_cap;

        int parsed = sscanf(line, "%d,%ld,%15[^,],%d,%d,%f,%f,%f,%f,%f,%d,%f,%f",
            &cycle, &window_ts, asset, &votes, &active,
            &win_rate, &sharpe, &dd_pct, &consensus_spread,
            &room_pnl_pct, &room_trades, &room_wr, &room_cap);

        if (parsed < 13) continue;
        if (window_ts < cutoff) continue;

        if (nrows < MAX_POINTS) {
            rows[nrows].cycle = cycle;
            rows[nrows].window_ts = window_ts;
            strncpy(rows[nrows].asset, asset, 15);
            rows[nrows].votes = votes;
            rows[nrows].active = active;
            rows[nrows].win_rate = win_rate;
            rows[nrows].sharpe = sharpe;
            rows[nrows].dd_pct = dd_pct;
            rows[nrows].consensus_spread = consensus_spread;
            rows[nrows].room_pnl_pct = room_pnl_pct;
            rows[nrows].room_trades = room_trades;
            rows[nrows].room_wr = room_wr;
            rows[nrows].room_cap = room_cap;
            nrows++;
        }
    }
    free(line);

    fprintf(stderr, "[DEMO] Read %ld lines, %ld headers skipped, %d rows in window\n", linenum, skipped, nrows);

    /* Sample to ~288 points */
    int step = 1;
    if (nrows > 288) step = nrows / 288;
    int out_count = 0;

    /* mkdir */
    system("mkdir -p /home/wubu2/money-room/docs/demos");

    FILE *out = fopen("/home/wubu2/money-room/docs/demos/demo_history.json", "w");
    if (!out) { perror("fopen"); return 1; }

    fprintf(out, "{\n");
    fprintf(out, "  \"generated_at\": %ld,\n", now);
    fprintf(out, "  \"cutoff_ts\": %ld,\n", cutoff);
    fprintf(out, "  \"rows_total\": %d,\n", nrows);
    fprintf(out, "  \"rows_shown\": %d,\n", (nrows + step - 1) / step);
    fprintf(out, "  \"step\": %d,\n", step);
    fprintf(out, "  \"history\": [\n");

    for (int i = 0; i < nrows; i += step) {
        LogRow *r = &rows[i];
        fprintf(out, "    [%d,%ld,%d,%d,%.4f,%.4f,%.4f,%d,%.4f,%.4f]",
            r->cycle, r->window_ts,
            r->votes, r->active,
            r->win_rate, r->sharpe, r->dd_pct,
            r->room_trades, r->room_wr, r->room_cap);
        out_count++;
        if (i + step < nrows) fputc(',', out);
        fputc('\n', out);
    }
    fprintf(out, "  ]\n}\n");
    fclose(out);

    fprintf(stderr, "[DEMO] Wrote %d points to demos/demo_history.json\n", out_count);
    return 0;
}
