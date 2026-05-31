#ifndef PAPER_FEATURE_BRIDGE_H
#define PAPER_FEATURE_BRIDGE_H

#include "types.h"

// Populate ALL auxiliary MarketTick fields from timeline.db
// Replaces hardcoded vix=16, sp500=5000, etc. with real historical data
void paper_load_aux(MarketTick *tick);

#endif
