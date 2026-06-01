/**
 * env_manager.c — E21: Environment Separation (dev/test/prod)
 *
 * Manages environment-specific config profiles for the money room.
 * Each profile has its own settings, paths, and API endpoints.
 *
 * Features:
 *   - Three environments: dev, test, prod
 *   - Per-environment config overrides
 *   - Environment-aware path resolution
 *   - Profile switching and status
 *   - Can run a command under a specific environment
 *
 * Build: gcc -O3 -march=native env_manager.c -o env_manager -lm -ljansson
 * Usage:
 *   ./env_manager init           — create default config
 *   ./env_manager list            — list environments
 *   ./env_manager switch <env>    — set active environment
 *   ./env_manager status          — show current env
 *   ./env_manager set <env> <key> <val>  — set env config value
 *   ./env_manager get <key>       — get config value for current env
 *   ./env_manager exec <env> -- <cmd> [args]  — run cmd under env
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <ctype.h>
#include <jansson.h>

#define CONFIG_DIR  "/home/wubu2/.hermes/environments"
#define CONFIG_FILE CONFIG_DIR "/environments.json"
#define ACTIVE_FILE CONFIG_DIR "/.active"

static const char *default_envs =
"{\n"
"  \"environments\": {\n"
"    \"dev\": {\n"
"      \"label\": \"Development\",\n"
"      \"db_path\": \"/home/wubu2/.hermes/pm_logs/timeline.db\",\n"
"      \"state_path\": \"/home/wubu2/.hermes/pm_logs/dev/room_state.bin\",\n"
"      \"log_level\": \"DEBUG\",\n"
"      \"paper_trading\": true,\n"
"      \"engine_flags\": \"-DPAPER_MODE\",\n"
"      \"api_base\": \"https://api.sandbox.example.com\",\n"
"      \"description\": \"Local development with paper trading\"\n"
"    },\n"
"    \"test\": {\n"
"      \"label\": \"Testing/Staging\",\n"
"      \"db_path\": \"/home/wubu2/.hermes/pm_logs/timeline.db\",\n"
"      \"state_path\": \"/home/wubu2/.hermes/pm_logs/test/room_state.bin\",\n"
"      \"log_level\": \"INFO\",\n"
"      \"paper_trading\": true,\n"
"      \"engine_flags\": \"-DPAPER_MODE\",\n"
"      \"api_base\": \"https://api.test.example.com\",\n"
"      \"description\": \"Pre-production testing\"\n"
"    },\n"
"    \"prod\": {\n"
"      \"label\": \"Production\",\n"
"      \"db_path\": \"/home/wubu2/.hermes/pm_logs/timeline.db\",\n"
"      \"state_path\": \"/home/wubu2/.hermes/pm_logs/c_room/room_state.bin\",\n"
"      \"log_level\": \"WARNING\",\n"
"      \"paper_trading\": false,\n"
"      \"engine_flags\": \"\",\n"
"      \"api_base\": \"https://api.example.com\",\n"
"      \"description\": \"Live production with real capital\"\n"
"    }\n"
"  }\n"
"}\n";

/* ─── Ensure config dir exists ─── */
static void ensure_dir(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", CONFIG_DIR);
    system(cmd);
}

/* ─── Read config JSON ─── */
static json_t *read_config(void) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *data = malloc(sz + 1);
    fread(data, 1, sz, f);
    data[sz] = '\0';
    fclose(f);
    json_error_t err;
    json_t *j = json_loads(data, 0, &err);
    free(data);
    return j;
}

/* ─── Write config JSON ─── */
static int write_config(json_t *j) {
    char *out = json_dumps(j, JSON_INDENT(2));
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) { free(out); return 1; }
    fprintf(f, "%s\n", out);
    fclose(f);
    free(out);
    return 0;
}

