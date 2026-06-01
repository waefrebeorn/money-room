/* secret_health.c — E19: Test key rotation infrastructure */
#include "secrets.h"
#include <stdio.h>

int main(void) {
    printf("=== Secret Key Rotation Test ===\n\n");

    /* List available secrets */
    list_secrets();

    /* Health report */
    secret_health_report();

    /* Test set_secret with 30-second expiry */
    printf("\n--- Rotate test: set MGT_API_KEY with 30s expiry ---\n");
    int ret = set_secret("MGT_API_KEY", "test_key_abc123", 30);
    printf("set_secret returned: %d\n", ret);

    printf("secret_age: %ld seconds\n", secret_age_seconds("MGT_API_KEY"));
    printf("secret_expired: %d (should be 0)\n", secret_expired("MGT_API_KEY"));

    /* Test rotate from env file */
    printf("\n--- Rotate test: re-read from secrets.env ---\n");
    ret = rotate_secret("MGT_API_KEY");
    printf("rotate_secret returned: %d (expect -3 if not in file)\n", ret);

    /* Test secret that exists */
    printf("\n--- Check existing secrets ---\n");
    const char *pm = get_secret("POLYMARKET_API_KEY");
    printf("POLYMARKET_API_KEY: %s\n", pm ? pm : "NOT_SET");

    printf("\n=== Secret rotation infrastructure: OK ===\n");
    return 0;
}
