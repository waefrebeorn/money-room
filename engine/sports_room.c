/**
 * sports_room.c — 10,000 Sports Fans Betting Engine
 *
 * NOT 10K traders. 10K sports fans.
 * Same Darwin evolution. Different feature space.
 *
 * Reads sports data from timeline.db.
 * Each fan has a genome with sports-specific weights.
 * Best fans get cloned, worst get culled.
 * Paper capital: $50/fan.
 *
 * Timeline is shared. Usage is different.
 *
 * NOT FINANCIAL ADVICE. Sports prediction is algorithmic 
 * analysis of public data, not gambling advice.
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include <unistd.h>
#include "sports_types.h"

#define DB_PATH "/home/wubu2/.hermes/pm_logs/timeline.db"
#define STATE_DIR "/home/wubu2/.hermes/pm_logs/c_room"
#define SPORTS_STATE_PATH STATE_DIR "/sports_state.bin"

// ── Globals ──
static SportsGenome *g_fans = NULL;
static SportsRoomState *g_state = NULL;
static sqlite3 *g_db = NULL;

// ── Feature computation ──
static void compute_sports_features(Game *game, SportsFeatureVector *fv) {
    memset(fv, 0, sizeof(SportsFeatureVector));
    
    if (!g_db) return;
    
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT data FROM timeline WHERE source = ? "
        "ORDER BY ts DESC LIMIT 20";
    
    /* Get recent games for both teams to compute form */
    char src_home[128], src_away[128];
    char clean_home[64], clean_away[64];
    
    /* Clean team names for source matching */
    int hi = 0, ai = 0;
    for (int i = 0; game->home_team[i] && hi < 55; i++) {
        char c = game->home_team[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') clean_home[hi++] = c;
        else if (c == ' ') clean_home[hi++] = '_';
    }
    clean_home[hi] = 0;
    for (int i = 0; game->away_team[i] && ai < 55; i++) {
        char c = game->away_team[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') clean_away[ai++] = c;
        else if (c == ' ') clean_away[ai++] = '_';
    }
    clean_away[ai] = 0;
    
    /* Source pattern: sports_<sport>_<team> */
    snprintf(src_home, sizeof(src_home), "sports_%s_%s", game->sport, clean_home);
    snprintf(src_away, sizeof(src_away), "sports_%s_%s", game->sport, clean_away);
    
    /* Query home team recent results */
    int home_wins = 0, home_games = 0, home_points_for = 0, home_points_against = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, src_home, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *d = (const char *)sqlite3_column_text(stmt, 0);
            if (!d) continue;
            /* Parse: {"home_score":X,"away_score":Y,"status":"final"} */
            const char *hs = strstr(d, "\"home_score\":");
            const char *as = strstr(d, "\"away_score\":");
            if (!hs || !as) continue;
            double hsc = atof(hs + 13);
            double asc = atof(as + 13);
            home_games++;
            home_points_for += (int)hsc;
            home_points_against += (int)asc;
            if (hsc > asc) home_wins++;
        }
        sqlite3_finalize(stmt);
    }
    
    /* Query away team recent results */
    int away_wins = 0, away_games = 0, away_points_for = 0, away_points_against = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, src_away, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *d = (const char *)sqlite3_column_text(stmt, 0);
            if (!d) continue;
            const char *hs = strstr(d, "\"home_score\":");
            const char *as = strstr(d, "\"away_score\":");
            if (!hs || !as) continue;
            double hsc = atof(hs + 13);
            double asc = atof(as + 13);
            away_games++;
            away_points_for += (int)asc;  /* away team's score */
            away_points_against += (int)hsc;
            if (asc > hsc) away_wins++;
        }
        sqlite3_finalize(stmt);
    }
    
    /* Fill feature vector */
    fv->home_win_pct = home_games > 0 ? (float)home_wins / home_games : 0.5f;
    fv->away_win_pct = away_games > 0 ? (float)away_wins / away_games : 0.5f;
    fv->offense_rating = home_games > 0 ?
        (float)home_points_for / home_games / 120.0f : 0.5f;  /* normalize */
    fv->defense_rating = home_games > 0 ?
        (float)home_points_against / home_games / 120.0f : 0.5f;
    
    /* Line movement: inferred from spread */
    fv->line_movement = game->spread != 0 ? 0.5f + (float)(game->spread / 20.0f) : 0.5f;
    if (fv->line_movement < 0) fv->line_movement = 0;
    if (fv->line_movement > 1) fv->line_movement = 1;
    
    /* Public bet % — inferred from line */
    fv->public_bet_pct = game->spread < 0 ? 0.6f : 0.4f;  /* public likes favorites */
    
    /* Over/Under */
    float ou_pct = game->over_under > 0 ?
        (game->over_under - 180.0f) / 100.0f : 0.5f;
    if (ou_pct < 0) ou_pct = 0;
    if (ou_pct > 1) ou_pct = 1;
    fv->over_pct = ou_pct;
    fv->under_pct = 1.0f - ou_pct;
    
    /* Crowd price — from Polymarket data for this game */
    fv->crowd_price = 0.5f;  /* default = coin flip */
    
    /* News sentiment — neutral default */
    fv->news_sentiment = 0.0f;
    
    /* Momentum: last 5 games win rate - 0.5 */
    float home_mom = home_games > 0 ? (float)home_wins / home_games - 0.5f : 0;
    float away_mom = away_games > 0 ? (float)away_wins / away_games - 0.5f : 0;
    fv->momentum = (home_mom - away_mom) / 2.0f;  /* -1 to 1 */
    
    /* Rest advantage — assume neutral (would need scheduled game times) */
    fv->rest_advantage = 0.0f;
    
    /* Playoff stakes — check if sport is in playoff window */
    fv->playoff_stakes = 0.5f;
    
    /* H2H win rate — inferred from spread */ 
    fv->h2h_win_rate = 0.5f;
    
    /* Market value — neutral */
    fv->market_value = 0.5f;
    
    /* Injury impact — neutral (would need injury API) */
    fv->injury_impact = 0.0f;
    
    /* Weather score — neutral (would need weather API) */
    fv->weather_score = 0.5f;
}

