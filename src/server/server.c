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

Worker    g_workers[SERVER_WORKERS];
pthread_t g_worker_threads[SERVER_WORKERS];
int       g_workers_started;
ConnQueue g_queue;
_Atomic int g_stop;
_Atomic int g_drain_over;
int          g_drain_ms;
_Thread_local const char *response_route;
_Thread_local int response_head;
_Thread_local char response_allow[ROUTE_ALLOW_CAP];
_Thread_local char response_origin[RESPONSE_VALUE_CAP];
_Thread_local char response_requested_headers[RESPONSE_VALUE_CAP];
_Thread_local int response_cors;
_Thread_local int response_preflight;
static _Thread_local char response_wire[RESPONSE_WIRE_CAP + 8192];
ClientLimit g_clients[256];
static pthread_mutex_t g_limit_lock = PTHREAD_MUTEX_INITIALIZER;
int g_connections_per_ip = 16;
int g_requests_per_minute = 600;
int client_limit(uint32_t ip, int action)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { return -1; }
    (void)pthread_mutex_lock(&g_limit_lock);
    int slot = -1;
    int empty = -1;
    for (int i = 0; i < 256; i++)
    {
        if (g_clients[i].used && g_clients[i].ip == ip) { slot = i; break; }
        if (!g_clients[i].used || (!g_clients[i].connections && now.tv_sec - g_clients[i].window >= 60)) { empty = i; }
    }
    if (slot < 0 && empty >= 0 && action == 1)
    {
        slot = empty;
        g_clients[slot] = (ClientLimit){ .ip = ip, .used = 1, .window = now.tv_sec };
    }
    int result = -1;
    if (slot >= 0)
    {
        ClientLimit *c = &g_clients[slot];
        if (now.tv_sec - c->window >= 60) { c->requests = 0; c->window = now.tv_sec; }
        if (action == -1)
        {
            if (c->connections > 0) { c->connections--; }
            result = 0;
        }
        else if (action == 1 && c->connections < g_connections_per_ip)
        {
            c->connections++;
            result = 0;
        }
        else if (action == 0 && c->requests < g_requests_per_minute)
        {
            c->requests++;
            result = 0;
        }
    }
    (void)pthread_mutex_unlock(&g_limit_lock);
    return result;
}

void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

int queue_push(int fd, uint32_t ip)
{
    assert(fd >= 0);
    assert(SERVER_QUEUE > 0);

    int rc = -1;

    (void)pthread_mutex_lock(&g_queue.lock);

    assert(g_queue.count >= 0);
    assert(g_queue.count <= SERVER_QUEUE);

    if (g_queue.count < SERVER_QUEUE)
    {
        g_queue.fds[g_queue.tail] = fd;
        g_queue.ips[g_queue.tail] = ip;
        g_queue.tail = (g_queue.tail + 1) % SERVER_QUEUE;
        g_queue.count++;
        (void)pthread_cond_signal(&g_queue.not_empty);
        rc = 0;
    }
    (void)pthread_mutex_unlock(&g_queue.lock);

    return rc;
}

int queue_pop(uint32_t *ip)
{
    assert(SERVER_QUEUE > 0);
    assert(SERVER_WORKERS > 0);

    int fd = -1;

    (void)pthread_mutex_lock(&g_queue.lock);

    assert(g_queue.count >= 0);
    assert(g_queue.count <= SERVER_QUEUE);

    while ((g_queue.count == 0) && (g_queue.stopping == 0)) { (void)pthread_cond_wait(&g_queue.not_empty, &g_queue.lock); }
    if (g_queue.count > 0)
    {
        fd = g_queue.fds[g_queue.head];
        *ip = g_queue.ips[g_queue.head];
        g_queue.head = (g_queue.head + 1) % SERVER_QUEUE;
        g_queue.count--;
    }
    (void)pthread_mutex_unlock(&g_queue.lock);

    return fd;
}

static const char *status_reason(int status)
{
    assert(status >= 100);
    assert(status < 600);

    switch (status)
    {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 417: return "Expectation Failed";
    case 429: return "Too Many Requests";
    case 422: return "Unprocessable Content";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Status";
    }
}

