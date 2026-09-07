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

_Thread_local char g_wire[FETCH_MAX_RESPONSE];
#ifdef GARGANTUA_TLS
SSL_CTX *g_tls_context;
#endif

Response failure(int code, str message)
{
    Response response = { code, "", 0u, message };

    return response;
}

static int deadline_left(const struct timespec *deadline)
{
    assert(deadline != NULL);

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { return -1; }

    long left = ((deadline->tv_sec - now.tv_sec) * 1000L) +
                ((deadline->tv_nsec - now.tv_nsec) / 1000000L);

    return (left <= 0L) ? -1 : (int)((left > 60000L) ? 60000L : left);
}

int host_allowed(const char *host)
{
    assert(host != NULL);

    const char *allow = config_str("fetch.allow", "");
    if ((allow == NULL) || (allow[0] == '\0')) { return 1; }

    size_t host_len = strlen(host);
    const char *cursor = allow;

    while (*cursor != '\0')
    {
        while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == ',')) { cursor++; }

        const char *end = cursor;
        while ((*end != '\0') && (*end != ',')) { end++; }

        size_t n = (size_t)(end - cursor);
        while ((n > 0u) && ((cursor[n - 1u] == ' ') || (cursor[n - 1u] == '\t'))) { n--; }

        if ((n == host_len) && (strncasecmp(cursor, host, n) == 0)) { return 1; }

        cursor = (*end == '\0') ? end : (end + 1);
    }

    return 0;
}

int header_value_safe(const char *value)
{
    assert(value != NULL);

    size_t n     = strlen(value);
    int    slash = 0;

    if ((n == 0u) || (n > 128u)) { return 0; }

    for (size_t i = 0u; i < n; i++)
    {
        unsigned char c = (unsigned char)value[i];
        if ((c < 0x20u) || (c > 0x7Eu) || (c == (unsigned char)'"')) { return 0; }
        if (c == (unsigned char)'/') { slash++; }
    }

    return (slash == 1) ? 1 : 0;
}

static int url_char_safe(unsigned char c)
{
    return ((c > 0x20u) && (c < 0x7Fu)) ? 1 : 0;
}

static int take_authority(const char *rest, const char *end, Target *target)
{
    assert(rest != NULL);
    assert(target != NULL);

    const char *colon    = memchr(rest, ':', (size_t)(end - rest));
    size_t      host_len = (colon != NULL) ? (size_t)(colon - rest)
                                           : (size_t)(end - rest);

    if ((host_len == 0u) || (host_len >= sizeof(target->host))) { return -1; }
    memcpy(target->host, rest, host_len);
    target->host[host_len] = '\0';

    for (size_t i = 0u; i < host_len; i++)
    {
        char c  = target->host[i];
        int  ok = (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-'));
        if (ok == 0) { return -1; }
    }

    if (colon == NULL) { return 0; }

    size_t port_len = (size_t)(end - colon - 1);
    if ((port_len == 0u) || (port_len >= sizeof(target->port))) { return -1; }
    memcpy(target->port, colon + 1, port_len);
    target->port[port_len] = '\0';

    for (size_t i = 0u; i < port_len; i++)
    {
        if ((target->port[i] < '0') || (target->port[i] > '9')) { return -1; }
    }

    return 0;
}

int parse_url(const char *url, Target *target)
{
    assert(url != NULL);
    assert(target != NULL);

    memset(target, 0, sizeof(*target));

    size_t len = strlen(url);
    if ((len == 0u) || (len >= (size_t)FETCH_MAX_URL)) { return -1; }
    for (size_t i = 0u; i < len; i++)
    {
        if (url_char_safe((unsigned char)url[i]) == 0) { return -1; }
    }

    const char *rest = NULL;
    if (strncmp(url, "http://", 7u) == 0)
    {
        target->secure = 0;
        rest = url + 7;
        (void)snprintf(target->port, sizeof(target->port), "80");
    }
    else if (strncmp(url, "https://", 8u) == 0)
    {
        target->secure = 1;
        rest = url + 8;
        (void)snprintf(target->port, sizeof(target->port), "443");
    }
    else
    {
        return -1;
    }

    const char *slash = strchr(rest, '/');
    const char *authority_end = (slash != NULL) ? slash : (rest + strlen(rest));

    if (memchr(rest, '@', (size_t)(authority_end - rest)) != NULL) { return -1; }

    if (take_authority(rest, authority_end, target) != 0) { return -1; }

    const char *path = (slash != NULL) ? slash : "/";
    if (strlen(path) >= sizeof(target->path)) { return -1; }
    (void)snprintf(target->path, sizeof(target->path), "%s", path);

    return 0;
}

static int wait_ready(int fd, short events, const struct timespec *deadline)
{
    for (int guard = 0; guard < 1000; guard++)
    {
        int left = deadline_left(deadline);
        if (left < 0) { return -1; }

        struct pollfd entry = { .fd = fd, .events = events, .revents = 0 };
        int ready = poll(&entry, 1u, left);

        if ((ready < 0) && (errno == EINTR)) { continue; }
        if (ready <= 0) { return -1; }
        if ((entry.revents & (short)(POLLERR | POLLNVAL)) != 0) { return -1; }

        return 0;
    }

    return -1;
}

static int address_allowed(const struct addrinfo *entry)
{
    assert(entry != NULL);

    if (strcmp(config_str("fetch.link_local", "deny"), "allow") == 0) { return 1; }

    if (entry->ai_family == AF_INET)
    {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)(const void *)
                                       entry->ai_addr;
        uint32_t host = ntohl(v4->sin_addr.s_addr);

        if (((host & 0xFFFF0000u) == 0xA9FE0000u) || ((host & 0xFF000000u) == 0x00000000u)) { return 0; }
    }
    else if (entry->ai_family == AF_INET6)
    {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)(const void *)
                                        entry->ai_addr;
        const unsigned char *bytes = v6->sin6_addr.s6_addr;

        if ((bytes[0] == 0xFEu) && ((bytes[1] & 0xC0u) == 0x80u)) { return 0; }
    }

    return 1;
}

