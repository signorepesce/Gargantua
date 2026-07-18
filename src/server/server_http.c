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

