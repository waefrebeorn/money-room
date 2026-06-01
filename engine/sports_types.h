#ifndef SPORTS_TYPES_H
#define SPORTS_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// ── Sports Room Constants ──
#define SPORTS_AGENTS       10000      // 10K sports fans
#define SPORTS_FEATURES     18         // Feature dimensions
#define MAX_GAMES_PER_DAY   100        // Max concurrent games
#define MAX_SPORTS          5          // NFL, NBA, MLB, NHL, soccer
#define MAX_SPORT_NAME      16
#define MAX_TEAM_NAME       64
#define SPORTS_STATE_MAGIC  0x53505254  // "SPRT" in hex

// ── Sports Genome ──
// Each of the 10K sports fans evolves these weights
typedef struct {
    // Core feature weights (18 dimensions)
    float feat_weight[SPORTS_FEATURES];
    
    // Meta parameters
    float risk_tolerance;           // Kelly fraction (0.1-1.0)
    float conviction_threshold;     // Min edge to bet (0-0.1)
    float max_stake_pct;            // Max % of bankroll per bet (0.01-0.25)
    float confidence_decay;         // How fast confidence fades after loss (0-1)
    
    // Strategy bias
    float favorite_bias;            // -1 = dogs only, +1 = favorites only
    float over_under_bias;          // -1 = unders, +1 = overs
    float sport_specialization[MAX_SPORTS]; // Per-sport expertise weights
    
    // State
    float capital;                  // Current paper bankroll
    int total_bets;                 // Lifetime bets
    int wins;                       // Lifetime wins
    int consecutive_wins;
    int consecutive_losses;
    float peak_capital;
    float win_rate;                 // Running win rate
} SportsGenome;

// ── Sports Feature Vector ──
typedef struct {
    float home_win_pct;            // Home team win% last 10
    float away_win_pct;            // Away team win% last 10
    float line_movement;           // Opening line → current (pct change)
    float public_bet_pct;          // % of public bets on favorite
    float over_pct;                // % of bets on over
    float under_pct;               // % of bets on under
    float h2h_win_rate;            // Head-to-head win rate (0-1)
    float days_since_last_game;    // Rest days for both teams (diff)
    float offense_rating;          // Points scored/game (normalized)
    float defense_rating;          // Points allowed/game (normalized)
    float injury_impact;           // Injury impact score (0-1)
    float weather_score;           // Weather impact (0-1)
    float rest_advantage;          // Team rest days advantage (-1 to 1)
    float playoff_stakes;          // Playoff/elimination importance (0-1)
    float crowd_price;             // Polymarket/Kalshi implied probability
    float news_sentiment;          // Team news sentiment (-1 to 1)
    float momentum;                // Team momentum (last 5 games)
    float market_value;            // Team market value (normalized)
} SportsFeatureVector;

// ── Game ──
typedef struct {
    int64_t game_ts;               // Game timestamp
    char sport[MAX_SPORT_NAME];    // "nfl", "nba", "mlb", "nhl", "soccer"
    char home_team[MAX_TEAM_NAME];
    char away_team[MAX_TEAM_NAME];
    double home_score;             // 0 = not yet played
    double away_score;             // 0 = not yet played
    double home_moneyline;         // Odds (e.g., -150, +200)
    double away_moneyline;
    double spread;                 // Point spread
    double over_under;             // Total O/U
    int status;                    // 0=upcoming, 1=live, 2=resolved
} Game;

// ── Bet Record ──
typedef struct {
    int fan_id;
    int64_t game_ts;
    char team[MAX_TEAM_NAME];      // Which team they bet on
    double stake;                  // Amount wagered
    double odds;                   // Odds at bet time
    int prediction;                // 0=under/away, 1=over/home
    int resolved;                  // 0=open, 1=won, -1=lost
    double payout;                 // Amount won (0 if lost)
} BetRecord;

// ── Sports Room State (persistent) ──
typedef struct {
    uint32_t magic;                // SPORTS_STATE_MAGIC
    int cycle;
    int64_t last_update;
    double total_capital;
    double mean_capital;
    double mean_win_rate;
    int total_bets;
    int resolved_bets;
    double total_pnl;
    double sharpe_ratio;
    double max_drawdown;
    int active_games;
    int n_fans;
    double consensus_spread;        // How divided fans are
    char active_sport[MAX_SPORT_NAME];
} SportsRoomState;

// ── Darwin Record ──
typedef struct {
    int cycle;
    int culled;
    int cloned;
    double mean_wr_before;
    double mean_wr_after;
    double mean_capital_before;
    double mean_capital_after;
    double diversity;
    double redistributed_pool;
} SportsDarwinRecord;

// ── Inline helpers ──
static inline void init_sports_genome(SportsGenome *g) {
    g->risk_tolerance = 0.25f;            // Conservative Kelly
    g->conviction_threshold = 0.05f;      // Need 5% edge
    g->max_stake_pct = 0.10f;            // Max 10% per bet
    g->confidence_decay = 0.1f;          // Slow decay
    g->favorite_bias = 0.0f;            // Neutral
    g->over_under_bias = 0.0f;          // Neutral
    g->capital = 50.0f;                  // $50 paper bankroll
    g->total_bets = 0;
    g->wins = 0;
    g->consecutive_wins = 0;
    g->consecutive_losses = 0;
    g->peak_capital = 50.0f;
    g->win_rate = 0.0f;
    
    // Randomize feature weights
    for (int i = 0; i < SPORTS_FEATURES; i++) {
        g->feat_weight[i] = (float)(rand() % 200 - 100) / 100.0f;
    }
    
    // Randomize sport specialization
    for (int i = 0; i < MAX_SPORTS; i++) {
        g->sport_specialization[i] = (float)(rand() % 100) / 100.0f;
    }
}

#endif // SPORTS_TYPES_H
