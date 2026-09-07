#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "fetch.h"
#include "config.h"
#include "json.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef GARGANTUA_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

typedef struct
{
    int  secure;
    char host[FETCH_MAX_HOST];
    char port[8];
    char path[FETCH_MAX_PATH];
} Target;

typedef struct
{
    int fd;
#ifdef GARGANTUA_TLS
    SSL *ssl;
#endif
} Channel;
#include "fetch_internal.h"

int fetch_send_all(Channel *channel, const char *data, size_t len, const struct timespec *deadline)
{
    size_t sent  = 0u;
    int    guard = 0;

    while ((sent < len) && (guard < 4096))
    {
        ssize_t step = channel_write(channel, data + sent, len - sent, deadline);
        if (step < 0) { return -1; }
        sent += (size_t)step;
        guard++;
    }

    return (sent == len) ? 0 : -1;
}

int read_reply(Channel *channel, size_t *len, const struct timespec *deadline)
{
    assert(len != NULL);

    *len = 0u;

    for (int guard = 0; guard < FETCH_READ_STEPS; guard++)
    {
        if (*len + 1u >= sizeof(g_wire)) { return FETCH_TOO_LARGE; }

        ssize_t got = channel_read(channel, g_wire + *len, sizeof(g_wire) - *len - 1u, deadline);
        if (got == -2) { continue; }
        if (got < 0) { return FETCH_TIMEOUT; }
        if (got == 0) { return 0; }

        *len += (size_t)got;
        g_wire[*len] = '\0';
    }

    return FETCH_TIMEOUT;
}

static int hex_value(char c)
{
    if ((c >= '0') && (c <= '9')) { return c - '0'; }
    if ((c >= 'a') && (c <= 'f')) { return (c - 'a') + 10; }
    if ((c >= 'A') && (c <= 'F')) { return (c - 'A') + 10; }

    return -1;
}

static int decode_chunked(char *body, size_t available, size_t *out_len)
{
    assert(body != NULL);
    assert(out_len != NULL);

    size_t read  = 0u;
    size_t write = 0u;

    for (int guard = 0; guard < 4096; guard++)
    {
        size_t size   = 0u;
        int    digits = 0;

        while ((read < available) && (hex_value(body[read]) >= 0))
        {
            if (size > ((size_t)FETCH_MAX_BODY / 16u)) { return FETCH_TOO_LARGE; }
            size = (size * 16u) + (size_t)hex_value(body[read]);
            digits++;
            read++;
        }
        if ((digits == 0) || (digits > 8)) { return FETCH_BAD_REPLY; }

        while ((read < available) && (body[read] != '\n')) { read++; }
        if (read >= available) { return FETCH_BAD_REPLY; }
        read++;

        if (size == 0u)
        {
            *out_len = write;
            return 0;
        }

        if ((size > (available - read)) || ((write + size) > (size_t)FETCH_MAX_BODY))
        {
            return ((write + size) > (size_t)FETCH_MAX_BODY)
                       ? FETCH_TOO_LARGE : FETCH_BAD_REPLY;
        }

        memmove(body + write, body + read, size);
        write += size;
        read  += size;

        if (((read + 1u) >= available) || (body[read] != '\r') || (body[read + 1u] != '\n'))
        {
            if ((read < available) && (body[read] == '\n'))
            {
                read++;
                continue;
            }
            return FETCH_BAD_REPLY;
        }
        read += 2u;
    }

    return FETCH_BAD_REPLY;
}

static int parse_status_line(const char *wire, size_t len, size_t *at)
{
    assert(wire != NULL);
    assert(at != NULL);

    if ((len < 14u) || (strncmp(wire, "HTTP/1.", 7u) != 0) || ((wire[7] != '0') && (wire[7] != '1')) || (wire[8] != ' ')) { return -1; }

    int status = 0;
    for (size_t i = 9u; i < 12u; i++)
    {
        if ((wire[i] < '0') || (wire[i] > '9')) { return -1; }
        status = (status * 10) + (wire[i] - '0');
    }
    if ((status < 100) || (status > 599)) { return -1; }

    const char *eol = memchr(wire, '\n', len);
    if (eol == NULL) { return -1; }

    *at = (size_t)(eol - wire) + 1u;

    return status;
}

static int header_int(const char *line, size_t len, long *out)
{
    assert(line != NULL);
    assert(out != NULL);

    size_t i = 0u;
    while ((i < len) && ((line[i] == ' ') || (line[i] == '\t'))) { i++; }
    if (i >= len) { return -1; }

    long value = 0L;
    size_t digits = 0u;
    while ((i < len) && (line[i] >= '0') && (line[i] <= '9'))
    {
        if (value > ((long)FETCH_MAX_BODY + 1L)) { return -1; }
        value = (value * 10L) + (long)(line[i] - '0');
        digits++;
        i++;
    }

    if ((digits == 0u) || (digits > 9u)) { return -1; }

    *out = value;

    return 0;
}

