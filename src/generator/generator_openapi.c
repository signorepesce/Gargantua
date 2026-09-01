#include "generator.h"
#include "server.h"
#include <assert.h>
#include <string.h>

#define OPENAPI_MAX_DOCUMENT ((size_t)SERVER_BODY_MAX - 1u)

typedef struct
{
    FILE *file;
    size_t size;
    int failed;
} OpenApiWriter;

static void openapi_write_text(OpenApiWriter *w, const char *s)
{
    size_t n = strlen(s);
    if (w->failed != 0) { return; }
    if (n > OPENAPI_MAX_DOCUMENT - w->size)
    {
        (void)fprintf(stderr, "OpenAPI document exceeds SERVER_BODY_MAX - 1 (%zu bytes)\n", OPENAPI_MAX_DOCUMENT);
        w->failed = 1;
        return;
    }
    if (fwrite(s, 1u, n, w->file) != n)
    {
        w->failed = 1;
        return;
    }
    w->size += n;
}

static void openapi_write_quoted(OpenApiWriter *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    openapi_write_text(w, "\"");
    for (size_t i = 0u; (i < GENERATOR_MAX_URL) && (s[i] != '\0'); i++)
    {
        unsigned char c = (unsigned char)s[i];
        char escaped[7] = {0};
        if ((c == (unsigned char)'"') || (c == (unsigned char)'\\'))
        {
            escaped[0] = '\\';
            escaped[1] = (char)c;
        }
        else if (c < 32u)
        {
            memcpy(escaped, "\\u00", 4u);
            escaped[4] = hex[c >> 4u];
            escaped[5] = hex[c & 15u];
        }
        else
        {
            escaped[0] = (char)c;
        }
        openapi_write_text(w, escaped);
    }
    openapi_write_text(w, "\"");
}

static int openapi_type_exists(const Generator *ctx, const char *name)
{
    for (int i = 0; i < ctx->type_count; i++)
    {
        if (strcmp(ctx->types[i].name, name) == 0) { return 1; }
    }
    return 0;
}

static void openapi_write_schema_ref(OpenApiWriter *w, const Generator *ctx, const char *name)
{
    char ref[GENERATOR_MAX_NAME + 32];
    if (openapi_type_exists(ctx, name) == 0)
    {
        w->failed = 1;
        return;
    }
    (void)snprintf(ref, sizeof(ref), "#/components/schemas/%s", name);
    openapi_write_text(w, "{\"$ref\":");
    openapi_write_quoted(w, ref);
    openapi_write_text(w, "}");
}

static const char *openapi_scalar_type(const char *type)
{
    if ((strcmp(type, "int") == 0) || (strcmp(type, "FIELD_INT") == 0))
    {
        return "{\"type\":\"integer\",\"format\":\"int32\"}";
    }
    if ((strcmp(type, "long") == 0) || (strcmp(type, "FIELD_LONG") == 0))
    {
        return "{\"type\":\"integer\",\"format\":\"int64\"}";
    }
    if ((strcmp(type, "double") == 0) || (strcmp(type, "FIELD_DOUBLE") == 0))
    {
        return "{\"type\":\"number\",\"format\":\"double\"}";
    }
    if ((strcmp(type, "bool") == 0) || (strcmp(type, "FIELD_BOOL") == 0))
    {
        return "{\"type\":\"boolean\"}";
    }
    if ((strcmp(type, "str") == 0) || (strcmp(type, "FIELD_STR") == 0))
    {
        return "{\"type\":\"string\"}";
    }
    return NULL;
}

static void openapi_write_schema(OpenApiWriter *w, const Generator *ctx, const char *type)
{
    const char *scalar = openapi_scalar_type(type);
    if (scalar != NULL) { openapi_write_text(w, scalar); }
    else { openapi_write_schema_ref(w, ctx, type); }
}

static void openapi_write_type_schema(OpenApiWriter *w, const Generator *ctx, const ParsedType *type, int partial, unsigned depth);

static void openapi_write_inline_type(OpenApiWriter *w, const Generator *ctx, const char *name, unsigned depth)
{
    assert(w != NULL);
    assert(ctx != NULL);

    for (int j = 0; j < ctx->type_count; j++)
    {
        if (strcmp(ctx->types[j].name, name) == 0)
        {
            openapi_write_type_schema(w, ctx, &ctx->types[j], 1, depth);
            return;
        }
    }
}

static void openapi_write_validation(OpenApiWriter *w, const ParsedField *field)
{
    assert(w != NULL);
    assert(field != NULL);

    char rule[256];
    int  integral = (strcmp(field->kind, "FIELD_INT") == 0) ||
                    (strcmp(field->kind, "FIELD_LONG") == 0);

    if ((field->validation & 1u) != 0u)
    {
        if (integral) { (void)snprintf(rule, sizeof(rule), ",\"minimum\":%ld", field->min_int); }
        else          { (void)snprintf(rule, sizeof(rule), ",\"minimum\":%.21Lg", field->min); }
        openapi_write_text(w, rule);
    }
    if ((field->validation & 2u) != 0u)
    {
        if (integral) { (void)snprintf(rule, sizeof(rule), ",\"maximum\":%ld", field->max_int); }
        else          { (void)snprintf(rule, sizeof(rule), ",\"maximum\":%.21Lg", field->max); }
        openapi_write_text(w, rule);
    }
    if ((field->validation & 4u) != 0u)
    {
        (void)snprintf(rule, sizeof(rule), ",\"minLength\":%u,\"maxLength\":%u", field->size_min, field->size_max);
        openapi_write_text(w, rule);
    }
    if ((field->validation & 8u) != 0u) { openapi_write_text(w, ",\"format\":\"email\""); }
    if (field->references[0])
    {
        openapi_write_text(w, ",\"x-references\":{\"table\":");
        openapi_write_quoted(w, field->references);
        openapi_write_text(w, ",\"key\":");
        openapi_write_quoted(w, field->reference_key);
        openapi_write_text(w, "}");
    }
}

