/**
 * register_processor.c — Account registration & key management
 *
 * Processes pending registrations, generates unique API keys,
 * maintains .auth.json with tier/expiration tracking.
 *
 * Build: gcc -O2 -o register_processor register_processor.c -lcrypto -ljansson
 * Usage: ./register_processor [--add-key email tier]
 *        ./register_processor              (processes pending registrations)
 *        ./register_processor --list       (list all active keys)
 *        ./register_processor --cleanup    (remove expired trial keys)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define AUTH_PATH "/home/wubu2/money-room/docs/.auth.json"
#define PENDING_PATH "/home/wubu2/money-room/docs/data/pending_registrations.json"
#define MAX_KEYS 4096

static void hex_encode(const unsigned char *in, int len, char *out) {
    for (int i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", in[i]);
}

/* Generate random API key: mr_<16 random bytes hex> */
static void generate_key(char *buf, int bufsz) {
    unsigned char rnd[16];
    RAND_bytes(rnd, sizeof(rnd));
    char hex[33];
    hex_encode(rnd, 16, hex);
    snprintf(buf, bufsz, "mr_%s", hex);
}

/* Base64 encode for hash storage */
static void base64_encode(const unsigned char *in, int len, char *out) {
    EVP_EncodeBlock((unsigned char*)out, in, len);
}

static int key_exists(json_t *root, const char *hash) {
    json_t *keys = json_object_get(root, "keys");
    if (!keys || !json_is_array(keys)) return 0;
    size_t idx;
    json_t *val;
    json_array_foreach(keys, idx, val) {
        if (json_is_string(val)) {
            if (strcmp(json_string_value(val), hash) == 0) return 1;
        } else if (json_is_object(val)) {
            json_t *h = json_object_get(val, "hash");
            if (h && strcmp(json_string_value(h), hash) == 0) return 1;
        }
    }
    return 0;
}

static json_t *load_json(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 2 || len > 1048576) { fclose(f); return NULL; }
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    json_error_t err;
    json_t *root = json_loads(buf, 0, &err);
    free(buf);
    return root;
}

static int save_json(const char *path, json_t *root) {
    char *dump = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS);
    if (!dump) return 0;
    FILE *f = fopen(path, "w");
    if (!f) { free(dump); return 0; }
    fputs(dump, f);
    fclose(f);
    free(dump);
    return 1;
}

/* Add a new key to .auth.json with tier info */
static int add_key(const char *email, const char *tier, int expires_in_hours) {
    json_t *root = load_json(AUTH_PATH);
    if (!root) {
        root = json_object();
        json_object_set_new(root, "keys", json_array());
    }

    /* Ensure keys is an array of objects */
    json_t *keys = json_object_get(root, "keys");
    if (!keys || !json_is_array(keys)) {
        json_object_set_new(root, "keys", json_array());
        keys = json_object_get(root, "keys");
    }

    /* Generate new key */
    char keybuf[64], hashbuf[64];
    generate_key(keybuf, sizeof(keybuf));
    base64_encode((unsigned char*)keybuf, strlen(keybuf), hashbuf);

    /* Skip if already exists (shouldn't happen with 128-bit random) */
    if (key_exists(root, hashbuf)) return -1;

    /* Build key object */
    json_t *entry = json_object();
    json_object_set_new(entry, "hash", json_string(hashbuf));
    json_object_set_new(entry, "tier", json_string(tier ? tier : "trial_1d"));
    json_object_set_new(entry, "email", json_string(email ? email : "anonymous"));
    json_object_set_new(entry, "created", json_integer(time(NULL)));
    if (expires_in_hours > 0) {
        json_object_set_new(entry, "expires", json_integer(time(NULL) + expires_in_hours * 3600));
    } else {
        json_object_set_new(entry, "expires", json_null());
    }
    json_object_set_new(entry, "active", json_true());

    json_array_append_new(keys, entry);

    int ok = save_json(AUTH_PATH, root);
    json_decref(root);

    if (ok) {
        printf("✅ Key generated\n");
        printf("   Key:    %s\n", keybuf);
        printf("   Email:  %s\n", email ? email : "anonymous");
        printf("   Tier:   %s\n", tier ? tier : "trial_1d");
        if (expires_in_hours > 0)
            printf("   Expires: %d hours\n", expires_in_hours);
        else
            printf("   Expires: never\n");
        return 0;
    }
    return -1;
}

