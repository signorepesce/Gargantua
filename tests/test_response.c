#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "response.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    char wire[RESPONSE_WIRE_CAP + 1];

    response_reset();
    CHECK(response_status_get(200) == 200);
    CHECK(response_write(wire, sizeof(wire), 200) == 0);

    CHECK(response_header("X-Test", "value") == 0);
    int n = response_write(wire, sizeof(wire), 200);
    CHECK(n > 0);
    wire[n] = '\0';
    CHECK(strstr(wire, "X-Test: value") != NULL);
    CHECK(strstr(wire, "\r\n") != NULL);

    response_reset();
    CHECK(response_location("/users/1") == 0);
    CHECK(response_header("Retry-After", "5") == 0);
    n = response_write(wire, sizeof(wire), 201);
    CHECK(n > 0);
    wire[n] = '\0';
    CHECK(strstr(wire, "Location: /users/1\r\n") != NULL);
    n = response_write(wire, sizeof(wire), 409);
    CHECK(n > 0);
    wire[n] = '\0';
    CHECK(strstr(wire, "Location") == NULL);
    CHECK(strstr(wire, "Retry-After: 5\r\n") != NULL);

    response_reset();
    CHECK(response_status(201) == 0);
    CHECK(response_status_get(200) == 201);

    response_reset();
    CHECK(response_status_get(200) == 200);
    CHECK(response_write(wire, sizeof(wire), 200) == 0);

    response_reset();
    CHECK(response_header(NULL, "v") != 0);
    CHECK(response_header("X", NULL) != 0);
    CHECK(response_header("Bad Name", "v") != 0);
    CHECK(response_header("X", "bad\r\nvalue") != 0);
    CHECK(response_header("X\r\n", "v") != 0);

    response_reset();
    int accepted = 0;
    for (int i = 0; i < RESPONSE_MAX_HEADERS + 4; i++)
    {
        char name[32];
        (void)snprintf(name, sizeof(name), "X-H%d", i);
        if (response_header(name, "v") == 0)
        {
            accepted++;
        }
    }
    CHECK(accepted <= RESPONSE_MAX_HEADERS);

    response_reset();
    CHECK(response_header("X-Test", "value") == 0);
    char tiny[4];
    CHECK(response_write(tiny, sizeof(tiny), 200) < 0);

    TEST_REPORT("response");
}