static void openapi_write_scalar_field(OpenApiWriter *w, const ParsedField *field)
{
    assert(w != NULL);
    assert(field != NULL);

    const char *scalar = openapi_scalar_type(field->kind);
    if (scalar == NULL)
    {
        w->failed = 1;
        return;
    }

    if ((strcmp(field->kind, "FIELD_STR") == 0) && ((field->flags & 2u) == 0u))
    {
        scalar = "{\"type\":[\"string\",\"null\"]}";
    }

    char prefix[128];
    (void)snprintf(prefix, sizeof(prefix), "%s", scalar);
    prefix[strlen(prefix) - 1u] = '\0';
    openapi_write_text(w, prefix);

    openapi_write_validation(w, field);
    openapi_write_text(w, "}");
}

static void openapi_write_required(OpenApiWriter *w, const ParsedType *type, int partial)
{
    assert(w != NULL);
    assert(type != NULL);

    int required = 0;
    for (int i = 0; (i < type->field_count) && (w->failed == 0); i++)
    {
        if ((partial == 0) && ((type->fields[i].flags & 2u) != 0u))
        {
            openapi_write_text(w, (required == 0) ? ",\"required\":[" : ",");
            openapi_write_quoted(w, type->fields[i].name);
            required++;
        }
    }

    if (required != 0) { openapi_write_text(w, "]"); }
}

static void openapi_write_type_schema(OpenApiWriter *w, const Generator *ctx, const ParsedType *type, int partial, unsigned depth)
{
    if ((depth >= 8u) || w->failed)
    {
        w->failed = 1;
        return;
    }

    openapi_write_text(w, "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{");

    for (int i = 0; (i < type->field_count) && (w->failed == 0); i++)
    {
        const ParsedField *field = &type->fields[i];

        if (i != 0) { openapi_write_text(w, ","); }
        openapi_write_quoted(w, field->name);
        openapi_write_text(w, ":");

        if (strcmp(field->kind, "FIELD_OBJECT") != 0)
        {
            openapi_write_scalar_field(w, field);
        }
        else if (partial == 0)
        {
            openapi_write_schema_ref(w, ctx, field->c_type);
        }
        else
        {
            openapi_write_inline_type(w, ctx, field->c_type, depth + 1u);
        }
    }

    openapi_write_text(w, "}");
    openapi_write_required(w, type, partial);
    openapi_write_text(w, "}");
}

static void openapi_write_response(OpenApiWriter *w, const Generator *ctx, const ParsedRoute *route)
{
    if (route->collection == 2)
    {
        openapi_write_text(w, "{\"type\":\"object\",\"required\":[\"items\",\"page\",\"size\",\"hasMore\"]," "\"properties\":{\"items\":");
    }
    if (route->collection != 0) { openapi_write_text(w, "{\"type\":\"array\",\"maxItems\":100,\"items\":"); }
    openapi_write_schema(w, ctx, route->return_type);
    if (route->collection != 0) { openapi_write_text(w, "}"); }
    if (route->collection == 2)
    {
        openapi_write_text(w, ",\"page\":{\"type\":\"integer\",\"format\":\"int32\",\"minimum\":0}," "\"size\":{\"type\":\"integer\",\"format\":\"int32\",\"minimum\":1,\"maximum\":100}," "\"hasMore\":{\"type\":\"boolean\"}}}");
    }
}

static void openapi_write_error_responses(OpenApiWriter *w)
{
    static const char *const codes[] = {"400", "401", "403", "404", "405", "409", "413", "415", "429", "500", "503", "default"};
    for (size_t i = 0u; i < sizeof(codes) / sizeof(codes[0]); i++)
    {
        openapi_write_text(w, ",");
        openapi_write_quoted(w, codes[i]);
        openapi_write_text(w, ":{\"$ref\":\"#/components/responses/GargantuaError\"}");
    }
}

static void openapi_write_security(OpenApiWriter *w, const ParsedRoute *route)
{
    assert(w != NULL);
    assert(route != NULL);

    if (route->public_route)
    {
        openapi_write_text(w, ",\"security\":[]");
    }
    else if (route->authenticated || route->role[0])
    {
        openapi_write_text(w, ",\"security\":[{\"BearerAuth\":[]}]");
    }
    else
    {
        openapi_write_text(w, ",\"security\":[{},{\"BearerAuth\":[]}]," "\"x-security-config\":\"security.enabled\"");
    }

    if (route->role[0])
    {
        openapi_write_text(w, ",\"x-required-role\":");
        openapi_write_quoted(w, route->role);
    }
}

static void openapi_write_parameters(OpenApiWriter *w, const Generator *ctx, const ParsedRoute *route)
{
    assert(w != NULL);
    assert(route != NULL);

    int parameters = 0;

    for (int i = 0; i < route->param_count; i++)
    {
        const ParsedParam *param = &route->params[i];
        if (param->is_body != 0) { continue; }

        openapi_write_text(w, (parameters == 0) ? ",\"parameters\":[" : ",");
        parameters++;

        openapi_write_text(w, "{\"name\":");
        openapi_write_quoted(w, param->name);
        openapi_write_text(w, (param->is_query != 0) ? ",\"in\":\"query\"" : ",\"in\":\"path\"");
        openapi_write_text(w, (param->is_query == 0) ? ",\"required\":true,\"schema\":" : ",\"required\":false,\"schema\":");
        openapi_write_schema(w, ctx, param->type);
        openapi_write_text(w, "}");
    }

    if (parameters != 0) { openapi_write_text(w, "]"); }
}

