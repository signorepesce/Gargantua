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

static void handle_connection(Worker *w, int sock)
{
    assert(w != NULL);
    assert(sock >= 0);

    size_t len = 0u;

    for (int served = 0; served < SERVER_KEEPALIVE_MAX; served++)
    {
        if (g_drain_over != 0) { return; }

        HttpRequest req = {0};
        reset_response_state();

        int rc = read_request(sock, w->req, sizeof(w->req), &len, &req);
        response_head = (strcmp(req.method, "HEAD") == 0) ||
                        ((len >= 5u) && (memcmp(w->req, "HEAD ", 5u) == 0));
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
        if (consume_message(w, &req, &len) != 0) { return; }
    }
}

static void *worker_main(void *arg)
{
    Worker *w = arg;
    assert(w != NULL);
    assert(w->arena.base != NULL);

    for (;;)
    {
        int fd = queue_pop(&w->ip);
        if (fd < 0) { break; }

        handle_connection(w, fd);
        (void)close(fd);
        (void)client_limit(w->ip, -1);
        (void)arena_reset(&w->arena);
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
        if (arena_init(&g_workers[i].arena, g_workers[i].backing, (size_t)SERVER_ARENA) != 0)
        {
            (void)fprintf(stderr, "gargantua: the arena was not initialised\n");
            stop_workers();
            return -1;
        }
    }

    for (int i = 0; i < g_worker_count; i++)
    {
        if (pthread_create(&g_worker_threads[i], NULL, worker_main, &g_workers[i]) != 0)
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

