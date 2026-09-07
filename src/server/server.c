#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "server.h"
#include "dynbuf.h"
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
    DynBuf        req;
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

int deadline_after(struct timespec *deadline, int seconds)
{
    if ((deadline == NULL) || (seconds <= 0) || (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)) { return -1; }
    deadline->tv_sec += (time_t)seconds;
    return 0;
}

int set_timeout_until(int sock, int option, const struct timespec *deadline)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { return -1; }

    time_t sec = deadline->tv_sec - now.tv_sec;
    long nsec = deadline->tv_nsec - now.tv_nsec;
    if (nsec < 0L)
    {
        sec--;
        nsec += 1000000000L;
    }
    if ((sec < 0) || ((sec == 0) && (nsec <= 0L))) { return -1; }

    struct timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = (suseconds_t)((nsec + 999L) / 1000L);
    if (tv.tv_usec >= 1000000)
    {
        tv.tv_sec++;
        tv.tv_usec -= 1000000;
    }
    return (setsockopt(sock, SOL_SOCKET, option, &tv, sizeof(tv)) == 0)
               ? 0 : -1;
}

static void iovec_advance(struct iovec *iov, int *count, size_t consumed)
{
    assert(iov != NULL);
    assert(count != NULL);

    size_t left = consumed;
    int    i    = 0;

    while ((i < *count) && (left > 0u))
    {
        if (left >= iov[i].iov_len)
        {
            left -= iov[i].iov_len;
            iov[i].iov_len = 0u;
            i++;
        }
        else
        {
            iov[i].iov_base = (char *)iov[i].iov_base + left;
            iov[i].iov_len -= left;
            left = 0u;
        }
    }

    while ((*count > 0) && (iov[0].iov_len == 0u))
    {
        if (*count == 2) { iov[0] = iov[1]; }
        (*count)--;
    }
}

static int send_header_body(int sock, const char *header, size_t hlen, const char *body, size_t blen)
{
    assert(header != NULL);
    assert(hlen > 0u);

    struct timespec deadline;
    if (deadline_after(&deadline, SERVER_TIMEOUT_S) != 0) { return -1; }
    struct iovec iov[2] = {{0}};
    int          count = 1;

    iov[0].iov_base = (void *)(size_t)header;
    iov[0].iov_len  = hlen;

    if ((body != NULL) && (blen > 0u))
    {
        iov[1].iov_base = (void *)(size_t)body;
        iov[1].iov_len  = blen;
        count = 2;
    }

    size_t total = hlen + ((count == 2) ? blen : 0u);
    size_t sent  = 0u;
    int    guard = 0;

    while ((sent < total) && (guard < SERVER_SEND_MAX))
    {
        if (set_timeout_until(sock, SO_SNDTIMEO, &deadline) != 0) { return -1; }
        ssize_t w = writev(sock, iov, count);
        if ((w < 0) && (errno == EINTR)) { continue; }
        if (w <= 0) { return -1; }

        sent += (size_t)w;
        guard++;

        iovec_advance(iov, &count, (size_t)w);
    }

    return (sent == total) ? 0 : -1;
}

int send_all(int sock, const char *buf, size_t len)
{
    assert(buf != NULL);
    assert(len > 0u);

    struct timespec deadline;
    if (deadline_after(&deadline, SERVER_TIMEOUT_S) != 0) { return -1; }

    size_t sent  = 0u;
    int    guard = 0;

    while ((sent < len) && (guard < SERVER_SEND_MAX))
    {
        if (set_timeout_until(sock, SO_SNDTIMEO, &deadline) != 0) { return -1; }
        ssize_t w = send(sock, buf + sent, len - sent, 0);
        if ((w < 0) && (errno == EINTR)) { continue; }
        if (w <= 0) { return -1; }
        sent += (size_t)w;
        guard++;
    }

    return (sent == len) ? 0 : -1;
}

static int header_append(char *header, size_t cap, size_t *at, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static int header_append(char *header, size_t cap, size_t *at, const char *format, ...)
{
    assert(header != NULL);
    assert(at != NULL);
    assert(format != NULL);

    if (*at >= cap) { return -1; }

    va_list args;
    va_start(args, format);
    int n = vsnprintf(header + *at, cap - *at, format, args);
    va_end(args);

    if ((n < 0) || ((size_t)n >= (cap - *at))) { return -1; }

    *at += (size_t)n;

    return 0;
}

static int write_cors_headers(char *header, size_t cap, size_t *at, int status)
{
    assert(header != NULL);
    assert(at != NULL);

    if (response_cors)
    {
        const char *vary = response_preflight
            ? ", Access-Control-Request-Method, Access-Control-Request-Headers" : "";
        if (header_append(header, cap, at, "Vary: Origin%s\r\n", vary) != 0) { return -1; }
    }

    if (response_origin[0] == '\0') { return 0; }

    const char *credentials =
        (strcmp(config_str("cors.credentials", "false"), "true") == 0)
            ? "Access-Control-Allow-Credentials: true\r\n" : "";

    if (header_append(header, cap, at, "Access-Control-Allow-Origin: %s\r\n" "Access-Control-Expose-Headers: Location\r\n%s", response_origin, credentials) != 0) { return -1; }

    if ((response_preflight == 0) || (status != 204)) { return 0; }

    return header_append(header, cap, at, "Access-Control-Allow-Methods: %s\r\n%s%s%s", response_allow, response_requested_headers[0] ? "Access-Control-Allow-Headers: " : "", response_requested_headers, response_requested_headers[0] ? "\r\n" : "");
}

int send_bytes(int sock, int status, const char *content_type, const char *body, size_t blen, int keep_alive)
{
    assert(content_type != NULL);
    assert(body != NULL);

    char  *header = response_wire;
    size_t cap    = sizeof(response_wire);
    size_t at     = 0u;

    if (header_append(header, cap, &at, "HTTP/1.1 %d %s\r\n" "Content-Type: %s\r\n" "X-Content-Type-Options: nosniff\r\n" "Referrer-Policy: no-referrer\r\n" "Connection: %s\r\n", status, status_reason(status), content_type, (keep_alive == 1) ? "keep-alive" : "close") != 0) { return -1; }

    if ((status != 204) && (status != 304) && (header_append(header, cap, &at, "Content-Length: %zu\r\n", blen) != 0)) { return -1; }

    int n = response_write(header + at, cap - at);
    if (n < 0) { return -1; }
    at += (size_t)n;

    if ((response_allow[0] != '\0') && (header_append(header, cap, &at, "Allow: %s\r\n", response_allow) != 0)) { return -1; }

    if (write_cors_headers(header, cap, &at, status) != 0) { return -1; }

    if (at + 2u >= cap) { return -1; }
    memcpy(header + at, "\r\n", 2u);
    at += 2u;

    if (send_header_body(sock, header, at, body, response_head ? 0u : blen) != 0) { return -1; }

    return (keep_alive == 1) ? 0 : -1;
}
