/**
 * api_server.c — Money Room subscription API server
 * Serves market data, features, predictions for paying subscribers.
 * Port: 9091 | Endpoints: GET /api/v1/status|market|features|predictions
 * Auth: API key in X-API-Key header (validated via LemonSqueezy)
 *
 * Build: gcc -O2 -o api_server api_server.c -lcurl -ljansson -lcrypto -lm
 * Usage: ./api_server [port]
 *
 * C HTTP server using raw sockets. Fork-per-connection.
 * Each fork reads market_feed.json + room_snapshot.json, returns JSON.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <jansson.h>
#include <curl/curl.h>

#define PORT 9091
#define MAX_BUF 65536
#define MAX_CONN 16
#define API_KEY_HEADER "X-API-Key: "
#define FEED_PATH "/home/wubu2/.hermes/pm_logs/c_room/market_feed.json"
#define SNAP_PATH "/home/wubu2/.hermes/pm_logs/c_room/room_snapshot.json"

/* Read file into malloc'd string */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

/* Validate API key via LemonSqueezy */
static int validate_key(const char *key) {
    if (!key || !*key) return 0;
    /* For now: accept any non-empty key (LemonSqueezy webhook TBD) */
    /* TODO: query LemonSqueezy API to validate subscription status */
    (void)key;
    return 1;
}

/* Extract header value from HTTP request */
static const char *get_header(const char *req, const char *hdr) {
    const char *p = strstr(req, hdr);
    if (!p) return NULL;
    p += strlen(hdr);
    while (*p == ' ') p++;
    const char *e = strstr(p, "\r\n");
    if (!e) e = p + strlen(p);
    static char val[256];
    size_t len = (size_t)(e - p);
    if (len > 255) len = 255;
    memcpy(val, p, len);
    val[len] = 0;
    return val;
}

/* Build JSON response */
static char *build_response(const char *endpoint, int *resp_len) {
    json_t *root = json_object();
    json_object_set_new(root, "endpoint", json_string(endpoint));
    json_object_set_new(root, "timestamp", json_integer(time(NULL)));
    json_object_set_new(root, "version", json_string("1.0.0"));

    if (strcmp(endpoint, "/api/v1/status") == 0) {
        json_object_set_new(root, "status", json_string("operational"));
        json_object_set_new(root, "service", json_string("Money Room API"));

    } else if (strcmp(endpoint, "/api/v1/market") == 0) {
        char *feed = read_file(FEED_PATH);
        if (feed) {
            json_error_t err;
            json_t *j = json_loads(feed, 0, &err);
            if (j) {
                json_object_set(root, "market", j);
                json_decref(j);
            }
            free(feed);
        }

    } else if (strcmp(endpoint, "/api/v1/features") == 0) {
        char *snap = read_file(SNAP_PATH);
        if (snap) {
            json_error_t err;
            json_t *j = json_loads(snap, 0, &err);
            if (j) {
                json_t *feats = json_object_get(j, "features");
                if (feats) json_object_set(root, "features", feats);
                json_decref(j);
            }
            free(snap);
        }

    } else if (strcmp(endpoint, "/api/v1/predictions") == 0) {
        char *snap = read_file(SNAP_PATH);
        if (snap) {
            json_error_t err;
            json_t *j = json_loads(snap, 0, &err);
            if (j) {
                json_t *market = json_object_get(j, "market");
                json_t *votes = json_object_get(j, "vote_summary");
                json_t *stats = json_object_get(j, "stats");
                if (market) json_object_set(root, "market", market);
                if (votes) json_object_set(root, "vote_summary", votes);
                if (stats) json_object_set(root, "stats", stats);
                json_decref(j);
            }
            free(snap);
        }
    }

    char *out = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS);
    json_decref(root);
    if (resp_len) *resp_len = out ? (int)strlen(out) : 0;
    return out;
}

/* Handle one HTTP connection */
static void handle_client(int client_fd) {
    char buf[MAX_BUF];
    int n = (int)read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = 0;

    /* Parse request line */
    char method[16] = {0}, path[256] = {0};
    if (sscanf(buf, "%15s %255s", method, path) < 2) {
        close(client_fd); return;
    }

    /* Validate API key */
    const char *key = get_header(buf, API_KEY_HEADER);
    if (!validate_key(key)) {
        const char *resp = "{\"error\":\"unauthorized\",\"message\":\"Invalid or missing API key\"}";
        char hdr[512];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 401 Unauthorized\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n", strlen(resp));
        write(client_fd, hdr, strlen(hdr));
        write(client_fd, resp, strlen(resp));
        close(client_fd); return;
    }

    /* Route: only GET */
    if (strcmp(method, "GET") != 0) {
        const char *resp = "{\"error\":\"method_not_allowed\"}";
        char hdr[512];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n", strlen(resp));
        write(client_fd, hdr, strlen(hdr));
        write(client_fd, resp, strlen(resp));
        close(client_fd); return;
    }

    /* Build response */
    int resp_len;
    char *json = build_response(path, &resp_len);
    if (!json) {
        const char *err = "{\"error\":\"not_found\"}";
        char hdr[512];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n", strlen(err));
        write(client_fd, hdr, strlen(hdr));
        write(client_fd, err, strlen(err));
        close(client_fd); return;
    }

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n", resp_len);
    write(client_fd, hdr, strlen(hdr));
    write(client_fd, json, (size_t)resp_len);
    free(json);
    close(client_fd);
}

int main(int argc, char **argv) {
    int port = PORT;
    if (argc > 1) port = atoi(argv[1]);
    if (port <= 0 || port > 65535) port = PORT;

    signal(SIGCHLD, SIG_IGN);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sock); return 1;
    }
    if (listen(sock, MAX_CONN) < 0) {
        perror("listen"); close(sock); return 1;
    }

    printf("[API] Money Room API server on port %d\n", port);
    printf("[API] Endpoints: /api/v1/status | /market | /features | /predictions\n");
    printf("[API] Auth: X-API-Key header\n");

    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int client_fd = accept(sock, (struct sockaddr*)&client, &client_len);
        if (client_fd < 0) { perror("accept"); continue; }

        pid_t pid = fork();
        if (pid == 0) {
            close(sock);
            handle_client(client_fd);
            _exit(0);
        }
        close(client_fd);
    }

    close(sock);
    return 0;
}
