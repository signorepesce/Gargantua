#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "generator.h"
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "emit_internal.h"

void emit_fields(FILE *out, const ParsedType *type)
{
    assert(out != NULL);
    assert(type != NULL);
    assert(type->field_count >= 0);

    (void)fprintf(out, "_Static_assert(sizeof(%s) <= USHRT_MAX," " \"$table(%s) is too large\");\n", type->name, type->name);

    for (int i = 0; (i < type->field_count) && (i < GENERATOR_MAX_FIELDS); i++)
    {
        const ParsedField *f = &type->fields[i];
        (void)fprintf(out, "_Static_assert(offsetof(%s, %s) <= USHRT_MAX," " \"field offset is too large\");\n" "_Static_assert(sizeof(((%s *)0)->%s) <= USHRT_MAX," " \"field is too large\");\n", type->name, f->name, type->name, f->name);
    }

    (void)fprintf(out, "static const FieldInfo %s__f[] =\n{\n", type->name);

    for (int i = 0; (i < type->field_count) && (i < GENERATOR_MAX_FIELDS); i++)
    {
        const ParsedField *f = &type->fields[i];
        (void)fprintf(out, "    { \"%s\", %s," " (unsigned short)offsetof(%s, %s)," " (unsigned short)sizeof(((%s *)0)->%s), %uu, ", f->name, f->kind, type->name, f->name, type->name, f->name, f->flags);
        if (strcmp(f->kind, "FIELD_OBJECT") == 0)
        {
            (void)fprintf(out, "&%s__type", f->c_type);
        }
        else { (void)fprintf(out, "NULL"); }
        if (f->references[0] != '\0')
        {
            (void)fprintf(out, ", \"%s\", \"%s\"", f->references, f->reference_key);
        }
        else { (void)fprintf(out, ", NULL, NULL"); }
        (void)fprintf(out, ", {%uu, %.21LeL, %.21LeL, %uu, %uu, (%ldL - %dL), (%ldL - %dL)} },\n", f->validation, f->min, f->max, f->size_min, f->size_max, f->min_int == LONG_MIN ? LONG_MIN + 1 : f->min_int, f->min_int == LONG_MIN, f->max_int == LONG_MIN ? LONG_MIN + 1 : f->max_int, f->max_int == LONG_MIN);
    }

    (void)fprintf(out, "};\n\n");
}

void emit_type(FILE *out, const ParsedType *type)
{
    assert(out != NULL);
    assert(type != NULL);

    (void)fprintf(out, "const TypeInfo %s__type =\n" "{\n" "    \"%s\", (unsigned short)sizeof(%s), %d, %s__f\n" "};\n\n", type->name, type->name, type->name, type->field_count, type->name);
}

static const ParsedField *find_primary_key(const ParsedType *type)
{
    assert(type != NULL);
    assert(type->field_count >= 0);

    for (int i = 0; (i < type->field_count) && (i < GENERATOR_MAX_FIELDS); i++)
    {
        if ((type->fields[i].flags & 1u) != 0u) { return &type->fields[i]; }
    }
    return NULL;
}

static void emit_sql_argument(FILE *out, const ParsedField *f, const char *var)
{
    assert(out != NULL);
    assert(f != NULL);

    const char *macro = "SQL_INT";
    if (strcmp(f->kind, "FIELD_STR") == 0)
    {
        macro = "SQL_TEXT";
    }
    else if (strcmp(f->kind, "FIELD_LONG") == 0)
    {
        macro = "SQL_LONG";
    }
    else if (strcmp(f->kind, "FIELD_DOUBLE") == 0)
    {
        macro = "SQL_DOUBLE";
    }
    else if (strcmp(f->kind, "FIELD_BOOL") == 0)
    {
        macro = "SQL_BOOL";
    }

    (void)fprintf(out, "%s(%s.%s)", macro, var, f->name);
}

