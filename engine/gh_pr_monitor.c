/**
 * gh_pr_monitor.c — GitHub PR Monitor (replaces 94-line Python)
 * Tracks open PRs on Scottcjn/Rustchain, reports changes.
 * Build: gcc -O2 gh_pr_monitor.c -o gh_pr_monitor -lcurl -ljansson -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <jansson.h>

#define REPO "Scottcjn/Rustchain"
#define AUTHOR "waefrebeorn"
#define STATE_FILE "/home/wubu2/.hermes/github_pr_state.json"

struct MemBuf { char *data; size_t size; };
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct MemBuf *mb = (struct MemBuf *)userp;
    char *np = realloc(mb->data, mb->size + total + 1);
    if (!np) return 0;
    mb->data = np; memcpy(mb->data + mb->size, ptr, total);
    mb->size += total; mb->data[mb->size] = 0;
    return total;
}

static char *get_token(void) {
    char *env = getenv("GITHUB_TOKEN");
    if (env && env[0]) return strdup(env);
    FILE *f = popen("gh auth token 2>/dev/null", "r");
    if (!f) return NULL;
    static char buf[256]; memset(buf, 0, sizeof(buf));
    if (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == 10) buf[len-1] = 0;
    }
    pclose(f);
    return buf[0] ? strdup(buf) : NULL;
}

static char *http_get(const char *url, const char *token) {
    CURL *curl = curl_easy_init(); if (!curl) return NULL;
    struct MemBuf mb = {0};
    struct curl_slist *h = NULL;
    char auth[512];
    strcpy(auth, "Authorization: Bearer ");
    strncat(auth, token, sizeof(auth) - strlen(auth) - 1);
    h = curl_slist_append(h, auth);
    h = curl_slist_append(h, "Accept: application/vnd.github+json");
    h = curl_slist_append(h, "User-Agent: gh-pr-monitor/1.0");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(h); curl_easy_cleanup(curl);
    if (res != CURLE_OK) { free(mb.data); return NULL; }
    return mb.data;
}

static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *b = malloc(sz + 1); if (!b) { fclose(f); return NULL; }
    size_t n = fread(b, 1, sz, f); b[n] = 0; fclose(f);
    return b;
}

int main(void) {
    char *token = get_token();
    if (!token) { fprintf(stderr, "No GitHub token\n"); return 1; }

    // Fetch PRs via GitHub API
    char url[512];
    snprintf(url, sizeof(url),
        "https://api.github.com/search/issues"
        "?q=is:pr+state:open+repo:%s+author:%s"
        "&sort=created&order=desc&per_page=50",
        REPO, AUTHOR);
    char *raw = http_get(url, token); free(token);
    if (!raw) { fprintf(stderr, "API request failed\n"); return 1; }

    json_error_t err;
    json_t *root = json_loads(raw, 0, &err); free(raw);
    if (!root) { fprintf(stderr, "JSON parse error\n"); return 1; }

    json_t *items = json_object_get(root, "items");
    if (!json_is_array(items)) { json_decref(root); return 1; }

    // Load previous state
    json_t *prev = NULL;
    char *prev_raw = read_file_str(STATE_FILE);
    if (prev_raw) {
        prev = json_loads(prev_raw, 0, &err);
        free(prev_raw);
    }
    if (!prev) prev = json_object();

    json_t *new_state = json_object();
    int changes = 0;
    char report[4096]; report[0] = 0;
    time_t t = time(NULL); (void)t;
    char tb[32]; strftime(tb, sizeof(tb), "%H:%M UTC", gmtime(&t));
    snprintf(report, sizeof(report), "=== GitHub PR Monitor @ %s ===\n", tb);

    size_t idx; json_t *item;
    json_array_foreach(items, idx, item) {
        int num = json_integer_value(json_object_get(item, "number"));
        const char *title = json_string_value(json_object_get(item, "title"));
        const char *state_str = json_string_value(json_object_get(item, "state"));
        const char *merge_status = json_string_value(json_object_get(item, "merge_status"));
        if (!merge_status) merge_status = "UNKNOWN";

        // Build PR entry
        json_t *entry = json_object();
        json_object_set_new(entry, "title", json_string(title ? title : ""));
        json_object_set_new(entry, "merge_status", json_string(merge_status));
        json_object_set_new(entry, "state", json_string(state_str ? state_str : ""));

        char key[32]; snprintf(key, sizeof(key), "%d", num);
        json_t *p = json_object_get(prev, key);
        if (!p) {
            char line[256]; snprintf(line, sizeof(line), "  New PR #%d: %s\n", num, title ? title : "");
            strncat(report, line, sizeof(report) - strlen(report) - 1);
            changes++;
        } else {
            const char *old_ms = json_string_value(json_object_get(p, "merge_status"));
            if (old_ms && strcmp(old_ms, merge_status) != 0) {
                char line[256]; snprintf(line, sizeof(line), "  PR #%d merge: %s -> %s\n", num, old_ms, merge_status);
                strncat(report, line, sizeof(report) - strlen(report) - 1);
                changes++;
            }
        }
        json_object_set_new(new_state, key, entry);
    }

    json_decref(root);

    // Save state
    char *out = json_dumps(new_state, JSON_INDENT(2));
    if (out) {
        FILE *f = fopen(STATE_FILE, "w");
        if (f) { fprintf(f, "%s\n", out); fclose(f); }
        free(out);
    }
    json_decref(new_state);
    json_decref(prev);

    if (changes > 0) {
        printf("%s", report);
    } else {
        printf("No changes @ %s\n", tb);
    }
    return 0;
}
