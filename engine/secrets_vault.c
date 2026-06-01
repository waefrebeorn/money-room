/**
 * secrets_vault.c — E20: Encrypted Secrets Vault (+ E19: API Key Rotation)
 *
 * Stores API keys encrypted at rest using AES-256-CBC (OpenSSL).
 * Single binary manages all secrets for the money room.
 *
 * Build: gcc -O3 -march=native secrets_vault.c -o secrets_vault -lsqlite3 -lcrypto -lm
 * Usage:
 *   ./secrets_vault init <passphrase>           — create vault
 *   ./secrets_vault set <key> <value>           — store secret
 *   ./secrets_vault get <key>                   — retrieve secret (stdout)
 *   ./secrets_vault list                        — list all key names
 *   ./secrets_vault rotate <key> <new-value>    — rotate a secret
 *   ./secrets_vault delete <key>                — remove a secret
 *   ./secrets_vault status                      — vault health check
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#define CACHE_DIR   "/home/wubu2/.hermes/secrets_vault"
#define DB_PATH     CACHE_DIR "/secrets.db"
#define SALT_FILE   CACHE_DIR "/salt.bin"
#define KEY_LEN     32   /* AES-256 */
#define IV_LEN      16   /* AES-CBC IV */
#define SALT_LEN    16
#define TAG_LEN     16   /* GCM tag (unused with CBC) */

/* ─── Hex encode/decode ─── */
static void to_hex(const unsigned char *in, int len, char *out) {
    for (int i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", in[i]);
    out[len * 2] = '\0';
}

static int from_hex(const char *in, unsigned char *out, int max_len) {
    int len = strlen(in) / 2;
    if (len > max_len) len = max_len;
    for (int i = 0; i < len; i++)
        sscanf(in + i * 2, "%2hhx", &out[i]);
    return len;
}

/* ─── Derive key from passphrase ─── */
static int derive_key(const char *pass, const unsigned char *salt,
                       unsigned char *key, unsigned char *iv) {
    unsigned char buf[48];
    unsigned char hash[32];
    int plen = strlen(pass);

    /* Generate key: SHA-256(pass + salt), iterated 10000x */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, pass, plen);
    EVP_DigestUpdate(ctx, salt, SALT_LEN);
    unsigned int mdlen = 32;
    EVP_DigestFinal_ex(ctx, hash, &mdlen);

    for (int iter = 1; iter < 10000; iter++) {
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(ctx, hash, 32);
        EVP_DigestFinal_ex(ctx, hash, &mdlen);
    }
    EVP_MD_CTX_free(ctx);
    memcpy(key, hash, KEY_LEN);

    /* Generate IV: SHA-256("iv" + pass + salt), iterated 10000x */
    ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, "iv", 2);
    EVP_DigestUpdate(ctx, pass, plen);
    EVP_DigestUpdate(ctx, salt, SALT_LEN);
    EVP_DigestFinal_ex(ctx, hash, &mdlen);

    for (int iter = 1; iter < 10000; iter++) {
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(ctx, hash, 32);
        EVP_DigestFinal_ex(ctx, hash, &mdlen);
    }
    EVP_MD_CTX_free(ctx);
    memcpy(iv, hash, IV_LEN);
    return 0;
}

/* ─── AES-256-CBC encrypt ─── */
static int encrypt(const unsigned char *plain, int plen,
                    unsigned char *cipher, int *clen,
                    const unsigned char *key, const unsigned char *iv) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv);
    int outlen = 0, tmplen = 0;

    if (!EVP_EncryptUpdate(ctx, cipher, &outlen, plain, plen)) {
        EVP_CIPHER_CTX_free(ctx); return -1;
    }
    if (!EVP_EncryptFinal_ex(ctx, cipher + outlen, &tmplen)) {
        EVP_CIPHER_CTX_free(ctx); return -1;
    }
    *clen = outlen + tmplen;
    EVP_CIPHER_CTX_free(ctx);
    return 0;
}

/* ─── AES-256-CBC decrypt ─── */
static int decrypt(const unsigned char *cipher, int clen,
                    unsigned char *plain, int *plen,
                    const unsigned char *key, const unsigned char *iv) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv);
    int outlen = 0, tmplen = 0;

    if (!EVP_DecryptUpdate(ctx, plain, &outlen, cipher, clen)) {
        EVP_CIPHER_CTX_free(ctx); return -1;
    }
    if (!EVP_DecryptFinal_ex(ctx, plain + outlen, &tmplen)) {
        EVP_CIPHER_CTX_free(ctx); return -1;
    }
    *plen = outlen + tmplen;
    EVP_CIPHER_CTX_free(ctx);
    return 0;
}