void emit_crud_declarations(FILE *out, const ParsedType *t)
{
    assert(out != NULL);
    assert(t != NULL);

    (void)fprintf(out, "%s %s_at(RowList wrap_list, int wrap_index);\n", t->name, t->name);
    (void)fprintf(out, "%s %s_apply(%s current, %s changes);\n", t->name, t->name, t->name, t->name);
    if (find_primary_key(t) == NULL || t->json_only != 0) { return; }
    (void)fprintf(out, "RowList %s_page(int wrap_page, int wrap_size);\n" "RowList %s_list(int wrap_limit);\n", t->name, t->name);
    for (int i = 0; i < t->field_count; i++)
    {
        const ParsedField *f = &t->fields[i];
        if (f->references[0] == '\0') { continue; }
        (void)fprintf(out, "RowList %s_page_by_%s(int wrap_id, int wrap_page, int wrap_size);\n" "%s %s_fetch_%s(%s wrap_row);\n", t->name, f->name, f->references, t->name, f->name, t->name);
    }

    (void)fprintf(out, "int  %s_create_table(void);\n" "int  %s_exists(int wrap_id);\n" "int  %s_get(int wrap_id, %s *wrap_out);\n" "%s %s_find(int wrap_id);\n" "int  %s_all(%s *wrap_rows, int wrap_max);\n", t->name, t->name, t->name, t->name, t->name, t->name, t->name, t->name);
    (void)fprintf(out, "int  %s_insert(%s wrap_row);\n" "int  %s_update(%s wrap_row);\n" "int  %s_save(%s wrap_row);\n" "int  %s_delete(int wrap_id);\n", t->name, t->name, t->name, t->name, t->name, t->name, t->name);
}

static void emit_crud_reads(FILE *out, const ParsedType *t, const ParsedField *pk)
{
    assert(out != NULL);
    assert(t != NULL && pk != NULL);

    (void)fprintf(out, "\nint %s_create_table(void)\n{\n" "    return db_create_table(&%s__type);\n}\n", t->name, t->name);
    (void)fprintf(out, "\nint %s_get(int wrap_id, %s *wrap_out)\n{\n" "    if (wrap_out == NULL) { return -1; }\n" "    memset(wrap_out, 0, sizeof(*wrap_out));\n" "    if (wrap_id <= 0) { return -1; }\n" "    int wrap_rc = db_query_one(&%s__type, wrap_out,\n" "        \"SELECT * FROM \\\"%s\\\" WHERE \\\"%s\\\" = ?\",\n" "        SQL_ARGS(SQL_INT(wrap_id)));\n" "    if (wrap_rc != 1) { memset(wrap_out, 0, sizeof(*wrap_out)); }\n" "    return wrap_rc;\n}\n", t->name, t->name, t->name, t->name, pk->name);
    (void)fprintf(out, "\n%s %s_find(int wrap_id)\n{\n" "    %s wrap_row = {0};\n" "    int wrap_rc = %s_get(wrap_id, &wrap_row);\n" "    if (wrap_rc < 0) { request_raise(500, \"%s lookup failed\"); }\n" "    else if (wrap_rc == 0) { request_raise(404, \"%s not found\"); }\n" "    return wrap_row;\n}\n", t->name, t->name, t->name, t->name, t->name, t->name);
    (void)fprintf(out, "\nint %s_exists(int wrap_id)\n{\n" "    if (wrap_id <= 0) { return -1; }\n" "    int wrap_key = 0;\n" "    static const FieldInfo wrap_field =\n" "        { \"%s\", FIELD_INT, 0u, (unsigned short)sizeof(int), 0u, NULL, NULL, NULL, {0} };\n" "    static const TypeInfo wrap_projection =\n" "        { \"%s\", (unsigned short)sizeof(int), 1u, &wrap_field };\n" "    return db_query_one(&wrap_projection, &wrap_key,\n" "        \"SELECT \\\"%s\\\" FROM \\\"%s\\\" WHERE \\\"%s\\\" = ?\",\n" "        SQL_ARGS(SQL_INT(wrap_id)));\n}\n", t->name, pk->name, t->name, pk->name, t->name, pk->name);
    (void)fprintf(out, "\nint %s_all(%s *wrap_rows, int wrap_max)\n{\n" "    if ((wrap_rows == NULL) || (wrap_max <= 0)) { return -1; }\n" "    return db_query_many(&%s__type, wrap_rows, wrap_max,\n" "        \"SELECT * FROM \\\"%s\\\" ORDER BY \\\"%s\\\" LIMIT ?\",\n" "        SQL_ARGS(SQL_INT(wrap_max)));\n}\n", t->name, t->name, t->name, t->name, pk->name);
    (void)fprintf(out, "\nint %s_delete(int wrap_id)\n{\n" "    if (wrap_id <= 0) { return -1; }\n" "    int wrap_rc = db_execute(\n" "        \"DELETE FROM \\\"%s\\\" WHERE \\\"%s\\\" = ?\",\n" "        SQL_ARGS(SQL_INT(wrap_id)));\n" "    return (wrap_rc == 0 || wrap_rc == 1) ? wrap_rc : -1;\n}\n", t->name, t->name, pk->name);
}

