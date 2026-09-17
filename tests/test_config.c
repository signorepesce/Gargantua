#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "config.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PATH = "/tmp/gargantua_test.properties";

static void write_config(const char *body)
{
    FILE *f = fopen(PATH, "w");
    if (f == NULL)
    {
        return;
    }
    (void)fputs(body, f);
    (void)fclose(f);
}

int main(void)
{
    write_config("server.port     = 9100\n"
                 "server.address  = 10.0.0.1\n"
                 "empty.value     =\n"
                 "spaced.key      =   padded   \n"
                 "# a comment line\n"
                 "database.url    = app.db   # trailing comment\n"
                 "not.a.number    = abc\n");

    CHECK(config_load(PATH) > 0);

    CHECK(strcmp(config_str("server.address", "fallback"), "10.0.0.1") == 0);
    CHECK(config_int("server.port", 1) == 9100);

    CHECK(strcmp(config_str("absent.key", "fallback"), "fallback") == 0);
    CHECK(config_int("absent.key", 42) == 42);

    CHECK(strcmp(config_str("empty.value", "fallback"), "") == 0);

    CHECK(strcmp(config_str("spaced.key", "x"), "padded") == 0);

    CHECK(strcmp(config_str("database.url", "x"), "app.db") == 0);

    CHECK(config_int("not.a.number", 7) == 7);

    CHECK(setenv("SERVER_ADDRESS", "192.168.1.1", 1) == 0);
    CHECK(strcmp(config_str("server.address", "x"), "192.168.1.1") == 0);
    CHECK(setenv("SERVER_PORT", "8080", 1) == 0);
    CHECK(config_int("server.port", 1) == 8080);
    (void)unsetenv("SERVER_ADDRESS");
    (void)unsetenv("SERVER_PORT");

    CHECK(setenv("SERVER_ADDRESS", "", 1) == 0);
    CHECK(strcmp(config_str("server.address", "x"), "10.0.0.1") == 0);
    (void)unsetenv("SERVER_ADDRESS");

    CHECK(config_load("/tmp/gargantua_does_not_exist.properties") == 0);
    CHECK(strcmp(config_str("server.address", "gone"), "gone") == 0);

    (void)remove(PATH);
    TEST_REPORT("config");
}
