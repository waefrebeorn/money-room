# 🏈 Sports Room — 10,000 Sports Fans Engine

> **Architecture:** Same C engine DNA, different genome.
> **Not 10K traders. 10K sports fans betting.**
> **Timeline is shared. Usage is different.**

---

## Overview

The sports room is a separate C engine sharing `timeline.db` with the trading room but using **sports-specific features and genome weights**. Instead of 10K traders predicting BTC direction, it's 10K sports fans predicting game outcomes.

Data flows: **Timeline DB → Sports Room Engine → Darwin Evolution → Betting Results**

```
timeline.db
├── sports_odds (moneyline, spread, over/under)
├── sports_results (game scores, stats)
├── sports_stats (team/player performance)
├── polymarket_sports (prediction market odds on games)
├── kalshi_sports (sports multi-game markets)
└── shared (macro, sentiment, etc.)
```

## Data Sources (Free)

| Source | Data | Cost | Status |
|--------|------|:----:|--------|
| **The Odds API** | Moneyline, spread, O/U for NFL/NBA/MLB/NHL/soccer | Free tier | 🔲 Plan |
| **Polymarket Gamma API** | Sports betting odds on games | Free | ✅ Works |
| **Kalshi API** | Sports multi-game markets | Free | ✅ Works |
| **ESPN/API-Sports** | Game results, team/player stats | Free (100/d) | 🔲 Plan |
| **GDELT** | News sentiment for teams/sports | Free | ✅ Have |

## Sports Genome (What each fan evolves)

Each of the 10K sports fans has a **genome** with sports-specific weights:

```c
// Sports Genome — each fan evolves these weights
typedef struct {
    // ── Core features ──
    float home_advantage_weight;     // How much home field matters
    float form_weight;               // Recent team form (last 5 games)
    float h2h_weight;               // Head-to-head history
    float line_movement_weight;      // How odds movement predicts outcome
    float public_bet_weight;         // Public betting % (fade the public?)
    float injury_weight;             // Player injury impact
    float rest_weight;              // Days of rest / travel distance
    float weather_weight;           // Weather impact (outdoor sports)
    float divisional_weight;        // Divisional/rivalry games
    float playoff_implication_weight;// Playoff/elimination stakes
    
    // ── Team-specific weights ──
    float offense_rating;           // Points scored / yardage
    float defense_rating;           // Points allowed / turnovers
    float special_teams_weight;     // Kicking/return game
    float clutch_weight;            // Performance in close games
    
    // ── Meta parameters ──
    float risk_tolerance;           // Kelly fraction (0.1-1.0)
    float conviction_threshold;     // Min edge to bet (0-0.1)
    float max_stake_pct;            // Max % of bankroll per bet
    float confidence_decay;         // How fast confidence fades after loss
    
    // ── Strategy bias ──
    float favorite_bias;            // 0 = dogs only, 1 = favorites only
    float over_under_bias;          // 0 = unders, 1 = overs
    float sport_specialization[5];  // NFL, NBA, MLB, NHL, soccer weights
} SportsGenome;
```

## Feature Vector

Computed each cycle from timeline data:

| Dimension | Feature | Source |
|:---------:|---------|--------|
| 0 | Home win% last 10 | sports_results |
| 1 | Away win% last 10 | sports_results |
| 2 | Line movement (opening→current) | sports_odds |
| 3 | Public bet% on favorite | sports_odds |
| 4 | Over bet% | sports_odds |
| 5 | Under bet% | sports_odds |
| 6 | H2H win rate (last 20) | sports_results |
| 7 | Days since last game | sports_results |
| 8 | Points scored/game (offense) | sports_stats |
| 9 | Points allowed/game (defense) | sports_stats |
| 10 | Injury impact score | sports_stats |
| 11 | Weather score | weather |
| 12 | Rest advantage (days) | sports_results |
| 13 | Playoff implication (0/1) | sports_schedule |
| 14 | Polymarket crowd price | polymarket_sports |
| 15 | News sentiment for team | news_gdelt |
| 16 | Momentum score | sports_results |
| 17 | Total team market value | sports_stats |

## Architecture (C)

The sports room reuses the same engine architecture as `room_engine.c` but with:
- Different genome struct (`SportsGenome` instead of `Genome`)
- Different features (sports stats instead of market OHLCV)
- Different resolution (daily, not 1-min — games happen daily)
- Same Darwin evolution (best fans cloned, worst culled)
- Same P2P betting pool (fans bet against each other)

```c
// sports_room.c — Main loop
while (running) {
    // 1. Load games for today
    load_games_from_timeline(&games, today);
    
    // 2. Compute features for each game
    for (each game) {
        compute_sports_features(game, &fv);
        
        // 3. Each fan votes
        for (each fan in fans) {
            vote = predict_outcome(fan.genome, fv);
            record_bet(fan, game, vote, fan.genome.risk_tolerance);
        }
    }
    
    // 4. Resolve bets from yesterday's games
    resolve_bets(yesterday_games, fans);
    
    // 5. Darwin evolution (daily)
    run_darwin(fans, N_FANS, cycle);
    
    // 6. Log state
    save_sports_state(state);
    sleep(3600); // Check hourly for new games
}
```

## Timeline Integration

The sports room reads from and writes to the shared `timeline.db`:

```sql
-- Read games
SELECT ts, data FROM timeline 
WHERE source = 'sports_results' 
  AND category = 'nba' 
  AND ts > today_epoch
ORDER BY ts;

-- Read odds
SELECT ts, data FROM timeline 
WHERE source = 'sports_odds' 
  AND data LIKE '%"' || team_name || '"%'
ORDER BY ts;

-- Write results (resolved bets)
INSERT INTO timeline (ts, source, category, data)
VALUES (now, 'sports_room_bets', 'nba', 
        '{"game":"LAL_vs_BOS","winner":"LAL","fan_count":4821,"correct_pct":61.2}');
```

## Pre-2025 Data Strategy

Before Polymarket/Kalshi sports data existed:
- **Game results**: Free from ESPN/API-Sports (has historical data)
- **Odds data**: The Odds API has historical snapsnots back to 2020
- **Sentiment**: GDELT news sentiment for team mentions
- **Team stats**: API-Sports has historical team/player stats

## Files to Build

| File | Purpose | Status |
|------|---------|--------|
| `sports_room.c` | Main sports engine loop | 🔲 Plan |
| `sports_features.c` | Sports feature computation | 🔲 Plan |
| `sports_genome.h` | Sports genome struct definition | 🔲 Plan |
| `scores_collector.c` | C binary to fetch game scores | 🔲 Plan |
| `odds_collector.c` | C binary to fetch sports odds | 🔲 Plan |

## First Steps

1. ✅ Kalshi collector built — sports markets flowing to timeline
2. 🔲 Polymarket sports collector — pulls sports betting odds
3. 🔲 Scores collector — free historical game results
4. 🔲 Sports genome header — define the fan architecture
5. 🔲 Sports room engine — main loop

---

*NOT FINANCIAL ADVICE. Sports prediction is algorithmic analysis of public data, not gambling advice. 10K paper fans betting paper money.*
