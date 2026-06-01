/*
 * ratelimit.h — E18: Rate limit protection for C API pipelines
 *
 * Generic retry/backoff + rate-limit header parsing library.
 * Include in any C pipeline that makes HTTP requests.
 *
 * Usage:
 *   #include "ratelimit.h"
 *
 *   HttpBuf *buf = http_get_retry(url, 3, 1000);  // 3 retries, 1s base delay
 *   if (buf) { ... use data ... free_buf(buf); }
 *
 *   // Or use the curl wrapper with full rate-limit header parsing:
 *   CURL *curl = curl_easy_init();
 *   curl_easy_setopt(curl, CURLOPT_URL, url);
 *   // ... set up write callback etc ...
 *   int rc = curl_easy_perform_retry(curl, 3, 2000);
 *   // Check rate-limit headers:
 *   long remaining = get_rate_limit_remaining(curl);
 *   // If remaining == 0, we've been rate-limited
 */

#ifndef RATELIMIT_H
#define RATELIMIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Sleep helper (nanosecond precision) ─── */
static inline void ratelimit_sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ─── Exponential backoff with jitter ─── */
/* Returns delay in ms for retry attempt `attempt` (0-indexed).
 * Base delay: first retry waits `base_ms * 2^attempt + jitter`
 * Jitter: random ±25% to avoid thundering herd */
static inline long ratelimit_backoff_ms(long base_ms, int attempt) {
    if (attempt < 0) return 0;
    long delay = base_ms * (1L << attempt);  /* double each retry */
    /* Add jitter: ±25% */
    long jitter_range = delay / 4;
    if (jitter_range > 0) {
        long jitter = rand() % (jitter_range * 2 + 1) - jitter_range;
        delay += jitter;
    }
    if (delay < 10) delay = 10;
    if (delay > 60000) delay = 60000;  /* cap at 60s */
    return delay;
}

/* ─── Retry-until callback pattern ─── */
/* Wraps a curl perform with retries.
 * Returns CURLE_OK on success, last error code on total failure.
 * Logs retry attempts to stderr. */
static inline CURLcode curl_easy_perform_retry(CURL *curl, int max_retries, long base_ms) {
    if (!curl) return CURLE_FAILED_INIT;
    if (max_retries < 0) max_retries = 0;

    CURLcode res;
    int attempt = 0;

    /* Seed rand for jitter */
    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)curl); seeded = 1; }

    do {
        res = curl_easy_perform(curl);

        if (res == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            /* Success — 2xx */
            if (http_code >= 200 && http_code < 300) return CURLE_OK;

            /* Rate-limited (429 Too Many Requests) or server error (5xx) — retry */
            if (http_code == 429 || http_code >= 500) {
                if (attempt < max_retries) {
                    /* Check for Retry-After header */
                    long retry_after = 0;
                    curl_easy_getinfo(curl, CURLINFO_RETRY_AFTER, &retry_after);
                    long delay = retry_after > 0 ? retry_after * 1000L : ratelimit_backoff_ms(base_ms, attempt);
                    fprintf(stderr, "[ratelimit] HTTP %ld on attempt %d, retrying in %ldms\n",
                            http_code, attempt + 1, delay);
                    ratelimit_sleep_ms(delay);
                    attempt++;
                    continue;
                }
                fprintf(stderr, "[ratelimit] HTTP %ld, exhausted %d retries\n",
                        http_code, max_retries);
                return CURLE_HTTP_RETURNED_ERROR;
            }

            /* Other HTTP errors (4xx except 429) — don't retry */
            return CURLE_HTTP_RETURNED_ERROR;
        }

        /* Curl-level error (timeout, connection, etc.) — retry if attempts remain */
        if (attempt < max_retries) {
            long delay = ratelimit_backoff_ms(base_ms, attempt);
            fprintf(stderr, "[ratelimit] curl error %d on attempt %d: %s, retrying in %ldms\n",
                    res, attempt + 1, curl_easy_strerror(res), delay);
            ratelimit_sleep_ms(delay);
            attempt++;
        } else {
            fprintf(stderr, "[ratelimit] curl error %d, exhausted %d retries: %s\n",
                    res, max_retries, curl_easy_strerror(res));
            return res;
        }
    } while (1);
}

/* ─── Read X-RateLimit-Remaining header ─── */
/* Returns -1 if header not found */
static inline long get_rate_limit_remaining(CURL *curl) {
    long remaining = -1;
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, NULL); /* dummy call to init */
    /* Try reading from the response headers */
    struct curl_slist *headers = NULL;
    curl_easy_getinfo(curl, CURLINFO_COOKIELIST, NULL);

    /* Alternative: use CURLINFO_HEADER_LIST for response headers */
    struct curl_header *header = NULL;
    if (curl_easy_header(curl, "X-RateLimit-Remaining", 0, CURLH_HEADER, -1, &header) == CURLHE_OK && header) {
        remaining = atol(header->value);
    }
    return remaining;
}

/* ─── Wrapper for existing http_get pattern ─── */
/* Use this in place of raw http_get() calls.
 * Replaces the typical pattern in feed_bridge.c, etc. */
typedef struct { char *data; size_t len; } HttpBuf;

static size_t ratelimit_write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    HttpBuf *b = (HttpBuf*)user;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    memcpy(p + b->len, ptr, n);
    b->data = p;
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* Fetch URL with retry. Returns HttpBuf (caller must free with free_buf).
 * NULL on total failure. */
static inline HttpBuf *http_get_retry(const char *url, int max_retries, long base_ms) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    HttpBuf *buf = (HttpBuf*)calloc(1, sizeof(HttpBuf));
    if (!buf) { curl_easy_cleanup(curl); return NULL; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: money-room/2.0");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ratelimit_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform_retry(curl, max_retries, base_ms);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf->data);
        free(buf);
        return NULL;
    }
    return buf;
}

/* Free an HttpBuf returned by http_get_retry */
static inline void free_buf(HttpBuf *buf) {
    if (buf) {
        free(buf->data);
        free(buf);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* RATELIMIT_H */
