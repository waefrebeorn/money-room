/**
 * sports_news_collector.c — Sports News + World Events + Sentiment
 * Fetches sports news from ESPN news feed and Google News RSS,
 * filters for sports-relevant world events, computes keyword-based sentiment
 *
 * Compile: gcc -O2 -Wall -o sports_news_collector sports_news_collector.c -lcurl -ljansson -lsqlite3 -lm
 * Usage:   ./sports_news_collector
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <strings.h>
#include <sys/stat.h>

#define OUT_DIR  "/home/wubu2/money-room/data/multi_market"
#define OUT_FILE OUT_DIR "/sports_news.json"
#define DB_PATH  "/home/wubu2/money-room/engine/timeline.db"

typedef struct { char *data; size_t len; } http_buf_t;

static size_t write_cb(void *ptr, size_t s, size_t n, void *u) {
    size_t t = s * n; http_buf_t *b = (http_buf_t*)u;
    char *np = realloc(b->data, b->len + t + 1);
    if (!np) return 0; b->data = np;
    memcpy(b->data + b->len, ptr, t); b->len += t; b->data[b->len] = '\0';
    return t;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    http_buf_t b = {NULL, 0};
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    h = curl_slist_append(h, "Accept: application/json, application/rss+xml, text/xml");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(b.data); return NULL; }
    return b.data;
}

// Simple tag extraction for RSS XML (no XML lib needed)
static void extract_tag(const char *xml, const char *tag, char *out, int max) {
    out[0] = '\0';
    char open[32], close[32];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    // Try CDATA variant
    char cdata_open[64];
    snprintf(cdata_open, sizeof(cdata_open), "<![CDATA[");
    // Try with namespace
    char ns_open[64], ns_close[64];
    snprintf(ns_open, sizeof(ns_open), "<media:%s>", tag);
    snprintf(ns_close, sizeof(ns_close), "</media:%s>", tag);

    const char *s = strstr(xml, open);
    if (!s) s = strstr(xml, ns_open);
    if (!s) return;
    s = strchr(s, '>');
    if (!s) return;
    s++;

    const char *e = strstr(s, close);
    if (!e) e = strstr(s, ns_close);
    if (!e) return;

    int len = (int)(e - s);
    if (len > max - 1) len = max - 1;

    // Strip CDATA wrapper
    const char *cdata = strstr(s, cdata_open);
    if (cdata && cdata < s + len) {
        cdata += strlen(cdata_open);
        const char *cend = strstr(cdata, "]]>");
        if (cend && cend < s + len) {
            s = cdata;
            len = (int)(cend - cdata);
            if (len > max - 1) len = max - 1;
        }
    }

    // Also strip any HTML entities
    strncpy(out, s, len);
    out[len] = '\0';
    // Decode &amp;
    char *amp;
    while ((amp = strstr(out, "&amp;"))) {
        *amp = '&';
        memmove(amp + 1, amp + 5, strlen(amp + 5) + 1);
    }
}

// Simple keyword sentiment score
static double compute_sentiment(const char *text) {
    if (!text || !text[0]) return 0;
    char low[2048];
    int i;
    for (i = 0; text[i] && i < 2047; i++)
        low[i] = tolower((unsigned char)text[i]);
    low[i] = '\0';

    double score = 0;
    int matches = 0;

    // Positive sports terms
    const char *pos_words[] = {
        "win", "won", "victory", "champion", "dominate", "comeback",
        "record", "star", "excellent", "amazing", "perfect", "streak",
        "playoff", "leading", "surge", "rally", "upset", "highlight",
        "brilliant", "outstanding", "clutch", "roar", "triumph", "shutout",
        "breakout", "career-high", "mvp", "all-star", "signature",
        "red-hot", "on fire", "unstoppable", "legendary",
        NULL
    };
    const char *neg_words[] = {
        "injury", "injured", "loss", "lose", "lost", "suspend",
        "suspended", "ban", "fine", "fined", "surgery", "out for",
        "struggle", "struggling", "collapse", "blow", "blown",
        "worst", "terrible", "embarrassing", "humiliate", "defeat",
        "crushing", "devastating", "fracture", "tear", "sprain",
        "covid", "pandemic", "postpone", "cancelled", "cancel",
        "scandal", "controversy", "protest", "strike", "lockout",
        "investigation", "lawsuit", "penalty", "disciplinary",
        NULL
    };
    // World events affecting sports
    const char *world_events[] = {
        "hurricane", "earthquake", "flood", "wildfire", "blizzard",
        "tornado", "extreme weather", "heat wave", "cold snap",
        "terrorist", "shooting", "violence", "security threat",
        "pandemic", "epidemic", "quarantine", "lockdown",
        "war", "conflict", "invasion", "sanction",
        "strike", "protest", "riot", "curfew",
        "power outage", "blackout", "cyberattack", "hack",
        NULL
    };

    for (int w = 0; pos_words[w]; w++) {
        if (strstr(low, pos_words[w])) {
            score += 0.3;
            matches++;
        }
    }
    for (int w = 0; neg_words[w]; w++) {
        if (strstr(low, neg_words[w])) {
            score -= 0.4;
            matches++;
        }
    }

    return matches > 0 ? score / sqrt(matches) : 0;
}

// Simple case-insensitive search (declaration for matches_team)
static int strcasestr_compat(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nl = strlen(needle);
    size_t hl = strlen(haystack);
    if (nl > hl) return 0;
    for (size_t i = 0; i <= hl - nl; i++) {
        size_t j;
        for (j = 0; j < nl; j++) {
            if (tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

// Check if article mentions any team from our DB
static int matches_team(const char *text, const char **teams, int n_teams) {
    if (!text || !text[0]) return 0;
    for (int i = 0; i < n_teams; i++) {
        if (strcasestr_compat(text, teams[i])) return 1;
    }
    return 0;
}

int main(void) {
    curl_global_init(CURL_GLOBAL_ALL);

    // Setup DB
    sqlite3 *db;
    sqlite3_open(DB_PATH, &db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS sports_news ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "title TEXT, source TEXT, url TEXT, "
        "pub_date TEXT, category TEXT, "
        "sentiment REAL, fetched_at INTEGER)",
        NULL, NULL, NULL);

    // Team names from DB for content matching
    const char *known_teams[] = {
        "Yankees", "Red Sox", "Dodgers", "Cubs", "Giants", "Cardinals",
        "Braves", "Astros", "Phillies", "Mets", "Padres", "Mariners",
        "Brewers", "Twins", "Guardians", "Blue Jays", "Orioles", "Rays",
        "Tigers", "Royals", "Angels", "Athletics", "Pirates", "Reds",
        "Rockies", "Marlins", "Diamondbacks", "Nationals", "White Sox", "Rangers",
        "Lakers", "Celtics", "Warriors", "Nuggets", "Thunder", "Knicks",
        "76ers", "Bucks", "Heat", "Cavaliers", "Suns", "Timberwolves",
        "Clippers", "Kings", "Hawks", "Magic", "Pacers", "Spurs",
        "Chiefs", "49ers", "Eagles", "Cowboys", "Ravens", "Bills",
        "Packers", "Bengals", "Lions", "Patriots", "Seahawks", "Steelers",
        NULL
    };

    json_t *root = json_array();
    int total = 0;

    // ===== Source 1: ESPN News Headlines =====
    printf("[SPORTSNEWS] ESPN headlines... ");
    fflush(stdout);
    char *espn = http_get("https://site.api.espn.com/apis/site/v2/sports/news/headlines");
    if (espn) {
        json_error_t err;
        json_t *j = json_loads(espn, 0, &err);
        free(espn);
        if (j) {
            json_t *articles = json_object_get(j, "articles");
            if (articles && json_is_array(articles)) {
                size_t ai;
                json_t *art;
                json_array_foreach(articles, ai, art) {
                    const char *title = json_string_value(json_object_get(art, "headline"));
                    const char *desc = json_string_value(json_object_get(art, "description"));
                    const char *src = json_string_value(json_object_get(art, "source"));
                    const char *pub = json_string_value(json_object_get(art, "published"));
                    const char *url = json_string_value(json_object_get(art, "links"));
                    if (!title) continue;

                    // Get URL from links.web
                    const char *art_url = "";
                    json_t *links = json_object_get(art, "links");
                    if (links) {
                        json_t *web = json_object_get(links, "web");
                        if (web) {
                            const char *href = json_string_value(json_object_get(web, "href"));
                            if (href) art_url = href;
                        }
                    }

                    char full_text[4096];
                    snprintf(full_text, sizeof(full_text), "%s %s", title, desc ? desc : "");

                    double sentiment = compute_sentiment(full_text);
                    const char *category = matches_team(full_text, known_teams, sizeof(known_teams)/sizeof(known_teams[0]))
                        ? "sports" : "general";

                    json_t *entry = json_pack("{s:s, s:s, s:s, s:s, s:s, s:f}",
                        "title", title,
                        "source", src ? src : "ESPN",
                        "url", art_url,
                        "pub_date", pub ? pub : "",
                        "category", category,
                        "sentiment", sentiment);
                    json_array_append_new(root, entry);

                    // DB insert
                    const char *sql = "INSERT INTO sports_news (title, source, url, pub_date, category, sentiment, fetched_at) VALUES (?,?,?,?,?,?,?)";
                    sqlite3_stmt *st;
                    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(st, 1, title, -1, SQLITE_STATIC);
                        sqlite3_bind_text(st, 2, src ? src : "ESPN", -1, SQLITE_STATIC);
                        sqlite3_bind_text(st, 3, art_url, -1, SQLITE_STATIC);
                        sqlite3_bind_text(st, 4, pub ? pub : "", -1, SQLITE_STATIC);
                        sqlite3_bind_text(st, 5, category, -1, SQLITE_STATIC);
                        sqlite3_bind_double(st, 6, sentiment);
                        sqlite3_bind_int64(st, 7, (sqlite3_int64)time(NULL));
                        sqlite3_step(st);
                        sqlite3_finalize(st);
                    }
                    total++;
                }
            }
            json_decref(j);
        }
        printf("%d headlines\n", total);
    } else {
        printf("no data\n");
    }

    // ===== Source 2: Google News RSS for sports world events =====
    printf("[SPORTSNEWS] Google News RSS sports... ");
    fflush(stdout);
    const char *rss_queries[] = {
        "sports+injury+report",
        "sports+weather+cancellation",
        "sports+stadium+event",
        "NBA+playoffs+update",
        "MLB+baseball+news",
        "NFL+football+training",
        "NHL+hockey+news",
        "soccer+EPL+transfer",
        NULL
    };
    int rss_count = 0;
    for (int q = 0; rss_queries[q]; q++) {
        char url[512];
        snprintf(url, sizeof(url),
            "https://news.google.com/rss/search?q=%s&hl=en-US&gl=US&ceid=US:en",
            rss_queries[q]);
        char *rss = http_get(url);
        if (!rss) continue;

        // Parse RSS items
        const char *p = rss;
        while ((p = strstr(p, "<item>"))) {
            char title[1024] = {0}, source[256] = {0}, pub_date[128] = {0};
            extract_tag(p, "title", title, sizeof(title));
            extract_tag(p, "source", source, sizeof(source));
            extract_tag(p, "pubDate", pub_date, sizeof(pub_date));

            if (!title[0]) { p += 6; continue; }

            // Skip generic headlines
            if (strstr(title, "Advertisement") || strlen(title) < 10) {
                p += 6; continue;
            }

            double sentiment = compute_sentiment(title);
            const char *category = matches_team(title, known_teams, sizeof(known_teams)/sizeof(known_teams[0]))
                ? "sports" : "world";

            json_t *entry = json_pack("{s:s, s:s, s:s, s:s, s:s, s:f}",
                "title", title,
                "source", source[0] ? source : "Google News",
                "url", "",
                "pub_date", pub_date,
                "category", category,
                "sentiment", sentiment);
            json_array_append_new(root, entry);

            // DB insert
            const char *sql = "INSERT INTO sports_news (title, source, pub_date, category, sentiment, fetched_at) VALUES (?,?,?,?,?,?)";
            sqlite3_stmt *st;
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, title, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 2, source[0] ? source : "Google News", -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 3, pub_date, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 4, category, -1, SQLITE_STATIC);
                sqlite3_bind_double(st, 5, sentiment);
                sqlite3_bind_int64(st, 6, (sqlite3_int64)time(NULL));
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            rss_count++;
            p += 6;
        }
        free(rss);
    }
    printf("%d RSS articles\n", rss_count);
    total += rss_count;

    // Write JSON output
    mkdir(OUT_DIR, 0755);
    json_dump_file(root, OUT_FILE, JSON_INDENT(2));
    printf("[SPORTSNEWS] Total: %d articles -> %s\n", total, OUT_FILE);

    json_decref(root);
    sqlite3_close(db);
    curl_global_cleanup();
    return 0;
}