int connect_target(const Target *target, const struct timespec *deadline)
{
    assert(target != NULL);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV;

    struct addrinfo *list = NULL;
    if (getaddrinfo(target->host, target->port, &hints, &list) != 0) { return FETCH_NO_HOST; }

    int fd      = -1;
    int guard   = 0;
    int blocked = 0;

    for (struct addrinfo *entry = list; (entry != NULL) && (guard < 8); entry = entry->ai_next, guard++)
    {
        if (address_allowed(entry) == 0)
        {
            blocked = 1;
            continue;
        }

        fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) { continue; }

        int flags = fcntl(fd, F_GETFL, 0);
        if ((flags < 0) || (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0))
        {
            (void)close(fd);
            fd = -1;
            continue;
        }

        int rc = connect(fd, entry->ai_addr, entry->ai_addrlen);
        if ((rc != 0) && (errno == EINPROGRESS))
        {
            if (wait_ready(fd, POLLOUT, deadline) == 0)
            {
                int       error = 0;
                socklen_t size  = sizeof(error);
                if ((getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0) && (error == 0)) { rc = 0; }
            }
        }

        if (rc == 0) { break; }

        (void)close(fd);
        fd = -1;
    }

    freeaddrinfo(list);

    if (fd < 0) { return (blocked != 0) ? FETCH_BLOCKED : FETCH_NO_CONNECT; }

    return fd;
}

#ifdef GARGANTUA_TLS
int tls_open(Channel *channel, const Target *target, const struct timespec *deadline)
{
    assert(channel != NULL);
    assert(target != NULL);

    if (g_tls_context == NULL) { return FETCH_NO_TLS; }

    channel->ssl = SSL_new(g_tls_context);
    if (channel->ssl == NULL) { return FETCH_TLS_FAILED; }

    char host[FETCH_MAX_HOST];
    (void)snprintf(host, sizeof(host), "%s", target->host);

    if ((SSL_set_fd(channel->ssl, channel->fd) != 1) || (SSL_set_tlsext_host_name(channel->ssl, host) != 1) || (SSL_set1_host(channel->ssl, host) != 1)) { return FETCH_TLS_FAILED; }

    SSL_set_hostflags(channel->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    SSL_set_verify(channel->ssl, SSL_VERIFY_PEER, NULL);

    for (int guard = 0; guard < 64; guard++)
    {
        int rc = SSL_connect(channel->ssl);
        if (rc == 1) { return 0; }

        int reason = SSL_get_error(channel->ssl, rc);
        short events = (reason == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN;

        if ((reason != SSL_ERROR_WANT_READ) && (reason != SSL_ERROR_WANT_WRITE)) { return FETCH_TLS_FAILED; }
        if (wait_ready(channel->fd, events, deadline) != 0) { return FETCH_TIMEOUT; }
    }

    return FETCH_TLS_FAILED;
}
#endif

void channel_close(Channel *channel)
{
    assert(channel != NULL);

#ifdef GARGANTUA_TLS
    if (channel->ssl != NULL)
    {
        (void)SSL_shutdown(channel->ssl);
        SSL_free(channel->ssl);
        channel->ssl = NULL;
    }
#endif

    if (channel->fd >= 0)
    {
        (void)close(channel->fd);
        channel->fd = -1;
    }
}

ssize_t channel_write(Channel *channel, const char *data, size_t len, const struct timespec *deadline)
{
    assert(channel != NULL);

#ifdef GARGANTUA_TLS
    if (channel->ssl != NULL)
    {
        int rc = SSL_write(channel->ssl, data, (int)len);
        if (rc > 0) { return (ssize_t)rc; }
        int reason = SSL_get_error(channel->ssl, rc);
        if ((reason != SSL_ERROR_WANT_READ) && (reason != SSL_ERROR_WANT_WRITE)) { return -1; }
        return (wait_ready(channel->fd, (reason == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT, deadline) == 0) ? 0 : -1;
    }
#endif

    if (wait_ready(channel->fd, POLLOUT, deadline) != 0) { return -1; }

    ssize_t sent = send(channel->fd, data, len, 0);
    if ((sent < 0) && (errno == EINTR)) { return 0; }

    return sent;
}

ssize_t channel_read(Channel *channel, char *out, size_t cap, const struct timespec *deadline)
{
    assert(channel != NULL);

#ifdef GARGANTUA_TLS
    if (channel->ssl != NULL)
    {
        int rc = SSL_read(channel->ssl, out, (int)cap);
        if (rc > 0) { return (ssize_t)rc; }
        int reason = SSL_get_error(channel->ssl, rc);
        if ((reason == SSL_ERROR_ZERO_RETURN) || ((reason != SSL_ERROR_WANT_READ) && (reason != SSL_ERROR_WANT_WRITE))) { return 0; }
        return (wait_ready(channel->fd, (reason == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN, deadline) == 0) ? -2 : -1;
    }
#endif

    if (wait_ready(channel->fd, POLLIN, deadline) != 0) { return -1; }

    ssize_t got = recv(channel->fd, out, cap, 0);
    if ((got < 0) && (errno == EINTR)) { return -2; }

    return got;
}