/* Process pending registrations from pending_registrations.json */
static int process_pending(void) {
    json_t *pending = load_json(PENDING_PATH);
    if (!pending) {
        printf("[REG] No pending registrations\n");
        return 0;
    }

    json_t *regs = json_object_get(pending, "registrations");
    if (!regs || !json_is_array(regs) || json_array_size(regs) == 0) {
        json_decref(pending);
        printf("[REG] No pending registrations\n");
        return 0;
    }

    int processed = 0;
    size_t idx;
    json_t *entry;
    json_array_foreach(regs, idx, entry) {
        const char *email = json_string_value(json_object_get(entry, "email"));
        const char *tier = json_string_value(json_object_get(entry, "tier"));
        if (!tier) tier = "trial_1d";
        int expires = json_integer_value(json_object_get(entry, "expires_in"));
        if (expires <= 0) expires = 24; /* default: 24h trial */

        printf("[REG] Processing: %s (tier=%s, expires=%dh)\n", email ? email : "anon", tier, expires);
        int ret = add_key(email, tier, expires);
        if (ret == 0) processed++;
        else printf("[REG]  SKIP (key exists or error)\n");
    }

    /* Clear pending registrations */
    json_object_set_new(pending, "registrations", json_array());
    save_json(PENDING_PATH, pending);
    json_decref(pending);

    printf("[REG] Processed %d registrations\n", processed);
    return processed;
}

/* List all active keys */
static int list_keys(void) {
    json_t *root = load_json(AUTH_PATH);
    if (!root) { printf("No auth file\n"); return 0; }

    json_t *keys = json_object_get(root, "keys");
    if (!keys || !json_is_array(keys)) { printf("No keys\n"); json_decref(root); return 0; }

    time_t now = time(NULL);
    int count = 0;
    size_t idx;
    json_t *entry;
    json_array_foreach(keys, idx, entry) {
        count++;
        const char *hash = json_string_value(json_object_get(entry, "hash"));
        const char *tier = json_string_value(json_object_get(entry, "tier"));
        const char *email = json_string_value(json_object_get(entry, "email"));
        json_t *exp = json_object_get(entry, "expires");
        time_t expires = json_is_integer(exp) ? json_integer_value(exp) : 0;
        json_t *act = json_object_get(entry, "active");
        int active = json_is_true(act);

        char expiry[32] = "never";
        if (expires > 0) {
            if (expires < now) snprintf(expiry, 32, "EXPIRED");
            else snprintf(expiry, 32, "%dh", (int)((expires - now) / 3600));
        }

        printf("  %-8s | %-12s | %-25s | %s | %s\n",
               hash ? hash + 4 : "?",  /* show last part of hash */
               tier ? tier : "?",
               email ? email : "?",
               expiry,
               active ? "active" : "disabled");
    }
    json_decref(root);
    printf("  Total: %d keys\n", count);
    return count;
}

/* Clean up expired trial keys */
static int cleanup_expired(void) {
    json_t *root = load_json(AUTH_PATH);
    if (!root) return 0;

    json_t *keys = json_object_get(root, "keys");
    if (!keys || !json_is_array(keys)) { json_decref(root); return 0; }

    time_t now = time(NULL);
    json_t *new_keys = json_array();
    int removed = 0;

    size_t idx;
    json_t *entry;
    json_array_foreach(keys, idx, entry) {
        json_t *exp = json_object_get(entry, "expires");
        time_t expires = json_is_integer(exp) ? json_integer_value(exp) : 0;
        if (expires > 0 && expires < now) {
            removed++;
            continue; /* skip expired keys */
        }
        json_array_append(new_keys, entry);
    }

    json_object_set(root, "keys", new_keys);
    save_json(AUTH_PATH, root);
    json_decref(root);

    printf("[REG] Cleaned %d expired keys\n", removed);
    return removed;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        return list_keys() ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "--cleanup") == 0) {
        return cleanup_expired();
    }
    if (argc > 2 && strcmp(argv[1], "--add-key") == 0) {
        /* --add-key email tier [expires_hours] */
        const char *email = argc > 2 ? argv[2] : "anonymous";
        const char *tier = argc > 3 ? argv[3] : "trial_1d";
        int hours = argc > 4 ? atoi(argv[4]) : 24;
        return add_key(email, tier, hours) ? 1 : 0;
    }

    /* Default: process pending registrations */
    printf("[REG] Registration Processor v1\n");
    int n = process_pending();
    if (n > 0) cleanup_expired(); /* clean old expired keys when adding new ones */
    return 0;
}
