#ifndef EXCHANGE_API_H
#define EXCHANGE_API_H

#ifdef __cplusplus
extern "C" {
#endif

// ── Exchange Ticker (public data, no keys needed) ──
typedef struct {
    double price;        // Last traded price
    double volume_24h;   // 24h volume
    double high_24h;     // 24h high
    double low_24h;      // 24h low
    double bid;          // Best bid
    double ask;          // Best ask
    double change_24h;   // 24h price change %
    int    has_data;     // 1 if data was fetched successfully
} ExchangeTicker;

// ── Exchange API Key Config (for private endpoints — empty by default) ──
typedef struct {
    char kraken_key[128];
    char kraken_secret[256];
    char binance_key[128];
    char binance_secret[256];
    char coinbase_key[128];
    char coinbase_secret[256];
    char coinbase_passphrase[128];
} ExchangeConfig;

// ── Public ticker fetchers (no keys required) ──
ExchangeTicker fetch_kraken_ticker(const char *pair, int timeout_sec);
ExchangeTicker fetch_binance_ticker(const char *symbol, int timeout_sec);
ExchangeTicker fetch_coinbase_ticker(const char *product_id, int timeout_sec);

// ── Config management ──
void exchange_config_init(ExchangeConfig *cfg);
int  exchange_config_load(ExchangeConfig *cfg, const char *path);
void exchange_config_print_status(const ExchangeConfig *cfg);

// ── Utility: cross-exchange price spread ──
double exchange_spread_pct(double price_a, double price_b);

#ifdef __cplusplus
}
#endif

#endif /* EXCHANGE_API_H */
