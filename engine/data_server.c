/**
 * data_server.c — Money Room static file server
 * Serves docs/data/ JSON files on port 9090 for the website dashboard.
 * No auth. CORS-enabled. Fork-per-connection.
 *
 * Build: gcc -O2 -o data_server data_server.c
 * Usage: ./data_server [port] [data_dir]
 *        Default port: 9090
 *        Default dir:  docs/data/ relative to CWD
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
#include <errno.h>
#include <libgen.h>

#define PORT 9090
#define MAX_BUF 65536
#define MAX_CONN 16
#define DATA_DIR "docs/data/"

/* Helper: suppress unused-result warning */
static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) break;
        buf += n;
        len -= (size_t)n;
    }
}

/* MIME type by file extension */
static const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0)   return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcmp(dot, ".svg") == 0)  return "image/svg+xml";
    if (strcmp(dot, ".png") == 0)  return "image/png";
    if (strcmp(dot, ".txt") == 0)  return "text/plain; charset=utf-8";
    if (strcmp(dot, ".csv") == 0)  return "text/csv; charset=utf-8";
    if (strcmp(dot, ".bin") == 0)  return "application/octet-stream";
    return "application/octet-stream";
}

/* Read entire file into malloc'd buffer */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    if (out_len) *out_len = n;
    return buf;
}

/* Simple path sanitizer — reject '..', absolute paths, and embedded nulls */
static int safe_path(const char *path) {
    if (!path || path[0] != '/') return 0;  /* must start with / */
    if (strstr(path, "..")) return 0;
    if (strchr(path, '~')) return 0;
    /* Check for embedded nulls (null injection attack) */
    size_t len = strlen(path);
    if (len != strnlen(path, 2048)) return 0;
    return 1;
}

/* Handle one HTTP connection */
static void handle_client(int client_fd, const char *data_root) {
    char buf[MAX_BUF];
    int n = (int)read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = 0;

    /* Parse request line */
    char method[16] = {0}, path[1024] = {0};
    if (sscanf(buf, "%15s %1023s", method, path) < 2) {
        close(client_fd); return;
    }

    /* Only GET */
    if (strcmp(method, "GET") != 0) {
        const char *body = "{\"error\":\"method_not_allowed\"}";
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n", strlen(body));
        write_all(client_fd, hdr, (size_t)hlen);
        write_all(client_fd, body, strlen(body));
        close(client_fd);
        return;
    }

    /* Validate path */
    if (!safe_path(path)) {
        const char *body = "{\"error\":\"bad_request\"}";
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n", strlen(body));
        write_all(client_fd, hdr, (size_t)hlen);
        write_all(client_fd, body, strlen(body));
        close(client_fd);
        return;
    }

    /* Build filesystem path:
       /data/file.json -> $data_root/file.json
       /               -> index (list available files) */
    int is_root = (strcmp(path, "/") == 0);
    char fpath[2048];
    if (is_root) {
        /* For /, show directory listing as JSON */
        char cmd[4096];
        n = snprintf(cmd, sizeof(cmd), "ls -1 '%s' 2>/dev/null", data_root);
        (void)n;
        FILE *ls = popen(cmd, "r");
        if (!ls) {
            const char *body = "{\"error\":\"internal_error\"}";
            char hdr[512];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n", strlen(body));
            write_all(client_fd, hdr, (size_t)hlen);
            write_all(client_fd, body, strlen(body));
            close(client_fd);
            return;
        }
        /* Build JSON array of filenames */
        char *list = NULL;
        size_t list_len = 0;
        FILE *mf = open_memstream(&list, &list_len);
        if (mf) {
            fprintf(mf, "[");
            char line[256];
            int first = 1;
            while (fgets(line, sizeof(line), ls)) {
                size_t l = strlen(line);
                if (l > 0 && line[l-1] == '\n') line[l-1] = 0;
                if (strlen(line) == 0) continue;
                if (!first) fprintf(mf, ",\n");
                else first = 0;
                fprintf(mf, "  \"%s\"", line);
            }
            fprintf(mf, "\n]");
            fclose(mf);
        }
        pclose(ls);

        if (!list) {
            const char *body = "{\"error\":\"no_files\"}";
            char hdr[512];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n", strlen(body));
            write_all(client_fd, hdr, (size_t)hlen);
            write_all(client_fd, body, strlen(body));
        } else {
            char hdr[512];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n", list_len);
            write_all(client_fd, hdr, (size_t)hlen);
            write_all(client_fd, list, list_len);
            free(list);
        }
        close(client_fd);
        return;
    }

    /* Normal file: /data/filename -> $data_root/filename */
    const char *fname = path;
    if (strncmp(fname, "/data/", 6) == 0) {
        fname += 6;
    } else {
        fname++;  /* skip leading / */
    }

    n = snprintf(fpath, sizeof(fpath), "%s/%s", data_root, fname);
    (void)n;

    size_t file_len;
    char *content = read_file(fpath, &file_len);
    if (!content) {
        char body[512];
        int blen = snprintf(body, sizeof(body),
            "{\"error\":\"not_found\",\"path\":\"%s\"}", path);
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n", blen);
        write_all(client_fd, hdr, (size_t)hlen);
        write_all(client_fd, body, (size_t)blen);
        close(client_fd);
        return;
    }

    /* Send file */
    const char *mime = mime_type(fpath);
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Connection: close\r\n\r\n", mime, file_len);
    write_all(client_fd, hdr, (size_t)hlen);
    write_all(client_fd, content, file_len);
    free(content);
    close(client_fd);
}

int main(int argc, char **argv) {
    int port = PORT;
    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p > 0 && p <= 65535) port = p;
    }

    const char *data_root = DATA_DIR;
    if (argc > 2) data_root = argv[2];

    /* Resolve data_root to absolute path for child forks */
    char abs_root[4096];
    if (data_root[0] == '/') {
        snprintf(abs_root, sizeof(abs_root), "%s", data_root);
    } else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            return 1;
        }
        snprintf(abs_root, sizeof(abs_root), "%s/%s", cwd, data_root);
    }

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
        perror("bind");
        close(sock);
        return 1;
    }
    if (listen(sock, MAX_CONN) < 0) {
        perror("listen");
        close(sock);
        return 1;
    }

    printf("[DATA] Money Room data server on port %d\n", port);
    printf("[DATA] Serving: %s/\n", abs_root);
    printf("[DATA] Endpoints: GET /, GET /<filename>, GET /data/<filename>\n");

    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int client_fd = accept(sock, (struct sockaddr*)&client, &client_len);
        if (client_fd < 0) { perror("accept"); continue; }

        pid_t pid = fork();
        if (pid == 0) {
            close(sock);
            handle_client(client_fd, abs_root);
            _exit(0);
        }
        close(client_fd);
    }

    close(sock);
    return 0;
}
