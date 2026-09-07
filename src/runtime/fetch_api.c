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

static Response take_reply(size_t wire_len)
{
    int    status  = 0;
    char  *payload = NULL;
    size_t length  = 0u;

    int rc = parse_reply(g_wire, wire_len, &status, &payload, &length);
    if (rc != 0) { return failure(rc, "invalid response"); }

    char *copy = request_alloc(length + 1u);
    if (copy == NULL) { return failure(FETCH_TOO_LARGE, "did not fit in the arena"); }

    memcpy(copy, payload, length);
    copy[length] = '\0';

    Response response = { status, copy, length, "" };

    return response;
}

static Response perform(const char *method, str url, str content_type, str body, int timeout_ms)
{
    assert(method != NULL);

    Target target;
    int    rejected = resolve_request(url, &target);
    if (rejected != 0) { return failure(rejected, "the request was rejected"); }

    if ((content_type != NULL) && (header_value_safe(content_type) == 0)) { return failure(FETCH_BAD_HEADER, "invalid content type"); }

    size_t body_len = (body != NULL) ? strlen(body) : 0u;
    if (body_len > (size_t)FETCH_MAX_BODY) { return failure(FETCH_TOO_LARGE, "body too large"); }

    struct timespec deadline;
    if (make_deadline(timeout_ms, &deadline) != 0) { return failure(FETCH_TIMEOUT, "clock"); }

    Channel channel;
    int opened = channel_open(&channel, &target, &deadline);
    if (opened != 0) { return failure(opened, "connection failed"); }

    char request[FETCH_MAX_HEADER];
    int  n = build_request(request, sizeof(request), method, &target, content_type, body_len);
    if (n < 0)
    {
        channel_close(&channel);
        return failure(FETCH_BAD_URL, "header too large");
    }

    if ((fetch_send_all(&channel, request, (size_t)n, &deadline) != 0) || ((body_len > 0u) && (fetch_send_all(&channel, body, body_len, &deadline) != 0)))
    {
        channel_close(&channel);
        return failure(FETCH_TIMEOUT, "send failed");
    }

    size_t wire_len = 0u;
    int    rc       = read_reply(&channel, &wire_len, &deadline);
    channel_close(&channel);

    if (rc != 0) { return failure(rc, "read failed"); }

    return take_reply(wire_len);
}

Response fetch_get(str url, int timeout_ms)
{
    return perform("GET", url, NULL, NULL, timeout_ms);
}

Response fetch_post(str url, str content_type, str body, int timeout_ms)
{
    return perform("POST", url, (content_type != NULL) ? content_type : "application/json", (body != NULL) ? body : "", timeout_ms);
}

str fetch_json_text(Response response, str key)
{
    if ((response.body == NULL) || (response.length == 0u) || (key == NULL)) { return ""; }

    JsonToken tokens[JSON_MAX_TOKENS];
    int count = json_parse(response.body, response.length, tokens, JSON_MAX_TOKENS);
    if (count <= 0) { return ""; }

    int slot = json_object_get(response.body, tokens, count, 0, key);
    if (slot < 0) { return ""; }

    int raw_len = tokens[slot].end - tokens[slot].start;
    if ((raw_len < 0) || (raw_len >= FETCH_MAX_FIELD)) { return ""; }

    static _Thread_local char value[FETCH_MAX_FIELD];
    json_copy_str(response.body, &tokens[slot], value, sizeof(value));

    str copy = arena_intern(value);

    return (copy != NULL) ? copy : "";
}

int fetch_json_int(Response response, str key, int fallback)
{
    if ((response.body == NULL) || (response.length == 0u) || (key == NULL)) { return fallback; }

    JsonToken tokens[JSON_MAX_TOKENS];
    int count = json_parse(response.body, response.length, tokens, JSON_MAX_TOKENS);
    if (count <= 0) { return fallback; }

    int slot = json_object_get(response.body, tokens, count, 0, key);
    if (slot < 0) { return fallback; }

    long value = 0L;
    if (json_token_long_checked(response.body, &tokens[slot], &value) != 0) { return fallback; }
    if ((value < -2147483647L) || (value > 2147483647L)) { return fallback; }

    return (int)value;
}

int fetch_tls_available(void)
{
#ifdef GARGANTUA_TLS
    return 1;
#else
    return 0;
#endif
}

int fetch_init(void)
{
#ifdef GARGANTUA_TLS
    if (g_tls_context != NULL) { return 0; }

    g_tls_context = SSL_CTX_new(TLS_client_method());
    if (g_tls_context == NULL)
    {
        (void)fprintf(stderr, "gargantua: the TLS context was not set up\n");
        return -1;
    }

    if ((SSL_CTX_set_min_proto_version(g_tls_context, TLS1_2_VERSION) != 1) || (SSL_CTX_set_default_verify_paths(g_tls_context) != 1))
    {
        (void)fprintf(stderr, "gargantua: no CA certificates found\n");
        SSL_CTX_free(g_tls_context);
        g_tls_context = NULL;
        return -1;
    }

    SSL_CTX_set_verify(g_tls_context, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_mode(g_tls_context, SSL_MODE_AUTO_RETRY);
#endif

    return 0;
}

void fetch_shutdown(void)
{
#ifdef GARGANTUA_TLS
    if (g_tls_context != NULL)
    {
        SSL_CTX_free(g_tls_context);
        g_tls_context = NULL;
    }
#endif
}
