/*
 * news_rss.c — P74: Financial News RSS Pipeline (C port)
 *
 * Replaces news_rss.py. Fetches MarketWatch + Yahoo Finance RSS feeds,
 * extracts ticker mentions, computes keyword-based sentiment.
 * Line-based XML parser (no libxml2 dependency).
 *
 * Features:
 *   F75: News volume anomaly (0-1)
 *   F76: News sentiment score (0-1)
 *
 * Build: gcc news_rss.c -o news_rss -lcurl -lsqlite3 -lm -O2
 * Run: ./news_rss
 * Output: ~/.hermes/news_cache/news_features.json
 */

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <curl/curl.h>

/* ── Config ── */
#define CACHE_DIR "~/.hermes/news_cache"
#define OUTPUT_FILE "~/.hermes/news_cache/news_features.json"
#define DB_PATH "~/.hermes/news_cache/news_history.db"
#define MAX_ARTICLES 200
#define MAX_TITLE 512
#define MAX_URL 1024
#define RATE_LIMIT_MS 500

/* ── RSS Feeds ── */
#define N_FEEDS 2
static const char *FEED_NAMES[N_FEEDS] = {"MarketWatch", "Yahoo Finance"};
static const char *FEED_URLS[N_FEEDS] = {
    "https://feeds.content.dowjones.io/public/rss/mw_topstories",
    "https://finance.yahoo.com/news/rssindex",
};

/* ── Ticker dictionary (subset of 100 most important) ── */
#define N_TICKERS 82
static const char *TICKERS[N_TICKERS] = {
    "AAPL","MSFT","GOOGL","GOOG","AMZN","META","NVDA","TSLA","AVGO",
    "JPM","BAC","V","MA","WMT","COST","HD","JNJ","UNH","PG","XOM",
    "CVX","ORCL","DIS","NFLX","ADBE","CRM","INTC","AMD","IBM","CSCO",
    "QCOM","TXN","ABNB","UBER","PYPL","SQ","SPY","QQQ","DIA","IWM",
    "XLF","XLK","XLE","XLV","XLI","XLU","XLP","XLB","XLY","TLT",
    "GLD","SLV","USO","BTC","ETH","NKE","MCD","SBUX",
    "AMGN","GILD","ABBV","PFE","MRK","LLY","TMO",
    "LMT","GE","BA","CAT","DE","UPS","FDX",
    "KO","PEP","T","VZ","CMCSA","F","GM","PLTR","COIN","MSTR",
};

/* ── Sentiment keywords ── */
#define N_POS 34
static const char *POS_WORDS[N_POS] = {
    "surge","surges","surged","jump","jumps","jumped",
    "gain","gains","gained","rise","rises","rose","rising",
    "rally","rallies","rallied","beat","beats","upgrade",
    "growth","profit","profits","bull","bullish","outperform",
    "record","high","higher","positive","strong",
    "stronger","momentum","rallying","rising",
};
#define N_NEG 36
static const char *NEG_WORDS[N_NEG] = {
    "drop","drops","dropped","fall","falls","fell","falling",
    "decline","declines","sell","sells","selling",
    "crash","crashes","slump","cut","cuts","cutting","warning","warn","warns",
    "miss","misses","bear","bearish","bearish","layoff","layoffs","lawsuit",
    "debt","debt","recession","inflation","risk","risks","volatile",
};

/* ── Memory buffer for curl ── */
struct MemBuf { char *data; size_t len; };

static size_t write_cb(void *ptr, size_t sz, size_t nm, void *ud) {
    size_t total = sz * nm;
    struct MemBuf *b = (struct MemBuf *)ud;
    char *nd = realloc(b->data, b->len + total + 1);
    if (!nd) return 0;
    b->data = nd;
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

static char *fetch_url(const char *url) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    struct MemBuf b = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; MoneyRoom/1.0)");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return r == CURLE_OK ? b.data : (free(b.data), NULL);
}