// ── Fan vote: predict game outcome ──
static float predict_outcome(SportsGenome *fan, SportsFeatureVector *fv) {
    float score = 0;
    float *w = fan->feat_weight;
    float *f = (float *)fv;
    for (int i = 0; i < SPORTS_FEATURES; i++) {
        score += w[i] * f[i];
    }
    // Sigmoid to 0-1
    return 1.0f / (1.0f + expf(-score));
}

// ── Place a bet ──
static void place_bet(SportsGenome *fan, Game *game, float prediction, 
                       BetRecord *bets, int *n_bets) {
    float edge = fabsf(prediction - 0.5f) - fan->conviction_threshold;
    if (edge <= 0) return;  // Not enough conviction
    
    // Kelly stake
    float kelly = (2.0f * prediction - 1.0f) / 1.0f;  // Simplified
    float stake = fan->capital * fan->max_stake_pct * kelly * fan->risk_tolerance;
    if (stake < 0.01f) return;
    if (stake > fan->capital * fan->max_stake_pct) 
        stake = fan->capital * fan->max_stake_pct;
    
    // Record bet
    BetRecord *bet = &bets[*n_bets];
    bet->fan_id = fan - g_fans;
    bet->game_ts = game->game_ts;
    bet->stake = stake;
    bet->prediction = prediction > 0.5f ? 1 : 0;
    bet->resolved = 0;
    bet->payout = 0;
    strncpy(bet->team, prediction > 0.5f ? game->home_team : game->away_team, MAX_TEAM_NAME - 1);
    (*n_bets)++;
    
    fan->capital -= stake;
}

