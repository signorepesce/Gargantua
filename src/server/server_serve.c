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