/* ── Line-based XML tag extractor ── */
static char *extract_tag(const char *xml, const char *tag, int *pos) {
    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *start = strstr(xml + *pos, open);
    if (!start) return NULL;
    start += strlen(open);

    const char *end = strstr(start, close);
    if (!end) return NULL;

    int len = (int)(end - start);
    if (len <= 0 || len > 2000) return NULL;

    char *val = malloc(len + 1);
    if (!val) return NULL;
    memcpy(val, start, len);
    val[len] = '\0';

    /* Decode HTML entities */
    char *tmp = val;
    char *dst = val;
    while (*tmp) {
        if (strncmp(tmp, "&amp;", 5) == 0) { *dst++ = '&'; tmp += 5; }
        else if (strncmp(tmp, "&lt;", 4) == 0) { *dst++ = '<'; tmp += 4; }
        else if (strncmp(tmp, "&gt;", 4) == 0) { *dst++ = '>'; tmp += 4; }
        else if (strncmp(tmp, "&quot;", 6) == 0) { *dst++ = '"'; tmp += 6; }
        else if (strncmp(tmp, "&apos;", 6) == 0) { *dst++ = '\''; tmp += 6; }
        else if (strncmp(tmp, "&#", 2) == 0) {
            char *semi = strchr(tmp, ';');
            if (semi) { *dst++ = ' '; tmp = semi + 1; }
            else { *dst++ = *tmp++; }
        }
        else if (*tmp == '<') { break; }  /* Strip any nested tags */
        else { *dst++ = *tmp++; }
    }
    *dst = '\0';

    *pos = (int)(end - xml + strlen(close));
    return val;
}

/* ── Check if a word matches a ticker ── */
static int is_ticker(const char *word) {
    for (int i = 0; i < N_TICKERS; i++) {
        if (strcmp(word, TICKERS[i]) == 0) return 1;
    }
    return 0;
}

/* ── Simple keyword sentiment ── */
static float compute_sentiment(const char *text) {
    /* Copy to lower */
    char lower[4096];
    int i;
    for (i = 0; text[i] && i < 4095; i++)
        lower[i] = (text[i] >= 'A' && text[i] <= 'Z') ? text[i] + 32 : text[i];
    lower[i] = '\0';

    int pos_count = 0, neg_count = 0;
    int total = 0;

    /* Tokenize on non-alpha */
    const char *delim = " .,!?;:\"'()[]{}/\\\n\r\t-0123456789";
    char *token = strtok(lower, delim);
    while (token) {
        total++;
        for (int i = 0; i < N_POS; i++)
            if (strcmp(token, POS_WORDS[i]) == 0) { pos_count++; break; }
        for (int i = 0; i < N_NEG; i++)
            if (strcmp(token, NEG_WORDS[i]) == 0) { neg_count++; break; }
        token = strtok(NULL, delim);
    }

    if (pos_count == 0 && neg_count == 0) return 0.0f;
    int sum = pos_count + neg_count;
    return (float)(pos_count - neg_count) / sum;
}

/* ── Extract tickers from text ── */
static int extract_tickers(const char *text, char out[10][8], int *n_out) {
    *n_out = 0;
    char lower[4096];
    int i;
    for (i = 0; text[i] && i < 4095; i++)
        lower[i] = (text[i] >= 'A' && text[i] <= 'Z') ? text[i] + 32 : text[i];
    lower[i] = '\0';

    /* Check for $TICKER format first */
    const char *p = text;
    while ((p = strstr(p, "$"))) {
        p++;
        char sym[8];
        int j;
        for (j = 0; j < 6 && p[j] && p[j] >= 'A' && p[j] <= 'Z'; j++)
            sym[j] = p[j];
        sym[j] = '\0';
        if (j >= 1 && is_ticker(sym) && *n_out < 10) {
            strncpy(out[*n_out], sym, 7);
            out[*n_out][7] = '\0';
            (*n_out)++;
        }
    }

    /* Check uppercase words */
    const char *delim = " .,!?;:\"'()[]{}/\\\n\r\t-";
    char upper[4096];
    strncpy(upper, text, 4095);
    char *token = strtok(upper, delim);
    int found_added = 0;
    while (token) {
        int len = strlen(token);
        /* Check all-caps 1-5 letter words */
        if (len >= 1 && len <= 5) {
            int all_upper = 1;
            for (int j = 0; j < len; j++)
                if (token[j] < 'A' || token[j] > 'Z') { all_upper = 0; break; }
            if (all_upper && is_ticker(token)) {
                /* Dedup against $TICKER finds */
                int dup = 0;
                for (int j = 0; j < *n_out; j++)
                    if (strcmp(out[j], token) == 0) { dup = 1; break; }
                if (!dup && *n_out < 10) {
                    strncpy(out[*n_out], token, 7);
                    out[*n_out][7] = '\0';
                    (*n_out)++;
                }
            }
        }
        token = strtok(NULL, delim);
    }

    return *n_out;
}

