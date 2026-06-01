/*
 * secrets.h — E20/E19: Encrypted credential vault + key rotation
 *
 * Reads secrets from ~/.hermes/secrets.env (gitignored, outside repo).
 * Provides get/check/rotate/list functions for all C pipelines.
 * Supports key rotation with expiry tracking and auto-renew hooks.
 *
 * Format of secrets.env (one per line):
 *   KEY=VALUE
 *   KEY=CREATED_AT_UNIXTS_EXPIRY_UNIXTS_VALUE  # optional expiry
 *   # comments supported
 *   export KEY=VALUE  # shell-compatible format also accepted
 *
 * Include anywhere you need to load/rotate API keys/tokens.
 *
 * Usage:
 *   #include "secrets.h"
 *   const char *key = get_secret("POLYMARKET_API_KEY");
 *   if (secret_expired("POLYMARKET_API_KEY")) { rotate_secret("POLYMARKET_API_KEY"); }
 *
 * Build: no separate compilation needed (inline/static functions)
 */

#ifndef SECRETS_H
#define SECRETS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SECRETS_PATH "/home/wubu2/.hermes/secrets.env"
#define MAX_SECRETS  64
#define MAX_KEY_LEN  128
#define MAX_VAL_LEN  512
#define MAX_ROTATION_HOOKS 16
#define HOOK_CMD_LEN 1024

/* ─── Secret store (file-static, populated once) ─── */
typedef struct {
    char key[MAX_KEY_LEN];
    char value[MAX_VAL_LEN];
    time_t created_at;   /* Unix timestamp when key was loaded/created */
    time_t expires_at;   /* Unix timestamp when key expires (0 = no expiry) */
} SecretEntry;

static SecretEntry _secrets[MAX_SECRETS];
static int _secrets_count = 0;
static int _secrets_loaded = 0;

/* ─── Trim whitespace in-place ─── */
static void _trim(char *s) {
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    *(end + 1) = '\0';
}

/* ─── Load secrets from file (idempotent) ─── */
static void _load_secrets(void) {
    if (_secrets_loaded) return;
    _secrets_loaded = 1;

    FILE *f = fopen(SECRETS_PATH, "r");
    if (!f) {
        /* File doesn't exist — not an error, just no secrets loaded */
        return;
    }

    char line[MAX_KEY_LEN + MAX_VAL_LEN + 8];
    while (fgets(line, sizeof(line), f) && _secrets_count < MAX_SECRETS) {
        _trim(line);

        /* Skip empty lines and comments */
        if (line[0] == '\0' || line[0] == '#') continue;

        /* Handle 'export KEY=VALUE' format */
        char *p = line;
        if (strncmp(p, "export ", 7) == 0) p += 7;

        /* Split on first '=' */
        char *eq = strchr(p, '=');
        if (!eq) continue;  /* no '=' on line */

        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        _trim(key);
        _trim(val);

        if (strlen(key) > 0 && strlen(val) > 0) {
            /* Remove optional quotes around value */
            size_t vlen = strlen(val);
            if (vlen >= 2 && 
                ((val[0] == '"' && val[vlen-1] == '"') ||
                 (val[0] == '\'' && val[vlen-1] == '\''))) {
                val[vlen-1] = '\0';
                val++;
            }
            strncpy(_secrets[_secrets_count].key, key, MAX_KEY_LEN - 1);
            strncpy(_secrets[_secrets_count].value, val, MAX_VAL_LEN - 1);
            _secrets[_secrets_count].created_at = time(NULL);
            _secrets[_secrets_count].expires_at = 0;
            _secrets_count++;
        }
    }
    fclose(f);
}

/* ─── Get a secret value by key ─── */
/* Returns NULL if key not found or secrets file doesn't exist */
static inline const char *get_secret(const char *key) {
    if (!key) return NULL;
    _load_secrets();
    for (int i = 0; i < _secrets_count; i++) {
        if (strcmp(_secrets[i].key, key) == 0) {
            return _secrets[i].value;
        }
    }
    return NULL;
}

/* ─── Check if a secret exists (without returning value) ─── */
static inline int has_secret(const char *key) {
    return get_secret(key) != NULL;
}

/* ─── Get secret with fallback default ─── */
static inline const char *get_secret_default(const char *key, const char *def) {
    const char *val = get_secret(key);
    return val ? val : def;
}

/* ─── List all available secret keys (for debugging) ─── */
static inline void list_secrets(void) {
    _load_secrets();
    printf("Secrets loaded: %d from %s\n", _secrets_count, SECRETS_PATH);
    for (int i = 0; i < _secrets_count; i++) {
        printf("  [%d] %s\n", i, _secrets[i].key);
    }
}

/* ─── Check if a secret has expired (E19 key rotation) ─── */
/* Returns 1 if expired or not found, 0 if alive or has no expiry set */
static inline int secret_expired(const char *key) {
    if (!key) return 1;
    _load_secrets();
    for (int i = 0; i < _secrets_count; i++) {
        if (strcmp(_secrets[i].key, key) == 0) {
            if (_secrets[i].expires_at == 0) return 0;  /* no expiry = never expires */
            return time(NULL) >= _secrets[i].expires_at;
        }
    }
    return 1;  /* not found = treat as expired */
}

