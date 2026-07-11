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