static void emit_crud_args(FILE *out, const ParsedType *t, const ParsedField *pk, int include_id)
{
    assert(out != NULL);
    assert(t != NULL && pk != NULL);

    if ((t->field_count == 1) && (include_id == 0))
    {
        (void)fprintf(out, "SQL_NOARGS");
        return;
    }

    (void)fprintf(out, "SQL_ARGS(");
    int first = 1;
    for (int i = 0; (i < t->field_count) && (i < GENERATOR_MAX_FIELDS); i++)
    {
        if (&t->fields[i] == pk) { continue; }
        if (first == 0) { (void)fprintf(out, ", "); }
        emit_sql_argument(out, &t->fields[i], "wrap_row");
        first = 0;
    }
    if (include_id != 0) { (void)fprintf(out, "%sSQL_INT(wrap_row.%s)", (first == 1) ? "" : ", ", pk->name); }
    (void)fprintf(out, ")");
}

static void emit_sql_fragment(FILE *out, const char *text, int bytes)
{
    assert(out != NULL);
    assert(text != NULL && strlen(text) < GENERATOR_MAX_PATH);

    for (size_t i = 0u; text[i] != '\0'; i++)
    {
        if (bytes != 0)
        {
            (void)fprintf(out, "%u,", (unsigned)(unsigned char)text[i]);
            if ((i % 16u) == 15u) { (void)fputc('\n', out); }
        }
        else
        {
            if (text[i] == '"' || text[i] == '\\') { (void)fputc('\\', out); }
            (void)fputc(text[i], out);
        }
    }
}

static void emit_sql_name(FILE *out, const char *name, int bytes)
{
    assert(out != NULL);
    assert(name != NULL);

    emit_sql_fragment(out, "\"", bytes);
    emit_sql_fragment(out, name, bytes);
    emit_sql_fragment(out, "\"", bytes);
}

static void emit_crud_insert(FILE *out, const ParsedType *t, const ParsedField *pk)
{
    assert(out != NULL);
    assert(t != NULL && pk != NULL);

    int bytes = t->field_count > 32 ? 1 : 0;
    (void)fprintf(out, "\nint %s_insert(%s wrap_row)\n{\n" "    if (wrap_row.%s != 0) { return -1; }\n" "    const char *wrap_bad = NULL;\n" "    if (validate_struct(&%s__type, &wrap_row, &wrap_bad) != 0) { return -1; }\n" "    static const char wrap_sql[] = %s", t->name, t->name, pk->name, t->name, bytes != 0 ? "{\n" : "\"");

    emit_sql_fragment(out, "INSERT INTO ", bytes);
    emit_sql_name(out, t->name, bytes);
    if (t->field_count == 1)
    {
        emit_sql_fragment(out, " DEFAULT VALUES", bytes);
    }
    else
    {
        emit_sql_fragment(out, " (", bytes);
        int first = 1;
        for (int i = 0; (i < t->field_count) && (i < GENERATOR_MAX_FIELDS); i++)
        {
            if (&t->fields[i] == pk) { continue; }
            if (first == 0) { emit_sql_fragment(out, ", ", bytes); }
            emit_sql_name(out, t->fields[i].name, bytes);
            first = 0;
        }
        emit_sql_fragment(out, ") VALUES (", bytes);
        first = 1;
        for (int i = 0; (i < t->field_count) && (i < GENERATOR_MAX_FIELDS); i++)
        {
            if (&t->fields[i] == pk) { continue; }
            emit_sql_fragment(out, first == 1 ? "?" : ", ?", bytes);
            first = 0;
        }
        emit_sql_fragment(out, ")", bytes);
    }
    emit_sql_fragment(out, " RETURNING ", bytes);
    emit_sql_name(out, pk->name, bytes);
    (void)fprintf(out, "%s\n    return db_insert_id(wrap_sql,\n        ", bytes != 0 ? "0};" : "\";");
    emit_crud_args(out, t, pk, 0);
    (void)fprintf(out, ");\n}\n");
}

