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

static int serve_static(Worker *w, int sock, const HttpRequest *req, const char *path, int keep_alive, int *status_out, int *handled)
{
    assert(w != NULL);
    assert(req != NULL);
    assert(path != NULL);
    assert(status_out != NULL);
    assert(handled != NULL);

    *handled = 0;

    if ((static_files_enabled() == 0) || ((strcmp(req->method, "GET") != 0) && (strcmp(req->method, "HEAD") != 0))) { return 0; }

    char *buffer = arena_alloc(&w->arena, (size_t)STATIC_MAX_BYTES);
    if (buffer == NULL) { return 0; }

    size_t      len          = 0u;
    const char *content_type = NULL;
    if (static_file_read(path, buffer, (size_t)STATIC_MAX_BYTES, &len, &content_type) != 0) { return 0; }

    response_allow[0] = '\0';
    response_route    = "<static>";
    *handled          = 1;
    *status_out       = 200;

    return send_bytes(sock, 200, content_type, buffer, len, keep_alive);
}

static int serve_missing_route(int sock, const HttpRequest *req, const RequestParams *params, int allowed, int keep_alive, int *status_out)
{
    assert(req != NULL);
    assert(params != NULL);
    assert(status_out != NULL);

    int status = (params->invalid || (allowed < 0)) ? 400
               : (allowed ? 405 : 404);

    if ((allowed > 0) && (params->invalid == 0) && (strcmp(req->method, "OPTIONS") == 0)) { status = 204; }

    const char *message = (status == 400) ? "bad path parameter"
                        : (status == 405) ? "method not allowed"
                        : (status == 204) ? "" : "no route";

    *status_out = status;

    return send_response(sock, status, JSON_CONTENT_TYPE, message, keep_alive);
}

static int openapi_needs_auth(const char *path)
{
    assert(path != NULL);

    return (strcmp(path, "/openapi.json") == 0) &&
           (strcmp(config_str("openapi.public", "false"), "true") != 0) &&
           (auth_gate(0, "", 0) != 0);
}

static int serve_handler_error(int sock, int rc, int keep_alive, int *status_out)
{
    assert(status_out != NULL);

    if (request_failed() == 1)
    {
        *status_out = request_fail_status();
        return send_response(sock, *status_out, JSON_CONTENT_TYPE, request_fail_message(), keep_alive);
    }

    if (rc == -2)
    {
        *status_out = 400;
        return send_response(sock, 400, JSON_CONTENT_TYPE, "bad parameter or body", keep_alive);
    }

    char message[128];
    (void)snprintf(message, sizeof(message), "response larger than %d bytes", (int)SERVER_BODY_MAX);
    *status_out = 500;

    return send_response(sock, 500, JSON_CONTENT_TYPE, message, keep_alive);
}

static const Route *resolve_route(const HttpRequest *req, const char *path, RequestParams *params, int *allowed)
{
    assert(req != NULL);
    assert(path != NULL);
    assert(params != NULL);
    assert(allowed != NULL);

    params->path_count  = 0;
    params->query_count = 0;
    params->invalid     = 0;

    const Route *route = route_find(req->method, path, params);
    response_route = (route == NULL) ? "<unmatched>" : route->url;

    *allowed = 0;
    if ((route == NULL) || (strcmp(req->method, "OPTIONS") == 0)) { *allowed = route_allow(path, response_allow, sizeof(response_allow)); }

    return route;
}

static char *prepare_handler(Worker *w, const HttpRequest *req)
{
    assert(w != NULL);
    assert(req != NULL);

    request_bind(req);
    arena_bind(&w->arena);

    char *body = arena_alloc(&w->arena, (size_t)SERVER_BODY_MAX);
    if (body != NULL) { request_fail_reset(); }

    return body;
}