/* ─── Get active env ─── */
static char *get_active(void) {
    FILE *f = fopen(ACTIVE_FILE, "r");
    if (!f) return strdup("dev"); /* default */
    char line[64];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return strdup("dev"); }
    fclose(f);
    size_t len = strlen(line);
    if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
    return strdup(line);
}

/* ─── CMD: init ─── */
static int cmd_init(void) {
    ensure_dir();
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) { fprintf(stderr, "Can't create config\n"); return 1; }
    fprintf(f, "%s", default_envs);
    fclose(f);
    chmod(CONFIG_FILE, 0600);
    printf("Environments initialized: dev, test, prod\n");
    return 0;
}

/* ─── CMD: list ─── */
static int cmd_list(void) {
    json_t *j = read_config();
    if (!j) { printf("No config. Run 'init' first.\n"); return 1; }
    json_t *envs = json_object_get(j, "environments");
    if (!envs) { json_decref(j); return 1; }

    char *active = get_active();
    printf("=== Environments ===\n");
    printf("  Active: %s\n\n", active);

    const char *key;
    json_t *val;
    json_object_foreach(envs, key, val) {
        const char *label = json_string_value(json_object_get(val, "label"));
        const char *desc = json_string_value(json_object_get(val, "description"));
        int paper = json_is_true(json_object_get(val, "paper_trading"));
        printf("  %s%s %s%s%s\n", key, active && strcmp(key, active) == 0 ? " ◀" : "",
               label ? label : "", paper ? " [PAPER]" : "", desc ? " — " : "");
        if (desc) printf("         %s\n", desc);
    }
    free(active);
    json_decref(j);
    return 0;
}

/* ─── CMD: switch ─── */
static int cmd_switch(const char *env) {
    json_t *j = read_config();
    if (!j) { printf("No config. Run 'init' first.\n"); return 1; }
    json_t *envs = json_object_get(j, "environments");
    if (!json_object_get(envs, env)) {
        printf("Unknown environment: %s (use: dev, test, prod)\n", env);
        json_decref(j);
        return 1;
    }
    ensure_dir();
    FILE *f = fopen(ACTIVE_FILE, "w");
    if (!f) { json_decref(j); return 1; }
    fprintf(f, "%s\n", env);
    fclose(f);
    printf("Switched to: %s\n", env);
    json_decref(j);
    return 0;
}

/* ─── CMD: status ─── */
static int cmd_status(void) {
    json_t *j = read_config();
    char *active = get_active();

    if (!j) {
        printf("=== Environment Status ===\n");
        printf("  Active: %s (no config)\n", active);
        printf("  Run 'init' to create config\n");
        free(active);
        return 0;
    }

    json_t *envs = json_object_get(j, "environments");
    json_t *cur = json_object_get(envs, active);

    printf("=== Environment Status ===\n");
    printf("  Active: %s\n\n", active);

    if (cur) {
        const char *key;
        json_t *val;
        json_object_foreach(cur, key, val) {
            if (json_is_string(val)) {
                printf("  %-20s %s\n", key, json_string_value(val));
            } else if (json_is_boolean(val)) {
                printf("  %-20s %s\n", key, json_is_true(val) ? "true" : "false");
            } else if (json_is_real(val)) {
                printf("  %-20s %.4f\n", key, json_real_value(val));
            } else if (json_is_integer(val)) {
                printf("  %-20s %lld\n", key, (long long)json_integer_value(val));
            }
        }
    }

    free(active);
    json_decref(j);
    return 0;
}

/* ─── CMD: set ─── */
static int cmd_set(const char *env, const char *key, const char *val) {
    json_t *j = read_config();
    if (!j) { printf("No config. Run 'init' first.\n"); return 1; }
    json_t *envs = json_object_get(j, "environments");
    json_t *cur = json_object_get(envs, env);
    if (!cur) {
        printf("Unknown environment: %s\n", env);
        json_decref(j);
        return 1;
    }
    json_object_set_new(cur, key, json_string(val));
    int rc = write_config(j);
    json_decref(j);
    if (rc == 0) printf("  %s/%s = %s\n", env, key, val);
    return rc;
}

