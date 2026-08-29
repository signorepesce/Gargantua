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