// ── Resolve bets ──
static void resolve_bets(Game *game, BetRecord *bets, int n_bets) {
    for (int i = 0; i < n_bets; i++) {
        if (bets[i].resolved || bets[i].game_ts != game->game_ts) continue;
        
        int home_won = game->home_score > game->away_score;
        int bet_won = (bets[i].prediction == 1 && home_won) ||
                      (bets[i].prediction == 0 && !home_won);
        
        SportsGenome *fan = &g_fans[bets[i].fan_id];
        bets[i].resolved = bet_won ? 1 : -1;
        
        if (bet_won) {
            double payout = bets[i].stake * 1.91;  // ~-110 odds
            fan->capital += payout;
            bets[i].payout = payout;
            fan->wins++;
            fan->consecutive_wins++;
            fan->consecutive_losses = 0;
        } else {
            fan->consecutive_losses++;
            fan->consecutive_wins = 0;
        }
        
        fan->total_bets++;
        if (fan->capital > fan->peak_capital) fan->peak_capital = fan->capital;
    }
}

// ── Darwin evolution ──
static void run_darwin(int cycle) {
    // Sort by win rate
    // Cull bottom 10%
    // Clone top 10% with mutation
    // Redistribute capital
    // TODO: Full implementation
    (void)cycle;
}

// ── Load games from timeline ──
static int load_games(Game *games, int max_games, int days_back) {
    if (!g_db) return 0;
    
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT source, data, ts FROM timeline "
        "WHERE source LIKE 'sports_%' AND source NOT LIKE 'sports_outcomes_%' "
        "AND source NOT LIKE 'sports__%' "
        "AND ts > ? "
        "GROUP BY source ORDER BY ts DESC LIMIT ?";
    
    int64_t cutoff = (int64_t)time(NULL) - (int64_t)days_back * 86400LL;
    int n = 0;
    
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    
    sqlite3_bind_int64(stmt, 1, cutoff);
    sqlite3_bind_int(stmt, 2, max_games * 2);  /* overfetch + dedupe */
    
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max_games) {
        const char *source = (const char *)sqlite3_column_text(stmt, 0);
        const char *data = (const char *)sqlite3_column_text(stmt, 2);
        int64_t ts = sqlite3_column_int64(stmt, 2);
        
        if (!source || !data) continue;
        
        /* Parse sport from source: sports_<sport> */
        const char *sport_start = source + 7;  /* after "sports_" */
        char sport_name[32];
        int si = 0;
        while (sport_start[si] && sport_start[si] != '_' && si < 30) {
            sport_name[si] = sport_start[si];
            si++;
        }
        sport_name[si] = 0;
        if (si == 0) continue;
        
        /* Parse JSON data */
        const char *home = "?";
        const char *away = "?";
        double home_score = 0, away_score = 0;
        double spread = 0, over_under = 0;
        
        /* Extract fields via simple strstr */
        const char *ht = strstr(data, "\"home_team\":\"");
        if (ht) { ht += 12; const char *he = strchr(ht, '"'); 
            if (he) { int len = (int)(he - ht); if (len > MAX_TEAM_NAME-1) len = MAX_TEAM_NAME-1;
                memcpy((char*)home, ht, len); ((char*)home)[len] = 0; } }
        
        const char *at = strstr(data, "\"away_team\":\"");
        if (at) { at += 12; const char *ae = strchr(at, '"');
            if (ae) { int len = (int)(ae - at); if (len > MAX_TEAM_NAME-1) len = MAX_TEAM_NAME-1;
                memcpy((char*)away, at, len); ((char*)away)[len] = 0; } }
        
        /* These are const char* pointing into the data string - need to copy */
        char home_buf[MAX_TEAM_NAME], away_buf[MAX_TEAM_NAME];
        strncpy(home_buf, home, MAX_TEAM_NAME-1);
        strncpy(away_buf, away, MAX_TEAM_NAME-1);
        home = (const char*)home_buf;
        away = (const char*)away_buf;
        
        const char *hs = strstr(data, "\"home_score\":");
        if (hs) home_score = atof(hs + 13);
        const char *as = strstr(data, "\"away_score\":");
        if (as) away_score = atof(as + 13);
        const char *sp = strstr(data, "\"spread\":");
        if (sp) spread = atof(sp + 9);
        const char *ou = strstr(data, "\"over_under\":");
        if (ou) over_under = atof(ou + 13);
        
        const char *st = strstr(data, "\"status\":\"");
        int status = 0;
        if (st) {
            st += 10;
            if (strncmp(st, "final", 5) == 0 || strncmp(st, "Final", 5) == 0) status = 2;
            else if (strncmp(st, "live", 4) == 0 || strncmp(st, "In Progress", 11) == 0) status = 1;
            else status = 0;
        }
        
        Game *g = &games[n];
        g->game_ts = ts;
        strncpy(g->sport, sport_name, MAX_SPORT_NAME-1);
        strncpy(g->home_team, home, MAX_TEAM_NAME-1);
        strncpy(g->away_team, away, MAX_TEAM_NAME-1);
        g->home_score = home_score;
        g->away_score = away_score;
        g->home_moneyline = 0;
        g->away_moneyline = 0;
        g->spread = spread;
        g->over_under = over_under;
        g->status = status;
        n++;
    }
    sqlite3_finalize(stmt);
    
    return n;
}

