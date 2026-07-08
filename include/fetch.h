#ifndef FETCH_H
#define FETCH_H

#include "gargantua.h"

#include <stddef.h>

#define FETCH_MAX_URL      512
#define FETCH_MAX_HOST     256
#define FETCH_MAX_PATH     384
#define FETCH_MAX_BODY     (64 * 1024)
#define FETCH_MAX_HEADER   (8 * 1024)
#define FETCH_MAX_RESPONSE (FETCH_MAX_BODY + FETCH_MAX_HEADER)
#define FETCH_TIMEOUT_MS   5000
#define FETCH_MAX_FIELD    1024
#define FETCH_READ_STEPS   200000

enum
{
    FETCH_BAD_URL     = -1,
    FETCH_BLOCKED     = -2,
    FETCH_NO_HOST     = -3,
    FETCH_NO_CONNECT  = -4,
    FETCH_TIMEOUT     = -5,
    FETCH_TOO_LARGE   = -6,
    FETCH_BAD_REPLY   = -7,
    FETCH_NO_TLS      = -8,
    FETCH_TLS_FAILED  = -9,
    FETCH_BAD_HEADER  = -10
};

typedef struct
{
    int    status;
    str    body;
    size_t length;
    str    error;
} Response;

int  fetch_init(void);
void fetch_shutdown(void);
int  fetch_tls_available(void);

Response fetch_get(str url, int timeout_ms);
Response fetch_post(str url, str content_type, str body, int timeout_ms);

str fetch_json_text(Response response, str key);
int fetch_json_int(Response response, str key, int fallback);

#define $fetch(url)                fetch_get((url), FETCH_TIMEOUT_MS)
#define $fetch_in(url, ms)         fetch_get((url), (ms))
#define $send(url, type, body)     fetch_post((url), (type), (body), FETCH_TIMEOUT_MS)
#define $send_json(url, body)      fetch_post((url), "application/json", (body), FETCH_TIMEOUT_MS)
#define $ok(response)              (((response).status >= 200) && ((response).status < 300))
#define $json_text(response, key)  fetch_json_text((response), (key))
#define $json_int(response, key, fallback) \
    fetch_json_int((response), (key), (fallback))

#endif
