#include "server.h"
#include "arena.h"
#include "http.h"
#include "config.h"
#include "route.h"
#include "response.h"
#include "static_files.h"
#include "db.h"
#include "gargantua.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SERVER_BACKLOG    32
#define SERVER_SEND_MAX   4096
#define SERVER_READ_STEPS 4096
#define TEXT_CONTENT_TYPE           "text/plain; charset=utf-8"
#define JSON_CONTENT_TYPE           "application/json; charset=utf-8"

typedef struct
{
    int             fds[SERVER_QUEUE];
    uint32_t        ips[SERVER_QUEUE];
    int             head;
    int             tail;
    int             count;
    int             stopping;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} ConnQueue;

typedef struct
{
    uint32_t      ip;
    Arena       arena;
    unsigned char backing[SERVER_ARENA];
    char          req[SERVER_REQ_MAX];
} Worker;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal stop must be lock free");
typedef struct
{
    uint32_t ip;
    int used;
    int connections;
    int requests;
    time_t window;
} ClientLimit;
#include "server_internal.h"

int send_response(int sock, int status, const char *content_type, const char *body, int keep_alive)
{
    assert(content_type != NULL);
    assert(body != NULL);

    char error[ERROR_CAP];
    if (status >= 400)
    {
        if (error_json_write(status, body, error, sizeof(error)) != 0) { return -1; }
        body         = error;
        content_type = JSON_CONTENT_TYPE;
    }

    size_t blen = ((status == 204) || (status == 205) || (status == 304))
                      ? 0u : strlen(body);

    return send_bytes(sock, status, content_type, body, blen, keep_alive);
}

static int handle_expect_continue(int sock, const HttpRequest *req, HttpParseResult parsed, int *already_sent)
{
    assert(req != NULL);
    assert(already_sent != NULL);

    if ((req->header_len == 0u) || (*already_sent != 0)) { return 0; }

    size_t      expect_len = 0u;
    const char *expect     = http_find_header(req, "Expect", &expect_len);
    if (expect == NULL) { return 0; }

    if ((req->minor_version != 1) || (expect_len != 12u) || (strncasecmp(expect, "100-continue", 12u) != 0)) { return -3; }

    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if ((parsed != HTTP_PARSE_OK) && (send_all(sock, interim, sizeof(interim) - 1u) != 0)) { return -1; }

    *already_sent = 1;

    return 0;
}

int read_request(int sock, char *buf, size_t cap, size_t *len, HttpRequest *req)
{
    assert(buf != NULL);
    assert(len != NULL);
    assert(req != NULL);

    HttpParseResult pr = HTTP_PARSE_NEED_MORE;
    int             guard = 0;
    int             sent_continue = 0;
    struct timespec deadline;
    if (deadline_after(&deadline, SERVER_TIMEOUT_S) != 0) { return -1; }

    while (guard < SERVER_READ_STEPS)
    {
        guard++;

        if (*len > 0u)
        {
            pr = http_parse_request(buf, *len, req);
            if (pr == HTTP_PARSE_ERROR) { return -1; }
            if ((req->header_len > 0u) && (req->content_length >= 0) && ((unsigned long)req->content_length > (unsigned long)SERVER_BODY_MAX)) { return -2; }
            int expect_rc = handle_expect_continue(sock, req, pr, &sent_continue);
            if (expect_rc != 0) { return expect_rc; }
            if (pr == HTTP_PARSE_OK)
            {
                if (req->body_len > (size_t)SERVER_BODY_MAX) { return -2; }
                return 1;
            }
        }

        if (*len + 1u >= cap) { return -2; }

        if (set_timeout_until(sock, SO_RCVTIMEO, &deadline) != 0) { return -1; }
        ssize_t got = recv(sock, buf + *len, cap - *len - 1u, 0);
        if (got == 0) { return (*len == 0u) ? 0 : -1; }
        if ((got < 0) && (errno == EINTR)) { continue; }
        if (got < 0) { return -1; }

        *len += (size_t)got;
        buf[*len] = '\0';
    }

    return -1;
}