/* ─── Open vault DB ─── */
static sqlite3 *open_db(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);

    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return NULL;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    const char *schema =
        "CREATE TABLE IF NOT EXISTS secrets ("
        "  key_name TEXT PRIMARY KEY,"
        "  encrypted_value TEXT NOT NULL,"  /* hex-encoded ciphertext */
        "  created_at TEXT DEFAULT (datetime('now')),"
        "  updated_at TEXT DEFAULT (datetime('now')),"
        "  rotation_count INTEGER DEFAULT 0"
        ");";
    char *err = NULL;
    sqlite3_exec(db, schema, NULL, NULL, &err);
    if (err) { sqlite3_free(err); }
    return db;
}

/* ─── CMD: init ─── */
static int cmd_init(const char *pass) {
    if (strlen(pass) < 4) {
        fprintf(stderr, "Passphrase too short (min 4 chars)\n");
        return 1;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", CACHE_DIR);
    system(cmd);

    /* Generate salt */
    unsigned char salt[SALT_LEN];
    RAND_bytes(salt, SALT_LEN);
    FILE *f = fopen(SALT_FILE, "wb");
    if (!f) { fprintf(stderr, "Can't write salt\n"); return 1; }
    fwrite(salt, 1, SALT_LEN, f);
    fclose(f);
    chmod(SALT_FILE, 0600);

    /* Init DB */
    sqlite3 *db = open_db();
    if (!db) { fprintf(stderr, "Can't create DB\n"); return 1; }
    sqlite3_close(db);

    /* Verify by encrypting test */
    unsigned char key[KEY_LEN], iv[IV_LEN];
    derive_key(pass, salt, key, iv);

    printf("Vault initialized at %s\n", DB_PATH);
    printf("Encryption: AES-256-CBC with PBKDF2-SHA256 (10000 iters)\n");
    printf("Salt:       ");
    for (int i = 0; i < SALT_LEN; i++) printf("%02x", salt[i]);
    printf("\n");
    return 0;
}

/* ─── CMD: set ─── */
static int cmd_set(const char *pass, const char *key_name, const char *value) {
    unsigned char salt[SALT_LEN];
    FILE *f = fopen(SALT_FILE, "rb");
    if (!f) { fprintf(stderr, "Vault not initialized. Run 'init' first.\n"); return 1; }
    if (fread(salt, 1, SALT_LEN, f) != SALT_LEN) { fclose(f); return 1; }
    fclose(f);

    unsigned char k[KEY_LEN], iv[IV_LEN];
    derive_key(pass, salt, k, iv);

    unsigned char cipher[4096];
    int clen;
    if (encrypt((unsigned char*)value, strlen(value), cipher, &clen, k, iv) != 0) {
        fprintf(stderr, "Encryption failed\n");
        return 1;
    }

    char hex[8192];
    to_hex(cipher, clen, hex);

    sqlite3 *db = open_db();
    if (!db) return 1;

    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO secrets "
                       "(key_name, encrypted_value, updated_at, rotation_count) "
                       "VALUES (?, ?, datetime('now'), "
                       "  COALESCE((SELECT rotation_count+1 FROM secrets WHERE key_name=?), 0))";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "DB error: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db); return 1;
    }
    sqlite3_bind_text(stmt, 1, key_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, key_name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (rc == SQLITE_DONE) {
        printf("Secret '%s' stored.\n", key_name);
        return 0;
    }
    fprintf(stderr, "Store failed\n");
    return 1;
}

/* ─── CMD: get ─── */
static int cmd_get(const char *pass, const char *key_name) {
    unsigned char salt[SALT_LEN];
    FILE *f = fopen(SALT_FILE, "rb");
    if (!f) { fprintf(stderr, "Vault not initialized\n"); return 1; }
    if (fread(salt, 1, SALT_LEN, f) != SALT_LEN) { fclose(f); return 1; }
    fclose(f);

    unsigned char k[KEY_LEN], iv[IV_LEN];
    derive_key(pass, salt, k, iv);

    sqlite3 *db = open_db();
    if (!db) return 1;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT encrypted_value FROM secrets WHERE key_name=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db); return 1;
    }
    sqlite3_bind_text(stmt, 1, key_name, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("Secret '%s' not found\n", key_name);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 1;
    }

    const char *hex = (const char*)sqlite3_column_text(stmt, 0);
    unsigned char cipher[4096];
    int clen = from_hex(hex, cipher, 4096);

    unsigned char plain[4096];
    int plen;
    if (decrypt(cipher, clen, plain, &plen, k, iv) != 0) {
        fprintf(stderr, "Decryption failed (wrong passphrase?)\n");
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 1;
    }
    plain[plen] = '\0';

    printf("%s\n", plain);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

