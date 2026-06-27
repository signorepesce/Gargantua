#include "auth.h"
#include "config.h"
#include "http.h"
#include "gargantua.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TOKEN_CAP 256u

static char g_token[TOKEN_CAP];
static char g_admin_token[TOKEN_CAP];
static int g_enabled;
static int g_user;
static int g_admin;
static _Thread_local int t_user;
static _Thread_local const char *t_role;

static int token_copy(char *out, const char *value, const char *key)
{
    size_t n = strlen(value);
    if (n == 0u) { out[0] = '\0'; return 0; }
    if (n < 32u)
    {
        (void)fprintf(stderr, "gargantua: %s has %zu characters;" " at least 32 are required\n", key, n);
        return -1;
    }
    if (n >= TOKEN_CAP)
    {
        (void)fprintf(stderr, "gargantua: %s exceeds %u characters\n", key, (unsigned)TOKEN_CAP - 1u);
        return -1;
    }
    for (size_t i = 0u; i < n; i++)
    {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21u || c > 0x7eu)
        {
            (void)fprintf(stderr, "gargantua: %s has a non-ASCII character" " at position %zu\n", key, i);
            return -1;
        }
    }
    memcpy(out, value, n + 1u);
    return 0;
}

int auth_init(void)
{
    const char *enabled = config_str("security.enabled", "false");
    if (strcmp(enabled, "false") != 0 && strcmp(enabled, "true") != 0)
    {
        (void)fprintf(stderr, "gargantua: security.enabled must be" " true or false, not '%s'\n", enabled);
        return -1;
    }
    g_enabled = strcmp(enabled, "true") == 0;
    g_user = config_int("security.user_id", 1);
    g_admin = config_int("security.admin_id", 2);
    if (g_user <= 0 || g_admin <= 0)
    {
        (void)fprintf(stderr, "gargantua: security.user_id and security.admin_id" " must be positive\n");
        return -1;
    }
    if (token_copy(g_token, config_str("security.token", ""), "security.token") != 0 || token_copy(g_admin_token, config_str("security.admin_token", ""), "security.admin_token") != 0) { return -1; }
    if (g_token[0] && strcmp(g_token, g_admin_token) == 0)
    {
        (void)fprintf(stderr, "gargantua: security.token and" " security.admin_token are identical\n");
        return -1;
    }
    if (g_enabled && !g_token[0] && !g_admin_token[0])
    {
        (void)fprintf(stderr, "gargantua: security.enabled=true but no" " token has been set\n");
        return -1;
    }
    return 0;
}

static int secret_equals(const char *value, size_t len, const char *secret)
{
    size_t n = strlen(secret);
    if (n == 0u || len >= TOKEN_CAP) { return 0; }
    unsigned different = (unsigned)(len ^ n);
    for (size_t i = 0u; i < TOKEN_CAP; i++)
    {
        unsigned a = i < len ? (unsigned char)value[i] : 0u;
        unsigned b = i < n ? (unsigned char)secret[i] : 0u;
        different |= a ^ b;
    }
    return different == 0u;
}

void auth_reset(void)
{
    t_user = 0;
    t_role = "";
}

void auth_bind(const void *request)
{
    auth_reset();
    const HttpRequest *req = request;
    if (req == NULL) { return; }
    size_t len = 0u;
    const char *value = http_find_header(req, "Authorization", &len);
    if (value == NULL || len <= 7u || strncasecmp(value, "Bearer ", 7u) != 0) { return; }
    value += 7;
    len -= 7u;
    int admin = secret_equals(value, len, g_admin_token);
    int user = secret_equals(value, len, g_token);
    if (admin) { t_user = g_admin; t_role = "admin"; }
    else if (user) { t_user = g_user; t_role = "user"; }
}

int auth_gate(int public_route, const char *role, int required)
{
    if (public_route) { return 0; }
    if (!required && !g_enabled && (role == NULL || !*role)) { return 0; }
    if (!t_user)
    {
        (void)response_header("WWW-Authenticate", "Bearer");
        request_fail(401, "authentication required");
        return -1;
    }
    if (role != NULL && *role && strcmp(role, t_role) != 0)
    {
        request_fail(403, "permission denied");
        return -1;
    }
    return 0;
}

int auth_user_id(void)
{
    return t_user;
}

void auth_require_owner(int id)
{
    if (!t_user) { request_raise(401, "authentication required"); }
    else if (id != t_user) { request_raise(403, "permission denied"); }
}

void auth_require_role(const char *role)
{
    if (!t_user) { request_raise(401, "authentication required"); }
    else if (role == NULL || strcmp(role, t_role) != 0) { request_raise(403, "permission denied"); }
}