static int take_header(const char *line, size_t line_len, long *declared, int *chunked)
{
    assert(line != NULL);
    assert(declared != NULL);
    assert(chunked != NULL);

    const char *colon = memchr(line, ':', line_len);
    if (colon == NULL) { return 0; }

    size_t name_len  = (size_t)(colon - line);
    size_t value_len = line_len - name_len - 1u;

    if ((name_len == 14u) && (strncasecmp(line, "Content-Length", 14u) == 0))
    {
        if ((*declared >= 0L) || (header_int(colon + 1, value_len, declared) != 0)) { return -1; }
        return 0;
    }

    if ((name_len == 17u) && (strncasecmp(line, "Transfer-Encoding", 17u) == 0))
    {
        if ((value_len < 7u) || (strncasecmp(colon + 1 + (value_len - 7u), "chunked", 7u) != 0)) { return -1; }
        *chunked = 1;
    }

    return 0;
}

static int take_body(char *body, size_t available, long declared, int chunked, size_t *body_len)
{
    assert(body != NULL);
    assert(body_len != NULL);

    if (chunked != 0)
    {
        if (declared >= 0L) { return FETCH_BAD_REPLY; }
        return decode_chunked(body, available, body_len);
    }

    size_t length = (declared >= 0L) ? (size_t)declared : available;
    if (length > available) { return FETCH_BAD_REPLY; }
    if (length > (size_t)FETCH_MAX_BODY) { return FETCH_TOO_LARGE; }

    *body_len = length;

    return 0;
}

int parse_reply(char *wire, size_t len, int *status_out, char **body_out, size_t *body_len)
{
    assert(status_out != NULL);
    assert(body_out != NULL);
    assert(body_len != NULL);

    size_t at     = 0u;
    int    status = parse_status_line(wire, len, &at);
    if (status < 0) { return FETCH_BAD_REPLY; }

    long declared = -1L;
    int  chunked  = 0;

    for (int guard = 0; guard < 128; guard++)
    {
        const char *eol = memchr(wire + at, '\n', len - at);
        if (eol == NULL) { return FETCH_BAD_REPLY; }

        size_t line_len = (size_t)(eol - (wire + at));
        if ((line_len > 0u) && (wire[at + line_len - 1u] == '\r')) { line_len--; }

        if (line_len == 0u)
        {
            at = (size_t)(eol - wire) + 1u;

            int rc = take_body(wire + at, len - at, declared, chunked, body_len);
            if (rc != 0) { return rc; }

            *status_out = status;
            *body_out   = wire + at;

            return 0;
        }

        if (take_header(wire + at, line_len, &declared, &chunked) != 0) { return FETCH_BAD_REPLY; }

        at = (size_t)(eol - wire) + 1u;
    }

    return FETCH_BAD_REPLY;
}

int channel_open(Channel *channel, const Target *target, const struct timespec *deadline)
{
    assert(channel != NULL);
    assert(target != NULL);

    memset(channel, 0, sizeof(*channel));
    channel->fd = -1;

    int fd = connect_target(target, deadline);
    if (fd < 0) { return fd; }
    channel->fd = fd;

#ifdef GARGANTUA_TLS
    if (target->secure != 0)
    {
        int rc = tls_open(channel, target, deadline);
        if (rc != 0)
        {
            channel_close(channel);
            return rc;
        }
    }
#else
    (void)deadline;
#endif

    return 0;
}

int make_deadline(int timeout_ms, struct timespec *deadline)
{
    assert(deadline != NULL);

    int budget = ((timeout_ms > 0) && (timeout_ms <= 60000)) ? timeout_ms
                                                             : FETCH_TIMEOUT_MS;

    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) { return -1; }

    deadline->tv_sec  += (time_t)(budget / 1000);
    deadline->tv_nsec += (long)((budget % 1000) * 1000000L);
    if (deadline->tv_nsec >= 1000000000L)
    {
        deadline->tv_sec  += 1;
        deadline->tv_nsec -= 1000000000L;
    }

    return 0;
}

int build_request(char *out, size_t cap, const char *method, const Target *target, const char *content_type, size_t body_len)
{
    assert(out != NULL);
    assert(method != NULL);
    assert(target != NULL);

    int n = snprintf(out, cap, "%s %s HTTP/1.1\r\n" "Host: %s\r\n" "User-Agent: gargantua\r\n" "Accept: */*\r\n" "Connection: close\r\n" "%s%s%s" "Content-Length: %zu\r\n" "\r\n", method, target->path, target->host, (content_type != NULL) ? "Content-Type: " : "", (content_type != NULL) ? content_type : "", (content_type != NULL) ? "\r\n" : "", body_len);

    return ((n <= 0) || ((size_t)n >= cap)) ? -1 : n;
}

int resolve_request(str url, Target *target)
{
    assert(target != NULL);

    if (url == NULL) { return FETCH_BAD_URL; }
    if (parse_url(url, target) != 0) { return FETCH_BAD_URL; }
    if (host_allowed(target->host) == 0) { return FETCH_BLOCKED; }

#ifndef GARGANTUA_TLS
    if (target->secure != 0) { return FETCH_NO_TLS; }
#endif

    return 0;
}