int serve_request(Worker *w, int sock, const HttpRequest *req, char *path, char *query, char *body_in, size_t body_len, int keep_alive, int *status_out)
{
    assert(w != NULL);
    assert(req != NULL);
    assert(path != NULL);
    assert(status_out != NULL);

    if (is_health_path(path) != 0) { return serve_health(sock, req, path, keep_alive, status_out); }

    RequestParams params;
    int allowed = 0;
    const Route *route = resolve_route(req, path, &params, &allowed);

    int handled = 0;
    int sent = serve_preflight(sock, req, keep_alive, status_out, &handled);
    if (handled != 0) { return sent; }

    if (route == NULL)
    {
        int served = 0;
        int rc = serve_static(w, sock, req, path, keep_alive, status_out, &served);
        if (served != 0) { return rc; }
        return serve_missing_route(sock, req, &params, allowed, keep_alive, status_out);
    }

    if (strcmp(req->method, "OPTIONS") != 0) { response_allow[0] = '\0'; }

    if (query_parse(&params, query) != 0)
    {
        *status_out = 400;
        return send_response(sock, 400, JSON_CONTENT_TYPE, "bad query", keep_alive);
    }

    char *body = prepare_handler(w, req);
    if (body == NULL)
    {
        *status_out = 503;
        return send_response(sock, 503, JSON_CONTENT_TYPE, "out of memory", keep_alive);
    }

    if (openapi_needs_auth(path) != 0)
    {
        *status_out = 401;
        return send_response(sock, 401, JSON_CONTENT_TYPE, "authentication required", keep_alive);
    }

    assert(route->handler != NULL);
    int rc = dispatch_route(route->handler, &params, body_in, body_len, body, (size_t)SERVER_BODY_MAX);

    if ((request_failed() == 1) || (rc != 0)) { return serve_handler_error(sock, rc, keep_alive, status_out); }

    *status_out = response_status_get(route->status);

    return send_response(sock, *status_out, route->content_type, body, keep_alive);
}

void log_request(const struct timeval *start, const char *method, const char *path, int status)
{
    assert(start != NULL);
    assert(method != NULL);
    (void)path;

    struct timeval now;
    (void)gettimeofday(&now, NULL);

    long usec = ((now.tv_sec - start->tv_sec) * 1000000L) +
                (now.tv_usec - start->tv_usec);

    const char *label = response_route == NULL ? "<unmatched>" : response_route;
    char safe[ROUTE_MAX_URL];
    size_t at = 0u;
    for (; at + 1u < sizeof(safe) && label[at]; at++)
    {
        unsigned char c = (unsigned char)label[at];
        safe[at] = c < 32u || c > 126u || c == '"' || c == '\\' ? '_' : (char)c;
    }
    safe[at] = '\0';
    flockfile(stdout);
    (void)printf("{\"method\":\"%s\",\"route\":\"%s\",\"status\":%d,\"duration_us\":%ld}\n", method, safe, status, usec);
    funlockfile(stdout);
    (void)fflush(stdout);
}

void reset_response_state(void)
{
    auth_reset();
    request_fail_reset();
    response_reset();

    response_route     = NULL;
    response_head      = 0;
    response_cors      = 0;
    response_preflight = 0;

    response_allow[0]             = '\0';
    response_origin[0]            = '\0';
    response_requested_headers[0] = '\0';
}

void send_read_error(int sock, int rc)
{
    if (rc == -2)
    {
        char message[128];
        (void)snprintf(message, sizeof(message), "request larger than %d bytes", (int)SERVER_REQ_MAX);
        (void)send_response(sock, 413, JSON_CONTENT_TYPE, message, 0);
        return;
    }

    if (rc == -3) { (void)send_response(sock, 417, JSON_CONTENT_TYPE, "unsupported expectation", 0); }
}

int keep_alive_wanted(const HttpRequest *req, int served)
{
    assert(req != NULL);

    int keep = (req->minor_version == 1) ? 1 : 0;

    if (request_header_has_token(req, "Connection", "close") != 0) { keep = 0; }
    if ((req->minor_version == 0) && (request_header_has_token(req, "Connection", "keep-alive") != 0)) { keep = 1; }
    if (served == (SERVER_KEEPALIVE_MAX - 1)) { keep = 0; }

    return keep;
}

void finish_request(Worker *w)
{
    assert(w != NULL);

    transaction_abort_if_open();
    auth_reset();
    request_fail_reset();
    request_bind(NULL);
    arena_bind(NULL);

    if (arena_reset(&w->arena) != 0) { (void)fprintf(stderr, "gargantua: out-of-bounds write!\n"); }
}

int consume_message(Worker *w, const HttpRequest *req, size_t *len)
{
    assert(w != NULL);
    assert(req != NULL);
    assert(len != NULL);

    if (req->message_len > *len) { return -1; }

    size_t left = *len - req->message_len;
    if (left > 0u) { memmove(w->req, w->req + req->message_len, left); }

    *len = left;
    w->req[*len] = '\0';

    return 0;
}