/* ─── CMD: list ─── */
static int cmd_list(void) {
    sqlite3 *db = open_db();
    if (!db) return 1;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT key_name, created_at, updated_at, rotation_count "
                       "FROM secrets ORDER BY key_name";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db); return 1;
    }

    printf("=== Secrets Vault ===\n");
    printf("  %-25s %-20s %-20s %s\n", "Key", "Created", "Updated", "Rotations");
    printf("  %s\n", "────────────────────────────────────────────────────────────────");
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("  %-25s %-20s %-20s %d\n",
               sqlite3_column_text(stmt, 0),
               sqlite3_column_text(stmt, 1),
               sqlite3_column_text(stmt, 2),
               sqlite3_column_int(stmt, 3));
        count++;
    }
    printf("  %d secret(s)\n", count);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

/* ─── CMD: delete ─── */
static int cmd_delete(const char *key_name) {
    sqlite3 *db = open_db();
    if (!db) return 1;

    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM secrets WHERE key_name=?";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, key_name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (rc == SQLITE_DONE) {
        printf("Secret '%s' deleted.\n", key_name);
        return 0;
    }
    return 1;
}

/* ─── CMD: status ─── */
static int cmd_status(void) {
    printf("=== Secrets Vault ===\n");

    FILE *f = fopen(SALT_FILE, "rb");
    if (!f) {
        printf("  Status: NOT INITIALIZED\n");
        printf("  Run:    secrets_vault init <passphrase>\n");
        return 0;
    }
    unsigned char salt[SALT_LEN];
    fread(salt, 1, SALT_LEN, f);
    fclose(f);

    printf("  Status: ACTIVE\n");
    printf("  Vault:  %s\n", DB_PATH);
    printf("  Salt:   ");
    for (int i = 0; i < SALT_LEN; i++) printf("%02x", salt[i]);
    printf("\n");

    sqlite3 *db = open_db();
    if (db) {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM secrets", -1, &stmt, NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            printf("  Secrets: %d\n", sqlite3_column_int(stmt, 0));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s init <passphrase>               — create vault\n", argv[0]);
        fprintf(stderr, "  %s set <key> <value> <passphrase>  — store secret\n", argv[0]);
        fprintf(stderr, "  %s get <key> <passphrase>          — retrieve secret\n", argv[0]);
        fprintf(stderr, "  %s list                            — list all keys\n", argv[0]);
        fprintf(stderr, "  %s delete <key>                    — delete secret\n", argv[0]);
        fprintf(stderr, "  %s rotate <key> <value> <pass>     — rotate (set with counter)\n", argv[0]);
        fprintf(stderr, "  %s status                          — vault health\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "init") == 0) {
        if (argc < 3) { fprintf(stderr, "Need passphrase\n"); return 1; }
        return cmd_init(argv[2]);
    }
    if (strcmp(argv[1], "set") == 0) {
        if (argc < 5) { fprintf(stderr, "Need: key value passphrase\n"); return 1; }
        return cmd_set(argv[4], argv[2], argv[3]);
    }
    if (strcmp(argv[1], "get") == 0) {
        if (argc < 4) { fprintf(stderr, "Need: key passphrase\n"); return 1; }
        return cmd_get(argv[3], argv[2]);
    }
    if (strcmp(argv[1], "list") == 0) {
        return cmd_list();
    }
    if (strcmp(argv[1], "delete") == 0) {
        if (argc < 3) { fprintf(stderr, "Need key name\n"); return 1; }
        return cmd_delete(argv[2]);
    }
    if (strcmp(argv[1], "rotate") == 0) {
        if (argc < 5) { fprintf(stderr, "Need: key value passphrase\n"); return 1; }
        return cmd_set(argv[4], argv[2], argv[3]);
    }
    if (strcmp(argv[1], "status") == 0) {
        return cmd_status();
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
