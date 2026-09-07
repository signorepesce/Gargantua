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

static void handle_connection(Worker *w, int sock)
{
    assert(w != NULL);
    assert(sock >= 0);

    for (int served = 0; served < SERVER_KEEPALIVE_MAX; served++)
    {
        if (g_drain_over != 0) { return; }

        HttpRequest req = {0};
        reset_response_state();

        int rc = read_request(sock, &w->req, &req);
        response_head = (strcmp(req.method, "HEAD") == 0) ||
                        ((w->req.len >= 5u) && (memcmp(w->req.data, "HEAD ", 5u) == 0));
        if (rc != 1)
        {
            send_read_error(sock, rc);
            return;
        }

        cors_prepare(&req);
        auth_bind(&req);

        if (client_limit(w->ip, 0) != 0)
        {
            (void)response_header("Retry-After", "60");
            (void)send_response(sock, 429, JSON_CONTENT_TYPE, "rate limit exceeded", 0);
            return;
        }

        struct timeval start;
        (void)gettimeofday(&start, NULL);

        char  path[HTTP_MAX_PATH];
        char *query = NULL;
        (void)snprintf(path, sizeof(path), "%s", req.path);
        if (split_path(path, &query) != 0)
        {
            (void)send_response(sock, 400, JSON_CONTENT_TYPE, "bad url", 0);
            return;
        }

        size_t body_len = (req.content_length > 0)
                              ? (size_t)req.content_length : req.body_len;
        int status = 0;
        int cont = serve_request(w, sock, &req, path, query, (char *)(size_t)req.body, body_len, keep_alive_wanted(&req, served), &status);

        log_request(&start, req.method, req.path, status);
        finish_request(w);

        if ((cont != 0) || (g_drain_over != 0)) { return; }
        if (consume_message(w, &req) != 0) { return; }
    }
}

static void *worker_main(void *arg)
{
    Worker *w = arg;
    assert(w != NULL);
    assert(w->arena.max != 0u);

    for (;;)
    {
        int fd = queue_pop(&w->ip);
        if (fd < 0) { break; }

        handle_connection(w, fd);
        (void)close(fd);
        (void)client_limit(w->ip, -1);
        (void)arena_reset(&w->arena);
        dynbuf_release(&w->req);
    }

    return NULL;
}

static void reject_busy(int fd)
{
    const char body[] = "{\"status\":503,\"code\":\"unavailable\",\"error\":\"server busy\",\"fields\":[],\"truncated\":false}";
    char response[512];
    int n = snprintf(response, sizeof(response), "HTTP/1.1 503 Service Unavailable\r\n" "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", sizeof(body) - 1u, body);
    if (n > 0 && (size_t)n < sizeof(response)) { (void)send(fd, response, (size_t)n, 0); }
    (void)close(fd);
}

static int g_worker_count = SERVER_WORKERS;
static void stop_workers(void);

static int start_workers(void)
{
    assert(SERVER_WORKERS > 0);
    assert(SERVER_ARENA > 0);

    g_worker_count = config_int("server.workers", SERVER_WORKERS);
    if ((g_worker_count < 1) || (g_worker_count > SERVER_WORKERS)) { g_worker_count = SERVER_WORKERS; }

    memset(&g_queue, 0, sizeof(g_queue));
    g_workers_started = 0;
    if (pthread_mutex_init(&g_queue.lock, NULL) != 0) { return -1; }
    if (pthread_cond_init(&g_queue.not_empty, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&g_queue.lock);
        return -1;
    }

    for (int i = 0; i < g_worker_count; i++)
    {
        if (dynbuf_init(&g_workers[i].req, (size_t)SERVER_REQ_MAX) != 0) { return -1; }
        if (arena_init(&g_workers[i].arena, (size_t)SERVER_ARENA) != 0)
        {
            (void)fprintf(stderr, "gargantua: the arena was not initialised\n");
            stop_workers();
            return -1;
        }
    }

    for (int i = 0; i < g_worker_count; i++)
    {
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) { return -1; }
        (void)pthread_attr_setstacksize(&attr, (size_t)SERVER_STACK);
        int made = pthread_create(&g_worker_threads[i], &attr, worker_main, &g_workers[i]);
        (void)pthread_attr_destroy(&attr);
        if (made != 0)
        {
            (void)fprintf(stderr, "gargantua: the thread did not start\n");
            stop_workers();
            return -1;
        }
        g_workers_started++;
    }

    return 0;
}

static void stop_workers(void)
{
    (void)pthread_mutex_lock(&g_queue.lock);
    g_queue.stopping = 1;
    while (g_queue.count > 0)
    {
        int queued = g_queue.fds[g_queue.head];
        (void)client_limit(g_queue.ips[g_queue.head], -1);
        g_queue.head = (g_queue.head + 1) % SERVER_QUEUE;
        g_queue.count--;
        (void)close(queued);
    }
    (void)pthread_cond_broadcast(&g_queue.not_empty);
    (void)pthread_mutex_unlock(&g_queue.lock);

    for (int i = 0; i < g_workers_started; i++) { (void)pthread_join(g_worker_threads[i], NULL); }
    g_workers_started = 0;
    (void)pthread_cond_destroy(&g_queue.not_empty);
    (void)pthread_mutex_destroy(&g_queue.lock);
}