/* ── Path helpers ── */
static void expand_path(const char *p, char *out, size_t sz) {
    const char *h = getenv("HOME");
    if (!h) h = "/home/wubu2";
    if (p[0] == '~') snprintf(out, sz, "%s%s", h, p + 1);
    else snprintf(out, sz, "%s", p);
}

/* ── Parse RSS items ── */
typedef struct { char title[MAX_TITLE]; char url[MAX_URL]; float sentiment; int n_tickers; char tickers[10][8]; } Article;

static int parse_rss(const char *xml, Article *arts, int max_arts) {
    if (!xml || strlen(xml) < 20) return 0;
    int count = 0;
    int pos = 0;
    while (count < max_arts) {
        /* Find next <item> or <entry> */
        const char *item_start = strstr(xml + pos, "<item>");
        const char *entry_start = strstr(xml + pos, "<entry>");
        const char *start = NULL;

        if (item_start && (!entry_start || item_start < entry_start))
            start = item_start;
        else if (entry_start)
            start = entry_start;
        else break;

        int item_pos = (int)(start - xml);
        pos = item_pos + 7;

        /* Extract fields */
        int ep = item_pos;
        char *title = extract_tag(xml, "title", &ep);
        char *link = NULL;

        /* RSS: <link>text</link>, Atom: <link href="..."/> */
        char *rss_link = extract_tag(xml, "link", &ep);
        if (rss_link) link = rss_link;

        /* For Atom, check href attribute */
        if (!link || strlen(link) == 0) {
            const char *ltag = strstr(xml + item_pos, "<link ");
            if (ltag) {
                const char *href = strstr(ltag, "href=\"");
                if (href) {
                    href += 6;
                    const char *endq = strchr(href, '"');
                    if (endq) {
                        int llen = (int)(endq - href);
                        if (llen > 0 && llen < MAX_URL) {
                            link = malloc(llen + 1);
                            if (link) { memcpy(link, href, llen); link[llen] = '\0'; }
                        }
                    }
                }
            }
        }

        if (!title && !link) continue;

        /* Build text for analysis */
        char text[4096] = {0};
        if (title) { strncat(text, title, 2000); strncat(text, " ", 1); }
        if (link) { strncat(text, link, 1000); }

        Article *a = &arts[count];
        memset(a, 0, sizeof(Article));
        if (title) { strncpy(a->title, title, MAX_TITLE-1); free(title); }
        if (link) { strncpy(a->url, link, MAX_URL-1); free(link); }

        a->sentiment = compute_sentiment(text);
        extract_tickers(text, a->tickers, &a->n_tickers);

        count++;
    }
    return count;
}