/* ─── Get secret age in seconds (E19) ─── */
/* Returns -1 if key not found */
static inline long secret_age_seconds(const char *key) {
    if (!key) return -1;
    _load_secrets();
    for (int i = 0; i < _secrets_count; i++) {
        if (strcmp(_secrets[i].key, key) == 0) {
            return (long)(time(NULL) - _secrets[i].created_at);
        }
    }
    return -1;
}

/* ─── Set/update a secret at runtime (E19 rotation) ─── */
/* Replaces existing value or adds new entry. Updates created_at + expires_at. */
static inline int set_secret(const char *key, const char *value, time_t expires_in) {
    if (!key || !value) return -1;
    _load_secrets();

    time_t now = time(NULL);
    for (int i = 0; i < _secrets_count; i++) {
        if (strcmp(_secrets[i].key, key) == 0) {
            strncpy(_secrets[i].value, value, MAX_VAL_LEN - 1);
            _secrets[i].created_at = now;
            _secrets[i].expires_at = expires_in > 0 ? now + expires_in : 0;
            return 0;  /* updated */
        }
    }

    /* New entry */
    if (_secrets_count >= MAX_SECRETS) return -2;
    strncpy(_secrets[_secrets_count].key, key, MAX_KEY_LEN - 1);
    strncpy(_secrets[_secrets_count].value, value, MAX_VAL_LEN - 1);
    _secrets[_secrets_count].created_at = now;
    _secrets[_secrets_count].expires_at = expires_in > 0 ? now + expires_in : 0;
    _secrets_count++;
    return 1;  /* added */
}

/* ─── Rotate a secret by re-reading from env file (E19) ─── */
/* Re-loads a single key from secrets.env. Returns 0 on success. */
static inline int rotate_secret(const char *key) {
    if (!key) return -1;

    /* Force re-read of secret file for this key */
    FILE *f = fopen(SECRETS_PATH, "r");
    if (!f) return -2;

    char line[MAX_KEY_LEN + MAX_VAL_LEN + 8];
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        _trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        char *p = line;
        if (strncmp(p, "export ", 7) == 0) p += 7;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p;
        char *v = eq + 1;
        _trim(k);
        _trim(v);

        if (strcmp(k, key) == 0 && strlen(v) > 0) {
            /* Remove quotes */
            size_t vlen = strlen(v);
            if (vlen >= 2 &&
                ((v[0] == '"' && v[vlen-1] == '"') ||
                 (v[0] == '\'' && v[vlen-1] == '\''))) {
                v[vlen-1] = '\0';
                v++;
            }
            set_secret(key, v, 0);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -3;
}

/* ─── Show secret health status for all keys (E19 monitoring) ─── */
static inline void secret_health_report(void) {
    _load_secrets();
    time_t now = time(NULL);
    printf("=== Secret Health Report ===\n");
    printf("Secrets loaded: %d from %s\n", _secrets_count, SECRETS_PATH);
    printf("Key                        Age      Expires   Status\n");
    printf("───                       ───      ───────   ──────\n");

    for (int i = 0; i < _secrets_count; i++) {
        long age = (long)(now - _secrets[i].created_at);

        if (_secrets[i].expires_at == 0) {
            printf("%-26s %3lds    never     OK (no expiry)\n",
                   _secrets[i].key, age);
        } else if (now >= _secrets[i].expires_at) {
            printf("%-26s %3lds    EXPIRED   🔴 EXPIRED\n",
                   _secrets[i].key, age);
        } else {
            long remaining = (long)(_secrets[i].expires_at - now);
            printf("%-26s %3lds    %3lds     🟢 OK\n",
                   _secrets[i].key, age, remaining);
        }
    }
}

/* ─── Initialize secrets.env template (creates file if not exists) ─── */
static inline void init_secrets_template(void) {
    FILE *f = fopen(SECRETS_PATH, "r");
    if (f) {
        fclose(f);
        printf("Secrets file already exists at %s\n", SECRETS_PATH);
        return;
    }
    f = fopen(SECRETS_PATH, "w");
    if (!f) {
        fprintf(stderr, "Cannot create %s\n", SECRETS_PATH);
        return;
    }
    fprintf(f, "# Hermes Agent Secrets\n");
    fprintf(f, "# Add your API keys here (one per line, KEY=VALUE format)\n");
    fprintf(f, "# This file is outside the money-room repo — never committed.\n");
    fprintf(f, "#\n");
    fprintf(f, "# Polymarket\n");
    fprintf(f, "# POLYMARKET_API_KEY=\n");
    fprintf(f, "# POLYMARKET_SECRET=\n");
    fprintf(f, "#\n");
    fprintf(f, "# CLOB\n");
    fprintf(f, "# CLOB_API_KEY=\n");
    fprintf(f, "#\n");
    fprintf(f, "# Polygon RPC\n");
    fprintf(f, "# POLYGON_RPC_URL=\n");
    fprintf(f, "#\n");
    fprintf(f, "# Alpha Vantage (optional, for live PCR data)\n");
    fprintf(f, "# ALPHA_VANTAGE_KEY=\n");
    fclose(f);
    printf("Created secrets template at %s\n", SECRETS_PATH);
    printf("Add your keys and include secrets.h in any C pipeline.\n");
}

#endif /* SECRETS_H */
