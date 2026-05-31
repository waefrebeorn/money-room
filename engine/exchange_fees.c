/*
 * exchange_fees.c — Per-exchange fee tables and slippage models
 * Platform-specific: Kraken, Coinbase, Binance, Bybit, Polymarket, Robinhood
 *
 * Compile: gcc -O3 -c exchange_fees.c  # object file
 *          gcc -O3 -o exchange_fees exchange_fees.c -lm  # standalone test
 * Usage:   ./exchange_fees [exchange] [trade_size] [is_taker]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Exchange Fee Table ── */
typedef struct {
    const char *name;
    float taker_fee;     /* Fraction of trade (0.0026 = 0.26%) */
    float maker_fee;     /* Fraction for limit orders */
    float min_trade;     /* Minimum trade size ($) */
    float min_fee;       /* Minimum fee per trade ($), 0 = no min */
} ExchangeFees;

static const ExchangeFees EXCHANGES[] = {
    {"kraken",        0.0026f, 0.0016f, 10.00f, 0.00f},
    {"coinbase",      0.0060f, 0.0040f,  2.00f, 0.99f},
    {"coinbase_adv",  0.0050f, 0.0035f,  2.00f, 0.00f},  /* Coinbase Advanced Trade */
    {"binance",       0.0010f, 0.00075f, 10.00f, 0.00f},
    {"binance_us",    0.0010f, 0.0010f,  10.00f, 0.00f},
    {"bybit",         0.00055f, 0.00020f, 5.00f, 0.00f},
    {"polymarket",    0.0000f, 0.0000f,   0.01f, 0.00f},  /* 0% fee, gas cost separate */
    {"robinhood",     0.0000f, 0.0000f,   1.00f, 0.00f},  /* 0% commission, PFOF */
    {"kucoin",        0.0010f, 0.0008f,   5.00f, 0.00f},
    {"okx",           0.0008f, 0.0005f,  10.00f, 0.00f},
    {NULL, 0, 0, 0, 0}
};

/* ── Slippage model ── */
/* Baseline slippage + market impact based on position size vs liquidity */
#define SLIPPAGE_BASELINE_BPS  5.0f    /* 5 bps = 0.05% baseline */
#define SLIPPAGE_IMPACT_BPS    2.0f    /* Addl bps per $100 notional */
#define SLIPPAGE_VOLATILE_BPS  15.0f   /* Addl during volatile periods */

/* ── Gas cost estimates (Polymarket, on-chain) ── */
#define GAS_ETH_MAINNET_USD    3.50f   /* ~$3.50 per settlement */
#define GAS_POLYGON_USD        0.15f   /* ~$0.15 per tx on Polygon */

/* ── API ── */

/* Lookup exchange by name, returns NULL if not found */
const ExchangeFees *exchange_lookup(const char *name) {
    for (int i = 0; EXCHANGES[i].name; i++) {
        if (strcasecmp(EXCHANGES[i].name, name) == 0)
            return &EXCHANGES[i];
    }
    return NULL;
}

/* Calculate total fee for a trade on a given exchange */
float exchange_calc_fee(const ExchangeFees *ex, float trade_size, int is_taker) {
    if (!ex || trade_size <= 0) return 0;
    float rate = is_taker ? ex->taker_fee : ex->maker_fee;
    float fee = trade_size * rate;
    if (ex->min_fee > 0 && fee < ex->min_fee) fee = ex->min_fee;
    return fee;
}

/* Calculate slippage in dollars for a given trade size */
/* Uses baseline + market impact model */
float exchange_calc_slippage(float trade_size, int is_volatile) {
    float bps = SLIPPAGE_BASELINE_BPS;
    /* Market impact: larger positions move the market more */
    float notional_k = trade_size / 100.0f;
    bps += notional_k * SLIPPAGE_IMPACT_BPS;
    /* Volatile period surcharge */
    if (is_volatile) bps += SLIPPAGE_VOLATILE_BPS;
    /* Convert bps to dollar cost */
    return trade_size * (bps / 10000.0f);
}

/* Calculate total trade cost (fee + slippage + gas if applicable) */
float exchange_total_cost(const ExchangeFees *ex, float trade_size,
                          int is_taker, int is_volatile, int is_onchain) {
    float fee = exchange_calc_fee(ex, trade_size, is_taker);
    float slippage = exchange_calc_slippage(trade_size, is_volatile);
    float gas = is_onchain ? GAS_POLYGON_USD : 0;
    return fee + slippage + gas;
}

/* Validate if trade_size is above exchange minimum */
int exchange_min_check(const ExchangeFees *ex, float trade_size) {
    if (!ex) return 0;
    return trade_size >= ex->min_trade;
}

/* ── Main (standalone test) ── */
int main(int argc, char **argv) {
    const char *ex_name = argc > 1 ? argv[1] : "kraken";
    float trade_size = argc > 2 ? atof(argv[2]) : 100.0f;
    int is_taker = argc > 3 ? atoi(argv[3]) : 1;

    const ExchangeFees *ex = exchange_lookup(ex_name);
    if (!ex) {
        /* List all */
        printf("Available exchanges:\n");
        for (int i = 0; EXCHANGES[i].name; i++) {
            printf("  %-15s taker=%.4f maker=%.4f min=$%.2f minfee=$%.2f\n",
                   EXCHANGES[i].name, EXCHANGES[i].taker_fee,
                   EXCHANGES[i].maker_fee, EXCHANGES[i].min_trade,
                   EXCHANGES[i].min_fee);
        }
        return 0;
    }

    printf("Exchange:    %s\n", ex->name);
    printf("Trade size:  $%.2f\n", trade_size);
    printf("Side:        %s\n", is_taker ? "TAKER (market)" : "MAKER (limit)");
    printf("Fee rate:    %.4f (%.2f%%)\n", is_taker ? ex->taker_fee : ex->maker_fee,
           100 * (is_taker ? ex->taker_fee : ex->maker_fee));
    printf("Fee cost:    $%.4f\n", exchange_calc_fee(ex, trade_size, is_taker));
    printf("Slippage:    $%.4f (normal)\n", exchange_calc_slippage(trade_size, 0));
    printf("Slippage:    $%.4f (volatile)\n", exchange_calc_slippage(trade_size, 1));
    printf("Total cost:  $%.4f\n", exchange_total_cost(ex, trade_size, is_taker, 0, 0));
    printf("Total cost:  $%.4f (volatile, on-chain)\n", exchange_total_cost(ex, trade_size, is_taker, 1, 1));
    printf("Min trade:   %s ($%.2f vs $%.2f min)\n",
           exchange_min_check(ex, trade_size) ? "PASS" : "FAIL",
           trade_size, ex->min_trade);

    /* Compare all exchanges for this trade size */
    printf("\n--- Comparison across all exchanges ---\n");
    printf("%-15s %12s %12s %12s %s\n", "Exchange", "Fee", "Slippage", "Total", "Min?");
    for (int i = 0; EXCHANGES[i].name; i++) {
        float fee = exchange_calc_fee(&EXCHANGES[i], trade_size, is_taker);
        float slip = exchange_calc_slippage(trade_size, 0);
        float total = fee + slip;
        int min_ok = exchange_min_check(&EXCHANGES[i], trade_size);
        printf("%-15s $%9.4f $%9.4f $%9.4f %s\n",
               EXCHANGES[i].name, fee, slip, total, min_ok ? "OK" : "MIN FAIL");
    }

    return 0;
}
