/**
 * bounty_scanner.c — R30: GitHub Bounty Auto Scanner
 * Replaces bounty_scanner.py (215 lines Python)
 *
 * Scans configured repos for labelled issues.
 * Tracks seen issues in SQLite. Only reports NEW bounties.
 *
 * Build: gcc bounty_scanner.c -o bounty_scanner -lcurl -ljansson -lsqlite3 -lm -O2
 * Run:   ./bounty_scanner
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <jansson.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>

#define DB_PATH "/home/wubu2/.hermes/bounty_scanner/bounties.db"
#define PER_PAGE 50

static const char *REPOS[] = {"scottcjn/RustChain", "scottcjn/rustchain-bounties", NULL};

struct MemBuf { char *data; size_t size; };

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct MemBuf *mb = (struct MemBuf *)userp;
    char *np = realloc(mb->data, mb->size + total + 1);
    if (!np) return 0;
    mb->data = np;
    memcpy(mb->data + mb->size, ptr, total);
    mb->size += total;
    mb->data[mb->size] = 0;
    return total;
}

static char *get_auth_token(void) {
    char *env = getenv("GITHUB_TOKEN");
    if (env && env[0]) return strdup(env);
    FILE *f = popen("gh auth token 2>/dev/null", "r");
    if (!f) return NULL;
    static char buf[256];
    memset(buf, 0, sizeof(buf));
    if (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == 10) buf[len-1] = 0;
    }
    pclose(f);
    return buf[0] ? strdup(buf) : NULL;
}

static char *http_get(const char *url, const char *auth_token) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    struct MemBuf mb = {0};
    struct curl_slist *headers = NULL;
    char auth[512];
    strcpy(auth, "Authorization: Bearer ");
    strncat(auth, auth_token, sizeof(auth) - strlen(auth) - 1);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "User-Agent: hermes-bounty-scanner/1.0");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) { free(mb.data); return NULL; }
    return mb.data;
}

typedef struct { const char *name; const char *keywords[20]; } CategoryMap;
static const CategoryMap CATEGORIES[] = {
    {"code",     {"fix","implement","build","develop","code","patch","refactor",
                  "add feature","integration","api","cli","command","test", NULL}},
    {"content",  {"blog","write","post","article","tutorial","guide","documentation",
                  "doc","video","tweet","thread","social", NULL}},
    {"community",{"star","follow","referral","friend","share","nominate",
                  "bring","invite","vote", NULL}},
    {"security", {"vulnerability","bug","security","audit","exploit","cve",
                  "injection","xss","csrf","sqli", NULL}},
    {NULL, {NULL}}
};

static const char *determine_category(const char *title, const char *body) {
    static char text[2048];
    text[0] = 0;
    if (title) strncat(text, title, sizeof(text)-1);
    strncat(text, " ", sizeof(text)-1);
    if (body) strncat(text, body, sizeof(text)-strlen(text)-1);
    for (char *p = text; *p; p++) *p = tolower((unsigned char)*p);
    int best_score = 0;
    const char *best_cat = "other";
    for (int c = 0; CATEGORIES[c].name; c++) {
        int score = 0;
        for (int k = 0; CATEGORIES[c].keywords[k]; k++) {
            if (strstr(text, CATEGORIES[c].keywords[k])) score++;
        }
        if (score > best_score) { best_score = score; best_cat = CATEGORIES[c].name; }
    }
    return best_cat;
}

static void extract_reward(const char *title, char *out, size_t out_sz) {
    out[0] = 0;
    if (!title) return;
    while (*title) {
        if (isdigit((unsigned char)*title)) {
            const char *start = title;
            while (isdigit((unsigned char)*title) || *title == '.' || *title == ',') title++;
            while (*title == ' ') title++;
            if (strncmp(title, "RTC", 3) == 0 || *title == '$' ||
                strncmp(title, "USD", 3) == 0 || strncmp(title, "Pool", 4) == 0) {
                size_t n = title - start;
                char num[64]; memset(num, 0, sizeof(num));
                strncpy(num, start, n < 63 ? n : 63);
                char unit[16]; memset(unit, 0, sizeof(unit));
                int i = 0;
                while (title[i] && !isspace((unsigned char)title[i]) && title[i] != ',' && i < 15) {
                    unit[i] = title[i]; i++;
                }
                snprintf(out, out_sz, "%s %s", num, unit);
                return;
            }
        } else title++;
    }
}

static sqlite3 *init_db(void) {
    char dir[256]; strncpy(dir, DB_PATH, sizeof(dir)-1);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }

    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS bounties ("
        "  id INTEGER PRIMARY KEY,"
        "  repo TEXT NOT NULL,"
        "  issue_number INTEGER NOT NULL,"
        "  title TEXT,"
        "  url TEXT UNIQUE,"
        "  category TEXT DEFAULT 'other',"
        "  reward_hint TEXT DEFAULT '',"
        "  status TEXT DEFAULT 'new',"
        "  first_seen INTEGER,"
        "  last_updated INTEGER,"
        "  UNIQUE(repo, issue_number)"
        ");"
        "CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT);",
        NULL, NULL, NULL);
    return db;
}

static json_t *fetch_bounties(const char *auth_token, const char *repo) {
    char url[1024];
    snprintf(url, sizeof(url),
        "https://api.github.com/search/issues"
        "?q=is:issue+state:open+repo:%s+label:bounty"
        "&sort=created&order=desc&per_page=%d",
        repo, PER_PAGE);
    char *raw = http_get(url, auth_token);
    if (!raw) return NULL;
    json_error_t err;
    json_t *root = json_loads(raw, 0, &err);
    free(raw);
    if (!root) return NULL;
    return root;
}

int main(void) {
    char *auth_token = get_auth_token();
    if (!auth_token) { fprintf(stderr, "No GitHub token.\\n"); return 1; }
    sqlite3 *db = init_db();
    if (!db) { free(auth_token); return 1; }
    time_t now = time(NULL);

    for (int r = 0; REPOS[r]; r++) {
        json_t *root = fetch_bounties(auth_token, REPOS[r]);
        if (!root) continue;
        json_t *items = json_object_get(root, "items");
        if (!json_is_array(items)) { json_decref(root); continue; }
        size_t idx; json_t *item;
        json_array_foreach(items, idx, item) {
            int issue_num = json_integer_value(json_object_get(item, "number"));
            const char *url = json_string_value(json_object_get(item, "html_url"));
            const char *title = json_string_value(json_object_get(item, "title"));
            const char *body = json_string_value(json_object_get(item, "body"));
            if (!body) body = "";
            const char *cat = determine_category(title, body);
            char reward[64]; extract_reward(title, reward, sizeof(reward));

            sqlite3_stmt *stmt;
            sqlite3_prepare_v2(db, "SELECT id FROM bounties WHERE repo=? AND issue_number=?", -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, REPOS[r], -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 2, issue_num);
            int exists = (sqlite3_step(stmt) == SQLITE_ROW);
            int existing_id = exists ? sqlite3_column_int(stmt, 0) : 0;
            sqlite3_finalize(stmt);

            if (exists) {
                sqlite3_prepare_v2(db, "UPDATE bounties SET title=?,category=?,reward_hint=?,last_updated=? WHERE id=?", -1, &stmt, NULL);
                sqlite3_bind_text(stmt, 1, title, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, cat, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 3, reward, -1, SQLITE_STATIC);
                sqlite3_bind_int64(stmt, 4, (sqlite3_int64)now);
                sqlite3_bind_int(stmt, 5, existing_id);
                sqlite3_step(stmt); sqlite3_finalize(stmt);
            } else {
                sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO bounties (repo,issue_number,title,url,category,reward_hint,status,first_seen,last_updated) VALUES (?,?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
                sqlite3_bind_text(stmt, 1, REPOS[r], -1, SQLITE_STATIC);
                sqlite3_bind_int(stmt, 2, issue_num);
                sqlite3_bind_text(stmt, 3, title, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 4, url, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 5, cat, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 6, reward, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 7, "new", -1, SQLITE_STATIC);
                sqlite3_bind_int64(stmt, 8, (sqlite3_int64)now);
                sqlite3_bind_int64(stmt, 9, (sqlite3_int64)now);
                sqlite3_step(stmt); sqlite3_finalize(stmt);
                printf("NEW: repo=%s num=%d title=%s\\n", REPOS[r], issue_num, title ? title : "");
            }
        }
        json_decref(root);
    }

    sqlite3_stmt *stmt;
    int total = 0;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM bounties", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    printf("Bounty Tracker: %d total\\n", total);

    sqlite3_prepare_v2(db, "SELECT category, COUNT(*) FROM bounties GROUP BY category ORDER BY COUNT(*) DESC", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("  %d %s\\n", sqlite3_column_int(stmt, 1), sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    free(auth_token);
    return 0;
}
