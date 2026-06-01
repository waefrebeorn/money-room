/**
 * gdelt_sentiment.c — P7: News Sentiment Feature Pipeline
 * Fetches news via Google News RSS, computes lexicon-based scores,
 * writes JSON snapshots for site + stores to SQLite.
 * Build: gcc -O2 gdelt_sentiment.c -o gdelt_sentiment -lcurl -ljansson -lsqlite3 -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#define CACHE_DIR "/home/wubu2/.hermes/gdelt_cache"
#define DB_PATH "/home/wubu2/.hermes/gdelt_cache/sentiment.db"
#define MAX_HEADLINES 100

static const char *RSS_QUERIES[] = {
    "stock+market+OR+economy+OR+inflation",
    "bitcoin+OR+crypto+OR+blockchain",
    "Federal+Reserve+OR+interest+rate+OR+tariff",
    "AI+OR+artificial+intelligence+OR+technology+earnings",
    "oil+OR+energy+OR+recession+OR+market",
    NULL};

static const char *POS_WORDS[] = {"surge","rally","gain","bullish","boom","growth",
    "recovery","optimism","upgrade","positive","breakthrough","outperform","profit",
    "strong","rebound","upside","green","buy","record","high", NULL};
static const char *NEG_WORDS[] = {"crash","plunge","decline","bearish","recession",
    "inflation","loss","downgrade","negative","sell-off","slump","downturn","collapse",
    "crisis","panic","risk","slowdown","drop","fall","tumble","cut","red",
    "pressure","tariff","war","sanction", NULL};

struct MemBuf { char *data; size_t size; };
static size_t write_cb(void *p, size_t s, size_t n, void *u) {
    size_t t = s * n; struct MemBuf *m = u;
    char *np = realloc(m->data, m->size + t + 1);
    if (!np) return 0; m->data = np;
    memcpy(m->data + m->size, p, t); m->size += t; m->data[m->size] = 0;
    return t;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    struct MemBuf mb = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode r = curl_easy_perform(c); curl_easy_cleanup(c);
    if (r != CURLE_OK) { free(mb.data); return NULL; }
    return mb.data;
}

static int in_list(const char *word, const char **list) {
    for (int i = 0; list[i]; i++)
        if (strcmp(word, list[i]) == 0) return 1;
    return 0;
}

static float compute_sentiment(const char *text) {
    if (!text || !*text) return 0.0f;
    char buf[4096]; strncpy(buf, text, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    for (char *p = buf; *p; p++) *p = tolower((unsigned char)*p);
    int pos = 0, neg = 0;
    char *tok = strtok(buf, " .,;:!?-\"\'()[]{}/\\\n\r\t");
    while (tok) {
        if (in_list(tok, POS_WORDS)) pos++;
        else if (in_list(tok, NEG_WORDS)) neg++;
        tok = strtok(NULL, " .,;:!?-\"\'()[]{}/\\\n\r\t");
    }
    int total = pos + neg;
    return total > 0 ? (float)(pos - neg) / total : 0.0f;
}

/* Find substring between two tags in XML. E.g. extract_xml(xml, "<title>", "</title>") */
static char *extract_tag(const char *xml, const char *tag_start, const char *tag_end, const char *after) {
    const char *s = after ? strstr(after, tag_start) : strstr(xml, tag_start);
    if (!s) return NULL;
    s += strlen(tag_start);
    const char *e = strstr(s, tag_end);
    if (!e) return NULL;
    size_t len = e - s;
    char *val = malloc(len + 1);
    if (!val) return NULL;
    memcpy(val, s, len); val[len] = 0;
    return val;
}

/* HTML entity decode in-place */
static void decode_html(char *s) {
    char *r = s;
    while (*s) {
        if (*s == '&') {
            if (strncmp(s, "&amp;", 5) == 0)  { *r++ = '&'; s += 5; }
            else if (strncmp(s, "&lt;", 4) == 0)  { *r++ = '<'; s += 4; }
            else if (strncmp(s, "&gt;", 4) == 0)  { *r++ = '>'; s += 4; }
            else if (strncmp(s, "&quot;", 6) == 0) { *r++ = '"'; s += 6; }
            else if (strncmp(s, "&#39;", 5) == 0)  { *r++ = '\''; s += 5; }
            else if (strncmp(s, "&apos;", 6) == 0) { *r++ = '\''; s += 6; }
            else if (strncmp(s, "&ndash;", 7) == 0){ *r++ = '-'; s += 7; }
            else if (strncmp(s, "&mdash;", 7) == 0){ *r++ = '-'; s += 7; }
            else { *r++ = *s; s++; }
        } else {
            *r++ = *s++;
        }
    }
    *r = 0;
}