/* ─── CMD: get ─── */
static int cmd_get(const char *key) {
    json_t *j = read_config();
    if (!j) { printf("No config.\n"); return 1; }

    char *active = get_active();
    json_t *envs = json_object_get(j, "environments");
    json_t *cur = json_object_get(envs, active);
    if (!cur) { free(active); json_decref(j); return 1; }

    json_t *val = json_object_get(cur, key);
    if (!val) {
        printf("Key '%s' not set for environment '%s'\n", key, active);
        free(active); json_decref(j);
        return 1;
    }

    if (json_is_string(val))
        printf("%s\n", json_string_value(val));
    else if (json_is_true(val))
        printf("true\n");
    else if (json_is_false(val))
        printf("false\n");
    else if (json_is_real(val))
        printf("%.4f\n", json_real_value(val));
    else if (json_is_integer(val))
        printf("%lld\n", (long long)json_integer_value(val));

    free(active);
    json_decref(j);
    return 0;
}

/* ─── CMD: exec — run command under an environment ─── */
static int cmd_exec(const char *env, int argc, char **argv) {
    json_t *j = read_config();
    if (!j) { printf("No config. Run 'init' first.\n"); return 1; }
    json_t *envs = json_object_get(j, "environments");
    json_t *cur = json_object_get(envs, env);
    if (!cur) {
        printf("Unknown environment: %s\n", env);
        json_decref(j);
        return 1;
    }

    if (argc < 1) {
        fprintf(stderr, "Need command to run\n");
        json_decref(j);
        return 1;
    }

    /* Export env vars */
    setenv("HERMES_ENV", env, 1);
    const char *key;
    json_t *val;
    json_object_foreach(cur, key, val) {
        if (json_is_string(val)) {
            char envname[256];
            snprintf(envname, sizeof(envname), "ENV_%s", key);
            /* Uppercase */
            for (char *p = envname; *p; p++) *p = toupper(*p);
            setenv(envname, json_string_value(val), 1);
        }
    }

    /* Fork and exec */
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "exec failed: %s\n", argv[0]);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        json_decref(j);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    json_decref(j);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s init                          — create default config\n", argv[0]);
        fprintf(stderr, "  %s list                          — list environments\n", argv[0]);
        fprintf(stderr, "  %s switch <env>                  — set active env\n", argv[0]);
        fprintf(stderr, "  %s status                        — show current env\n", argv[0]);
        fprintf(stderr, "  %s set <env> <key> <val>         — set config value\n", argv[0]);
        fprintf(stderr, "  %s get <key>                     — get config value\n", argv[0]);
        fprintf(stderr, "  %s exec <env> -- <cmd> [args]    — run under env\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "init") == 0) return cmd_init();
    if (strcmp(argv[1], "list") == 0) return cmd_list();
    if (strcmp(argv[1], "status") == 0) return cmd_status();

    if (strcmp(argv[1], "switch") == 0) {
        if (argc < 3) { fprintf(stderr, "Need env name\n"); return 1; }
        return cmd_switch(argv[2]);
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 5) { fprintf(stderr, "Need: env key value\n"); return 1; }
        return cmd_set(argv[2], argv[3], argv[4]);
    }

    if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) { fprintf(stderr, "Need key\n"); return 1; }
        return cmd_get(argv[2]);
    }

    if (strcmp(argv[1], "exec") == 0) {
        if (argc < 4) { fprintf(stderr, "Need: env -- cmd [args]\n"); return 1; }
        int cmd_start = -1;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--") == 0) { cmd_start = i + 1; break; }
        }
        if (cmd_start < 0) {
            /* No -- separator, treat args[3..] as the command */
            cmd_start = 3;
        }
        return cmd_exec(argv[2], argc - cmd_start, argv + cmd_start);
    }

    fprintf(stderr, "Unknown: %s\n", argv[1]);
    return 1;
}
