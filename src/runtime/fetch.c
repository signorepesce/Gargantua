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

