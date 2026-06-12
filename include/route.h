#ifndef ROUTE_H
#define ROUTE_H

#define ROUTE_MAX_URL 128

#include "gargantua.h"
#include <stddef.h>

typedef int (*RouteHandler)(const RequestParams *params, char *body, size_t body_len, char *out, size_t cap);

typedef struct
{
    const char *method;
    const char *url;
    const char *content_type;
    int         status;
    RouteHandler     handler;
} Route;

const Route *route_table(void);
int            route_count(void);

const Route *route_find(const char *method, const char *url, RequestParams *out);

#define ROUTE_ALLOW_CAP 2048

int route_allow(const char *url, char *out, size_t cap);

#endif
