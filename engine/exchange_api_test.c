/**
 * exchange_api_test.c — Quick integration test for exchange_api module
 * Compile: gcc -O3 -o exchange_api_test exchange_api_test.c exchange_api.o -lcurl -ljansson -lm
 */
#include <stdio.h>
#include "exchange_api.h"

int main(void) {
    printf("=== Exchange API Test ===\n\n");
    
    // Kraken BTC/USD
    printf("--- Kraken XBTUSD ---\n");
    ExchangeTicker kraken = fetch_kraken_ticker("XBTUSD", 10);
    if (kraken.has_data) {
        printf("  Price:  %.2f\n", kraken.price);
        printf("  Bid:    %.2f\n", kraken.bid);
        printf("  Ask:    %.2f\n", kraken.ask);
        printf("  Vol:    %.0f\n", kraken.volume_24h);
        printf("  High:   %.2f\n", kraken.high_24h);
        printf("  Low:    %.2f\n", kraken.low_24h);
        printf("  24h Chg: %.2f%%\n", kraken.change_24h);
    } else {
        printf("  FAILED\n");
    }
    
    // Binance BTC/USDT
    printf("\n--- Binance BTCUSDT ---\n");
    ExchangeTicker binance = fetch_binance_ticker("BTCUSDT", 10);
    if (binance.has_data) {
        printf("  Price:  %.2f\n", binance.price);
        printf("  Bid:    %.2f\n", binance.bid);
        printf("  Ask:    %.2f\n", binance.ask);
        printf("  Vol:    %.0f\n", binance.volume_24h);
        printf("  High:   %.2f\n", binance.high_24h);
        printf("  Low:    %.2f\n", binance.low_24h);
        printf("  24h Chg: %.2f%%\n", binance.change_24h);
    } else {
        printf("  FAILED\n");
    }
    
    // Coinbase BTC-USD
    printf("\n--- Coinbase BTC-USD ---\n");
    ExchangeTicker coinbase = fetch_coinbase_ticker("BTC-USD", 10);
    if (coinbase.has_data) {
        printf("  Price:  %.2f\n", coinbase.price);
        printf("  Bid:    %.2f\n", coinbase.bid);
        printf("  Ask:    %.2f\n", coinbase.ask);
        printf("  Vol:    %.0f\n", coinbase.volume_24h);
    } else {
        printf("  FAILED\n");
    }
    
    // Cross-exchange spread
    printf("\n--- Cross-Exchange Spread ---\n");
    double spread = exchange_spread_pct(kraken.price, binance.price);
    printf("  Kraken-Binance spread: %.4f%%\n", spread);
    
    printf("\n=== Test Complete ===\n");
    return 0;
}