// ── Save state ──
static void save_state(void) {
    // TODO: Write sports_state.bin
}

// ── Main ──
int main(int argc, char *argv[]) {
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════════╗\n");
    printf("  ║   SPORTS ROOM — 10,000 Sports Fans                 ║\n");
    printf("  ║   Not 10K traders. 10K fans betting.               ║\n");
    printf("  ║   Same engine. Different genome.                   ║\n");
    printf("  ╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  NOT FINANCIAL ADVICE. Algorithmic sports analysis.\n");
    printf("\n");
    
    // Init DB
    if (sqlite3_open(DB_PATH, &g_db) != SQLITE_OK) {
        fprintf(stderr, "Can't open %s\n", DB_PATH);
        return 1;
    }
    
    // Allocate 10K fans
    g_fans = calloc(SPORTS_AGENTS, sizeof(SportsGenome));
    for (int i = 0; i < SPORTS_AGENTS; i++) {
        init_sports_genome(&g_fans[i]);
    }
    
    printf("  🏟️  %d sports fans ready with paper bankrolls\n", SPORTS_AGENTS);
    printf("  Total paper capital: $%.0f\n", SPORTS_AGENTS * 50.0f);
    printf("\n");
    
    // Main loop (daily)
    Game games[MAX_GAMES_PER_DAY];
    BetRecord bets[SPORTS_AGENTS];
    int cycle = 0;
    
    while (1) {
        // Load today's games
        int n_games = load_games(games, MAX_GAMES_PER_DAY, 1);
        int n_bets = 0;
        
        // Each fan votes on each game
        for (int g = 0; g < n_games; g++) {
            SportsFeatureVector fv;
            compute_sports_features(&games[g], &fv);
            
            for (int f = 0; f < SPORTS_AGENTS; f++) {
                float prediction = predict_outcome(&g_fans[f], &fv);
                place_bet(&g_fans[f], &games[g], prediction, bets, &n_bets);
            }
        }
        
        // Resolve yesterday's games
        Game yesterday_games[MAX_GAMES_PER_DAY];
        int n_yesterday = load_games(yesterday_games, MAX_GAMES_PER_DAY, 2);
        for (int g = 0; g < n_yesterday; g++) {
            resolve_bets(&yesterday_games[g], bets, n_bets);
        }
        
        // Darwin
        run_darwin(cycle);
        
        // Report
        if (cycle % 10 == 0) {
            double total_cap = 0, total_wr = 0;
            for (int i = 0; i < SPORTS_AGENTS; i++) {
                total_cap += g_fans[i].capital;
                total_wr += g_fans[i].win_rate;
            }
            printf("  Cycle %d: %.0f fans active, avg cap $%.2f, avg WR %.3f\n",
                   cycle, total_cap / 50.0, total_cap / SPORTS_AGENTS,
                   total_wr / SPORTS_AGENTS);
        }
        
        cycle++;
        sleep(3600);  // Check hourly
    }
    
    free(g_fans);
    sqlite3_close(g_db);
    return 0;
}