int main(void) {
    mkdir(CACHE_DIR, 0755);
    sqlite3 *db;
    sqlite3_open(DB_PATH, &db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS sentiment ("
        "ts TEXT PRIMARY KEY, mean_sentiment REAL, weighted_sentiment REAL,"
        "article_count INTEGER, direction TEXT,"
        "pos_ratio REAL, neg_ratio REAL, neutral_ratio REAL)", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS headlines ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, ts TEXT, headline TEXT,"
        "source TEXT, url TEXT, sentiment REAL)", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    int total_articles = 0;
    float sent_sum = 0;
    int pos_count = 0, neg_count = 0, neu_count = 0;

    /* Headlines JSON array for output */
    json_t *all_headlines = json_array();
    int headline_count = 0;

    /* Fetch each RSS query */
    for (int q = 0; RSS_QUERIES[q]; q++) {
        char url[1024];
        snprintf(url, sizeof(url),
            "https://news.google.com/rss/search?q=%s&hl=en-US&gl=US&ceid=US:en",
            RSS_QUERIES[q]);

        char *xml = http_get(url);
        if (!xml) { fprintf(stderr, "RSS fetch failed for query %d\n", q); continue; }

        /* Find all <item> blocks */
        const char *pos = xml;
        int item_count = 0;
        while ((pos = strstr(pos, "<item>")) != NULL && headline_count < MAX_HEADLINES) {
            pos += 6; /* skip <item> */
            const char *item_end = strstr(pos, "</item>");
            if (!item_end) break;

            /* Extract fields */
            char *title = extract_tag(pos, "<title>", "</title>", NULL);
            char *link  = extract_tag(pos, "<link>", "</link>", NULL);
            char *src   = extract_tag(pos, "<source>", "</source>", NULL);
            char *desc  = extract_tag(pos, "<description>", "</description>", NULL);

            if (title) {
                decode_html(title);
                /* Trim whitespace */
                while (*title == ' ' || *title == '\n' || *title == '\r') title++;
                char *te = title + strlen(title) - 1;
                while (te > title && (*te == ' ' || *te == '\n' || *te == '\r')) *te-- = 0;

                if (strlen(title) > 3) { /* skip tiny titles */
                    /* Combine title + description for sentiment */
                    char combined[4096];
                    combined[0] = 0;
                    strncat(combined, title, sizeof(combined)-1);
                    if (desc) {
                        decode_html(desc);
                        strncat(combined, " ", sizeof(combined)-strlen(combined)-1);
                        strncat(combined, desc, sizeof(combined)-strlen(combined)-1);
                    }

                    float score = compute_sentiment(combined);
                    sent_sum += score;
                    total_articles++;
                    if (score > 0.05f) pos_count++;
                    else if (score < -0.05f) neg_count++;
                    else neu_count++;

                    /* Build JSON headline */
                    json_t *h = json_object();
                    json_object_set_new(h, "title", json_string(title));
                    json_object_set_new(h, "source", json_string(src ? src : "Google News"));
                    json_object_set_new(h, "url", json_string(link ? link : ""));
                    json_object_set_new(h, "sentiment", json_real(score));
                    json_object_set_new(h, "topic", json_string(RSS_QUERIES[q]));
                    json_array_append_new(all_headlines, h);
                    headline_count++;

                    /* SQLite */
                    time_t now = time(NULL); struct tm *tm = gmtime(&now);
                    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
                    sqlite3_stmt *stmt;
                    sqlite3_prepare_v2(db,
                        "INSERT INTO headlines (ts,headline,source,url,sentiment) VALUES (?,?,?,?,?)",
                        -1, &stmt, NULL);
                    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 3, src ? src : "Google News", -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 4, link ? link : "", -1, SQLITE_STATIC);
                    sqlite3_bind_double(stmt, 5, score);
                    sqlite3_step(stmt); sqlite3_finalize(stmt);
                }
            }

            free(title); free(link); free(src); free(desc);
            item_count++;
            pos = item_end + 7; /* past </item> */
        }

        fprintf(stderr, "Query %d: %d items\n", q, item_count);
        free(xml);
    }

    /* ─── Determine aggregate ─── */
    float mean = 0.0f; const char *dir = "neutral";
    float pos_ratio = 0.0f, neg_ratio = 0.0f, neu_ratio = 0.0f;
    if (total_articles > 0) {
        mean = sent_sum / total_articles;
        if (mean > 0.05f) dir = "bullish";
        else if (mean < -0.05f) dir = "bearish";
        pos_ratio = (float)pos_count / total_articles;
        neg_ratio = (float)neg_count / total_articles;
        neu_ratio = (float)neu_count / total_articles;
    }

    /* ─── Timestamp ─── */
    time_t now = time(NULL); struct tm *tm = gmtime(&now);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);

    /* ─── Store aggregate to SQLite ─── */
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO sentiment VALUES (?,?,?,?,?,?,?,?)",
            -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 2, mean);
        sqlite3_bind_double(stmt, 3, mean);
        sqlite3_bind_int(stmt, 4, total_articles);
        sqlite3_bind_text(stmt, 5, dir, -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 6, pos_ratio);
        sqlite3_bind_double(stmt, 7, neg_ratio);
        sqlite3_bind_double(stmt, 8, neu_ratio);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    sqlite3_close(db);

    /* ─── Write latest_sentiment.json (feature_vector format) ─── */
    {
        json_t *fv = json_object();
        json_object_set_new(fv, "sentiment_score", json_real(mean));
        json_object_set_new(fv, "sentiment_direction", json_string(dir));
        json_object_set_new(fv, "sentiment_articles", json_integer(total_articles));
        json_object_set_new(fv, "sentiment_conf", json_real(total_articles > 0 ? 0.65 : 0.2));
        json_object_set_new(fv, "pos_ratio", json_real(pos_ratio));
        json_object_set_new(fv, "neg_ratio", json_real(neg_ratio));
        json_object_set_new(fv, "neutral_ratio", json_real(neu_ratio));

        json_t *sent = json_object();
        json_object_set_new(sent, "timestamp", json_string(ts));
        json_object_set_new(sent, "feature_vector", fv);
        json_object_set(sent, "headlines", all_headlines);

        char path[512];
        snprintf(path, sizeof(path), "%s/latest_sentiment.json", CACHE_DIR);
        json_dump_file(sent, path, JSON_INDENT(2));
        json_decref(sent);
    }

    /* ─── Write latest.json (aggregate + headlines format) ─── */
    {
        json_t *agg = json_object();
        json_object_set_new(agg, "mean_sentiment", json_real(mean));
        json_object_set_new(agg, "weighted_sentiment", json_real(mean));
        json_object_set_new(agg, "direction", json_string(dir));
        json_object_set_new(agg, "article_count", json_integer(total_articles));
        json_object_set_new(agg, "pos_ratio", json_real(pos_ratio));
        json_object_set_new(agg, "neg_ratio", json_real(neg_ratio));
        json_object_set_new(agg, "neutral_ratio", json_real(neu_ratio));

        json_t *latest = json_object();
        json_object_set_new(latest, "timestamp", json_string(ts));
        json_object_set_new(latest, "status", json_string("ok"));
        json_object_set_new(latest, "aggregate", agg);
        json_object_set(latest, "headlines", all_headlines);

        char path[512];
        snprintf(path, sizeof(path), "%s/latest.json", CACHE_DIR);
        json_dump_file(latest, path, JSON_INDENT(2));
        json_decref(latest);
    }
    json_decref(all_headlines);

    /* ─── Print summary to stdout ─── */
    if (total_articles > 0) {
        printf("News Sentiment: %s (%.4f) — %d articles, %d headlines\n",
               dir, mean, total_articles, headline_count);
        printf("  Bullish: %.1f%%  Bearish: %.1f%%  Neutral: %.1f%%\n",
               pos_count*100.0/total_articles, neg_count*100.0/total_articles,
               neu_count*100.0/total_articles);
    } else {
        printf("News: No articles fetched\n");
    }

    return total_articles > 0 ? 0 : 1;
}
