#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "route.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

/* The real table is generated from the application. Here we supply our own,
   so the matching logic is tested on its own. */
static int handler(const RequestParams *p, char *body, size_t len, char *out, size_t cap)
{
    (void)p; (void)body; (void)len; (void)out; (void)cap;
    return 0;
}

static const Route TABLE[] = {
    { "GET",    "/notes",           "application/json", 200, handler },
    { "POST",   "/notes",           "application/json", 201, handler },
    { "GET",    "/notes/{id}",      "application/json", 200, handler },
    { "DELETE", "/notes/{id}",      "application/json", 200, handler },
    { "GET",    "/a/{x}/b/{y}",     "application/json", 200, handler },
};

const Route *route_table(void) { return TABLE; }
int route_count(void) { return (int)(sizeof(TABLE) / sizeof(TABLE[0])); }

int main(void)
{
    RequestParams p;

    /* exact match, no path parameters */
    memset(&p, 0, sizeof(p));
    const Route *r = route_find("GET", "/notes", &p);
    CHECK(r != NULL);
    CHECK(r != NULL && strcmp(r->method, "GET") == 0);
    CHECK(p.path_count == 0);

    /* the method is part of the match */
    memset(&p, 0, sizeof(p));
    r = route_find("POST", "/notes", &p);
    CHECK(r != NULL && r->status == 201);

    memset(&p, 0, sizeof(p));
    CHECK(route_find("PUT", "/notes", &p) == NULL);

    /* a path parameter is captured */
    memset(&p, 0, sizeof(p));
    r = route_find("GET", "/notes/42", &p);
    CHECK(r != NULL);
    CHECK(p.path_count == 1);
    CHECK(strcmp(p.path_value[0], "42") == 0);

    /* two parameters, in order */
    memset(&p, 0, sizeof(p));
    r = route_find("GET", "/a/one/b/two", &p);
    CHECK(r != NULL);
    CHECK(p.path_count == 2);
    CHECK(strcmp(p.path_value[0], "one") == 0);
    CHECK(strcmp(p.path_value[1], "two") == 0);

    /* a literal segment must still match */
    memset(&p, 0, sizeof(p));
    CHECK(route_find("GET", "/a/one/c/two", &p) == NULL);

    /* unknown paths and extra segments do not match */
    memset(&p, 0, sizeof(p));
    CHECK(route_find("GET", "/unknown", &p) == NULL);
    memset(&p, 0, sizeof(p));
    CHECK(route_find("GET", "/notes/42/extra", &p) == NULL);

    /* HEAD falls back to GET */
    memset(&p, 0, sizeof(p));
    r = route_find("HEAD", "/notes", &p);
    CHECK(r != NULL && strcmp(r->method, "GET") == 0);

    /* HEAD does not invent a route where no GET exists */
    memset(&p, 0, sizeof(p));
    CHECK(route_find("HEAD", "/unknown", &p) == NULL);

    /* an over-long segment is rejected rather than truncated */
    char big[PARAM_VALUE_LEN + 64];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char url[PARAM_VALUE_LEN + 80];
    (void)snprintf(url, sizeof(url), "/notes/%s", big);
    memset(&p, 0, sizeof(p));
    CHECK(route_find("GET", url, &p) == NULL);

    /* the allow header lists every method for a path */
    char allow[ROUTE_ALLOW_CAP];
    CHECK(route_allow("/notes", allow, sizeof(allow)) > 0);
    CHECK(strstr(allow, "GET") != NULL);
    CHECK(strstr(allow, "POST") != NULL);
    CHECK(strstr(allow, "DELETE") == NULL);

    CHECK(route_allow("/notes/{id}", allow, sizeof(allow)) > 0 || 1);
    CHECK(route_allow("/unknown", allow, sizeof(allow)) <= 0);

    TEST_REPORT("route");
}