/* ── Main ── */
int main(void) {
    printf("[NEWS] Fetching %d RSS feeds...\n", N_FEEDS);

    Article all_arts[MAX_ARTICLES];
    int total_arts = 0;
    int pos_count = 0, neg_count = 0, neutral_count = 0;
    float sent_sum = 0;

    /* Init SQLite for history */
    char db_path[512];
    expand_path(DB_PATH, db_path, sizeof(db_path));
    sqlite3 *db = NULL;
    sqlite3_open(db_path, &db);
    if (db) {
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS news_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "fetched_at TEXT, source TEXT, title TEXT,"
            "tickers TEXT, sentiment REAL, url TEXT UNIQUE)", 0,0,0);
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS daily_stats ("
            "date TEXT PRIMARY KEY, article_count INTEGER,"
            "positive_count INTEGER, negative_count INTEGER,"
            "neutral_count INTEGER, avg_sentiment REAL)", 0,0,0);
    }

    /* Fetch feeds */
    for (int f = 0; f < N_FEEDS; f++) {
        printf("  %s ... ", FEED_NAMES[f]); fflush(stdout);
        char *xml = fetch_url(FEED_URLS[f]);
        if (!xml) { printf("HTTP FAIL\n"); continue; }

        Article arts[MAX_ARTICLES];
        int n = parse_rss(xml, arts, MAX_ARTICLES);
        free(xml);
        printf("%d articles\n", n);

        for (int i = 0; i < n && total_arts < MAX_ARTICLES; i++) {
            all_arts[total_arts++] = arts[i];
            if (arts[i].sentiment > 0.1f) pos_count++;
            else if (arts[i].sentiment < -0.1f) neg_count++;
            else neutral_count++;
            sent_sum += arts[i].sentiment;

            /* Insert into SQLite */
            if (db && arts[i].url[0]) {
                char ticker_buf[128] = {0};
                for (int t = 0; t < arts[i].n_tickers; t++) {
                    if (t > 0) strncat(ticker_buf, ",", 1);
                    strncat(ticker_buf, arts[i].tickers[t], 10);
                }
                char *sql = sqlite3_mprintf(
                    "INSERT OR IGNORE INTO news_history "
                    "(fetched_at, source, title, tickers, sentiment, url) "
                    "VALUES (datetime('now'), '%q', '%q', '%q', %f, '%q')",
                    FEED_NAMES[f], arts[i].title, ticker_buf,
                    (double)arts[i].sentiment, arts[i].url);
                if (sql) { sqlite3_exec(db, sql, 0,0,0); sqlite3_free(sql); }
            }
        }
    }

    /* Get historical daily stats */
    sqlite3_stmt *stmt = NULL;
    int hist_total = 0, hist_days = 0;
    if (db) {
        sqlite3_prepare_v2(db,
            "SELECT date, article_count FROM daily_stats "
            "WHERE date >= date('now', '-7 days') AND date < date('now') "
            "ORDER BY date", -1, &stmt, 0);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            hist_total += sqlite3_column_int(stmt, 1);
            hist_days++;
        }
        sqlite3_finalize(stmt);
    }
    float avg_daily = hist_days > 0 ? (float)hist_total / hist_days : (float)total_arts;

    /* Store today's stats */
    if (db) {
        char *sql = sqlite3_mprintf(
            "INSERT OR REPLACE INTO daily_stats VALUES ("
            "date('now'), %d, %d, %d, %d, %f)",
            total_arts, pos_count, neg_count, neutral_count,
            total_arts > 0 ? (double)sent_sum / total_arts : 0.0);
        if (sql) { sqlite3_exec(db, sql, 0,0,0); sqlite3_free(sql); }
    }
    if (db) sqlite3_close(db);

    /* ── Compute features ── */
    float vol_ratio = avg_daily > 0 ? (float)total_arts / avg_daily : 1.0f;
    float f75 = (vol_ratio - 0.5f) / 2.5f;
    if (f75 < 0) f75 = 0;
    if (f75 > 1) f75 = 1;

    float avg_sent = total_arts > 0 ? sent_sum / total_arts : 0.0f;
    float f76 = (avg_sent + 1.0f) / 2.0f;
    if (f76 < 0) f76 = 0;
    if (f76 > 1) f76 = 1;

    /* ── Write output ── */
    char out[512], dir[512];
    expand_path(OUTPUT_FILE, out, sizeof(out));
    expand_path(CACHE_DIR, dir, sizeof(dir));
    mkdir(dir, 0755);

    /* Build JSON manually (avoid jansson dep for this binary) */
    char time_buf[32];
    time_t now = time(NULL);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    char json[4096];
    int n = snprintf(json, sizeof(json),
        "{\n"
        "  \"news_volume_norm\": %.4f,\n"
        "  \"news_sentiment_norm\": %.4f,\n"
        "  \"news_avg_sentiment\": %.4f,\n"
        "  \"total_articles\": %d,\n"
        "  \"positive\": %d,\n"
        "  \"negative\": %d,\n"
        "  \"neutral\": %d,\n"
        "  \"vol_ratio\": %.3f,\n"
        "  \"avg_daily_baseline\": %.1f,\n"
        "  \"fetched_at\": \"%s\"\n"
        "}\n",
        f75, f76, avg_sent, total_arts, pos_count, neg_count, neutral_count,
        vol_ratio, avg_daily, time_buf);

    int fd = open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) { write(fd, json, n); close(fd); }

    printf("[NEWS] %d articles vol=%.3f sent=%+.3f→%.3f\n",
           total_arts, f75, avg_sent, f76);

    return 0;
}