int split_path(char *path, char **query)
{
    assert(path != NULL);
    assert(query != NULL);

    char *mark = strchr(path, '?');
    if (mark != NULL)
    {
        *mark  = '\0';
        *query = mark + 1;
    }
    else
    {
        *query = NULL;
    }

    if (strchr(path, '\\') != NULL) { return -1; }
    for (size_t i = 0u; path[i] != '\0'; i++)
    {
        if ((path[i] == '%') && (path[i + 1u] != '\0') && (path[i + 2u] != '\0'))
        {
            char a = path[i + 1u];
            char b = path[i + 2u];
            int encoded_slash = ((a == '2') && ((b == 'f') || (b == 'F')));
            int encoded_backslash = ((a == '5') && ((b == 'c') || (b == 'C')));
            if ((encoded_slash != 0) || (encoded_backslash != 0)) { return -1; }
        }
    }
    if (url_decode(path) != 0) { return -1; }
    if ((strchr(path, '\\') != NULL) || (path[0] != '/')) { return -1; }

    const char *segment = path + 1;
    while (*segment != '\0')
    {
        const char *slash = strchr(segment, '/');
        size_t n = (slash != NULL) ? (size_t)(slash - segment)
                                   : strlen(segment);
        if ((n == 0u) || (n == 1u && segment[0] == '.') || (n == 2u && segment[0] == '.' && segment[1] == '.')) { return -1; }
        if (slash == NULL) { break; }
        segment = slash + 1;
        if (*segment == '\0') { return -1; }
    }
    return 0;
}

static int header_has_token(const char *value, size_t len, const char *wanted)
{
    size_t wanted_len = strlen(wanted);
    size_t at = 0u;

    while (at < len)
    {
        while (at < len && (value[at] == ' ' || value[at] == '\t' || value[at] == ',')) { at++; }
        size_t start = at;
        while (at < len && value[at] != ',') { at++; }
        size_t end = at;
        while (end > start && (value[end - 1u] == ' ' || value[end - 1u] == '\t')) { end--; }
        if ((end - start == wanted_len) && (strncasecmp(value + start, wanted, wanted_len) == 0)) { return 1; }
    }
    return 0;
}

int request_header_has_token(const HttpRequest *req, const char *name, const char *wanted)
{
    size_t name_len = strlen(name);
    for (int i = 0; (i < req->num_headers) && (i < HTTP_MAX_HEADERS); i++)
    {
        const HttpHeader *header = &req->headers[i];
        if ((header->name_len == name_len) && (strncasecmp(header->name, name, name_len) == 0) && (header_has_token(header->value, header->value_len, wanted) != 0)) { return 1; }
    }
    return 0;
}

static int header_single_value(const HttpRequest *req, const char *name, char *out, size_t cap)
{
    int found = 0;
    size_t name_len = strlen(name);
    out[0] = '\0';
    for (int i = 0; i < req->num_headers && i < HTTP_MAX_HEADERS; i++)
    {
        const HttpHeader *h = &req->headers[i];
        if ((h->name_len != name_len) || (strncasecmp(h->name, name, name_len) != 0)) { continue; }
        if (found || h->value_len >= cap) { return -1; }
        memcpy(out, h->value, h->value_len);
        out[h->value_len] = '\0';
        if (!response_value(out)) { return -1; }
        found = 1;
    }
    return found;
}

static int list_contains(const char *list, const char *value, int exact)
{
    size_t len = strlen(value);
    const char *at = list;
    while (*at != '\0')
    {
        while (*at == ' ' || *at == '\t') { at++; }
        const char *end = strchr(at, ',');
        const char *next = (end != NULL) ? end + 1 : at + strlen(at);
        if (end == NULL) { end = next; }
        while (end > at && (end[-1] == ' ' || end[-1] == '\t')) { end--; }
        if (len > 0u && (size_t)(end - at) == len && (exact ? memcmp(at, value, len) == 0 : strncasecmp(at, value, len) == 0)) { return 1; }
        at = next;
    }
    return 0;
}