static void emit_crud_update(FILE *out, const ParsedType *t, const ParsedField *pk)
{
    assert(out != NULL);
    assert(t != NULL && pk != NULL);

    (void)fprintf(out, "\nint %s_update(%s wrap_row)\n{\n" "    if (wrap_row.%s <= 0) { return -1; }\n", t->name, t->name, pk->name);
    if (t->field_count == 1)
    {
        (void)fprintf(out, "    return %s_exists(wrap_row.%s);\n}\n", t->name, pk->name);
        return;
    }

    int bytes = t->field_count > 32 ? 1 : 0;
    (void)fprintf(out, "    const char *wrap_bad = NULL;\n" "    if (validate_struct(&%s__type, &wrap_row, &wrap_bad) != 0) { return -1; }\n" "    static const char wrap_sql[] = %s", t->name, bytes != 0 ? "{\n" : "\"");
    emit_sql_fragment(out, "UPDATE ", bytes);
    emit_sql_name(out, t->name, bytes);
    emit_sql_fragment(out, " SET ", bytes);
    int first = 1;
    for (int i = 0; (i < t->field_count) && (i < GENERATOR_MAX_FIELDS); i++)
    {
        if (&t->fields[i] == pk) { continue; }
        if (first == 0) { emit_sql_fragment(out, ", ", bytes); }
        emit_sql_name(out, t->fields[i].name, bytes);
        emit_sql_fragment(out, " = ?", bytes);
        first = 0;
    }
    emit_sql_fragment(out, " WHERE ", bytes);
    emit_sql_name(out, pk->name, bytes);
    emit_sql_fragment(out, " = ?", bytes);
    (void)fprintf(out, "%s\n    int wrap_rc = db_execute(wrap_sql,\n        ", bytes != 0 ? "0};" : "\";");
    emit_crud_args(out, t, pk, 1);
    (void)fprintf(out, ");\n    return (wrap_rc == 0 || wrap_rc == 1) ? wrap_rc : -1;\n}\n");
}

void emit_crud(FILE *out, const ParsedType *t)
{
    assert(out != NULL);
    assert(t != NULL);

    (void)fprintf(out, "\n%s %s_at(RowList wrap_list, int wrap_index)\n{\n" "    %s wrap_row = {0};\n" "    if (wrap_list.type != &%s__type || wrap_list.items == NULL ||\n" "        wrap_index < 0 || wrap_index >= wrap_list.count || wrap_list.count > PAGE_MAX)\n" "    {\n        request_fail(500, \"invalid list index or type\");\n" "        return wrap_row;\n    }\n" "    memcpy(&wrap_row, (const unsigned char *)wrap_list.items +\n" "           (size_t)wrap_index * sizeof(wrap_row), sizeof(wrap_row));\n" "    return wrap_row;\n}\n", t->name, t->name, t->name, t->name);
    (void)fprintf(out, "\n%s %s_apply(%s current, %s changes)\n{\n" "    if (patch_apply(&%s__type, &current, &changes) != 0)\n" "    {\n        request_raise(request_failed() ? request_fail_status() : 500, request_failed() ? request_fail_message() : \"invalid patch context\");\n    }\n" "    return current;\n}\n", t->name, t->name, t->name, t->name, t->name);
    const ParsedField *pk = find_primary_key(t);
    if (pk == NULL || t->json_only != 0) { return; }
    (void)fprintf(out, "\nRowList %s_page(int wrap_page, int wrap_size)\n{\n" "    return db_query_page(&%s__type, \"%s\", NULL, 0, wrap_page, wrap_size);\n}\n" "\nRowList %s_list(int wrap_limit)\n{\n" "    return %s_page(0, wrap_limit);\n}\n", t->name, t->name, pk->name, t->name, t->name);
    for (int i = 0; i < t->field_count; i++)
    {
        const ParsedField *f = &t->fields[i];
        if (f->references[0] == '\0') { continue; }
        (void)fprintf(out, "\nRowList %s_page_by_%s(int wrap_id, int wrap_page, int wrap_size)\n{\n" "    return db_query_page(&%s__type, \"%s\", \"%s\", wrap_id, wrap_page, wrap_size);\n}\n" "\n%s %s_fetch_%s(%s wrap_row)\n{\n" "    return %s_find(wrap_row.%s);\n}\n", t->name, f->name, t->name, pk->name, f->name, f->references, t->name, f->name, t->name, f->references, f->name);
    }

    (void)fprintf(out, "_Static_assert(%d <= DB_MAX_ARGS, \"CRUD argument limit\");\n", t->field_count);
    emit_crud_reads(out, t, pk);
    emit_crud_insert(out, t, pk);
    emit_crud_update(out, t, pk);
    (void)fprintf(out, "\nint %s_save(%s wrap_row)\n{\n" "    if (wrap_row.%s == 0) { return %s_insert(wrap_row); }\n" "    int wrap_rc = %s_update(wrap_row);\n" "    return (wrap_rc == 1) ? wrap_row.%s : wrap_rc;\n}\n", t->name, t->name, pk->name, t->name, t->name, pk->name);
}