static int bind_listen(int port)
{
    assert(port > 0);
    assert(port < 65536);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    int opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);

    const char *address = config_str("server.address", "127.0.0.1");
    if (inet_pton(AF_INET, address, &addr.sin_addr) != 1)
    {
        (void)fprintf(stderr, "gargantua: invalid server.address: %s\n", address);
        (void)close(fd);
        return -1;
    }

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        (void)close(fd);
        return -1;
    }
    if (listen(fd, SERVER_BACKLOG) < 0)
    {
        perror("listen");
        (void)close(fd);
        return -1;
    }

    return fd;
}

static void print_routes(int port)
{
    assert(port > 0);
    assert(port < 65536);

    server_banner();

    (void)printf("listening on http://%s:%d" "  (%d workers, %d routes)\n", config_str("server.address", "127.0.0.1"), port, g_worker_count, route_count());

    const Route *table = route_table();
    int            n     = route_count();

    for (int i = 0; (i < n) && (i < SERVER_QUEUE); i++) { (void)printf("  %-6s %-22s -> %d\n", table[i].method, table[i].url, table[i].status); }
    (void)printf("\n");
    (void)fflush(stdout);
}

static int install_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;

    if ((sigaction(SIGINT, &action, NULL) != 0) || (sigaction(SIGTERM, &action, NULL) != 0)) { return -1; }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) { return -1; }

    return 0;
}

static int load_client_limits(void)
{
    g_connections_per_ip  = config_int("server.connections_per_ip", 16);
    g_requests_per_minute = config_int("server.requests_per_minute", 600);

    if ((g_connections_per_ip < 1) || (g_connections_per_ip > 256) || (g_requests_per_minute < 1) || (g_requests_per_minute > 1000000)) { return -1; }

    return 0;
}

static int set_nonblocking(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) { return -1; }

    int wanted = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);

    return (fcntl(fd, F_SETFL, wanted) != 0) ? -1 : 0;
}

static int accept_one(int fd)
{
    struct sockaddr_in peer;
    socklen_t          peer_len = sizeof(peer);

    int client = accept(fd, (struct sockaddr *)&peer, &peer_len);
    if (client < 0)
    {
        return ((errno == EINTR) || (errno == EAGAIN) || (errno == EWOULDBLOCK))
                   ? 0 : -1;
    }

    if (set_nonblocking(client, 0) != 0)
    {
        (void)close(client);
        return 0;
    }

    uint32_t ip = peer.sin_addr.s_addr;
    if (client_limit(ip, 1) != 0)
    {
        reject_busy(client);
        return 0;
    }
    if (queue_push(client, ip) != 0)
    {
        (void)client_limit(ip, -1);
        reject_busy(client);
    }

    return 0;
}

static int drain_expired(const struct timespec *started)
{
    assert(started != NULL);

    if (g_drain_ms <= 0) { return 1; }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { return 1; }

    long elapsed = ((now.tv_sec - started->tv_sec) * 1000L) +
                   ((now.tv_nsec - started->tv_nsec) / 1000000L);

    return (elapsed >= (long)g_drain_ms) ? 1 : 0;
}

static void accept_loop(int fd)
{
    struct timespec drain_started = {0, 0};
    int             draining      = 0;

    for (;;)
    {
        if (g_stop != 0)
        {
            if (draining == 0)
            {
                draining = 1;
                if (clock_gettime(CLOCK_MONOTONIC, &drain_started) != 0) { break; }
                if (g_drain_ms > 0)
                {
                    (void)printf("shutting down: drain %d ms\n", g_drain_ms);
                    (void)fflush(stdout);
                }
            }
            if (drain_expired(&drain_started) != 0) { break; }
        }

        struct pollfd listener = { .fd = fd, .events = POLLIN, .revents = 0 };

        int available = poll(&listener, 1u, 100);
        if (available < 0)
        {
            if (errno == EINTR) { continue; }
            break;
        }
        if ((available == 0) || ((listener.revents & POLLIN) == 0)) { continue; }
        if (accept_one(fd) != 0) { break; }
    }

    g_drain_over = 1;
}

int server_run(int port)
{
    assert(port > 0);
    assert(port < 65536);

    g_stop       = 0;
    g_drain_over = 0;
    memset(g_clients, 0, sizeof(g_clients));

    g_drain_ms = config_int("server.drain_ms", 0);
    if ((g_drain_ms < 0) || (g_drain_ms > 120000)) { return -1; }

    if ((load_client_limits() != 0) || (install_signal_handlers() != 0) || (static_files_init() != 0)) { return -1; }
    if (start_workers() != 0) { return -1; }

    int fd = bind_listen(port);
    if (fd < 0)
    {
        stop_workers();
        return -1;
    }
    if (set_nonblocking(fd, 1) != 0)
    {
        (void)close(fd);
        stop_workers();
        return -1;
    }

    print_routes(port);
    accept_loop(fd);

    (void)close(fd);
    stop_workers();
    (void)printf("shutdown complete.\n");

    return 0;
}