void cors_prepare(const HttpRequest *req)
{
    const char *origins = config_str("cors.origins", "");
    response_cors = (*origins != '\0');
    char origin[RESPONSE_VALUE_CAP];
    if (response_cors && header_single_value(req, "Origin", origin, sizeof(origin)) == 1 && strcmp(origin, "*") != 0 && list_contains(origins, origin, 1)) { memcpy(response_origin, origin, strlen(origin) + 1u); }
}

static int cors_preflight(const HttpRequest *req, const char *allow)
{
    char method[HTTP_MAX_METHOD];
    char requested[RESPONSE_VALUE_CAP];
    int m = header_single_value(req, "Access-Control-Request-Method", method, sizeof(method));
    int h = header_single_value(req, "Access-Control-Request-Headers", requested, sizeof(requested));
    if (!response_cors || strcmp(req->method, "OPTIONS") != 0 || (m == 0 && h == 0)) { return 0; }
    response_preflight = 1;
    if (m != 1 || h < 0 || !response_token(method)) { return 400; }
    if (response_origin[0] == '\0' || !list_contains(allow, method, 1)) { return 403; }
    if (h == 1)
    {
        char *at = requested;
        for (;;)
        {
            char *comma = strchr(at, ',');
            if (comma != NULL) { *comma = '\0'; }
            while (*at == ' ' || *at == '\t') { at++; }
            size_t len = strlen(at);
            while (len > 0u && (at[len - 1u] == ' ' || at[len - 1u] == '\t')) { at[--len] = '\0'; }
            if (!response_token(at)) { return 400; }
            if (!list_contains(config_str("cors.headers", ""), at, 0)) { return 403; }
            if (comma == NULL) { break; }
            at = comma + 1;
        }
        if (header_single_value(req, "Access-Control-Request-Headers", response_requested_headers, sizeof(response_requested_headers)) != 1) { return 400; }
    }
    return 204;
}

int is_health_path(const char *path)
{
    assert(path != NULL);

    return (strcmp(path, "/health/live") == 0) ||
           (strcmp(path, "/health/ready") == 0);
}

int serve_health(int sock, const HttpRequest *req, const char *path, int keep_alive, int *status_out)
{
    assert(req != NULL);
    assert(path != NULL);
    assert(status_out != NULL);

    if ((strcmp(req->method, "GET") != 0) && (strcmp(req->method, "HEAD") != 0))
    {
        (void)snprintf(response_allow, sizeof(response_allow), "GET, HEAD, OPTIONS");
        *status_out = (strcmp(req->method, "OPTIONS") == 0) ? 204 : 405;
        return send_response(sock, *status_out, JSON_CONTENT_TYPE, "", keep_alive);
    }

    int live = (strcmp(path, "/health/live") == 0);
    int up   = live ? 1 : ((g_stop == 0) && (db_ready() != 0));

    *status_out = up ? 200 : 503;

    return send_response(sock, *status_out, JSON_CONTENT_TYPE, up ? "{\"status\":\"up\"}" : "{\"status\":\"down\"}", keep_alive);
}

int serve_preflight(int sock, const HttpRequest *req, int keep_alive, int *status_out, int *handled)
{
    assert(req != NULL);
    assert(status_out != NULL);
    assert(handled != NULL);

    *handled = 0;
    if (strcmp(req->method, "OPTIONS") != 0) { return 0; }

    int preflight = cors_preflight(req, response_allow);
    if (preflight == 0) { return 0; }

    if (preflight != 204) { response_origin[0] = '\0'; }

    *handled    = 1;
    *status_out = preflight;

    return send_response(sock, preflight, JSON_CONTENT_TYPE, (preflight == 204) ? "" : "preflight rejected", keep_alive);
}
