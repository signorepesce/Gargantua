#ifndef GARGANTUA_FRAMEWORK_INTERNAL_H
#define GARGANTUA_FRAMEWORK_INTERNAL_H

#include "gargantua.h"

size_t field_storage_size(const FieldInfo *field);
FieldInfo field_scalar(const FieldInfo *field);
int field_is_null(const FieldInfo *field, const void *obj);
void field_set_present(const FieldInfo *field, void *obj);
void *request_alloc(size_t size);
int row_list_write(RowList list, int paginated, char *out, size_t cap);
const char *field_kind_name(FieldKind kind);
int field_text(const TypeInfo *type, const void *row, const char *name, char *out, size_t cap);
int json_write_struct(const TypeInfo *type, const void *obj, char *out, size_t cap);
int create_table_sql_write(const TypeInfo *type, char *out, size_t cap);
int validate_struct(const TypeInfo *type, const void *obj, const char **bad);
int text_write(str text, char *out, size_t cap);
int path_param_int(const RequestParams *p, int index, int *ok);
str path_param_text(const RequestParams *p, int index);
int query_parse(RequestParams *p, char *query);
str query_text(const RequestParams *p, str name);
int query_int(const RequestParams *p, str name, int *ok);
int query_has(const RequestParams *p, str name);
void request_bind(const void *request);
void arena_bind(void *arena);
int error_json_write(int status, str message, char *out, size_t cap);
void field_error_add(str field, str code, str message);
void request_fail_code(int status, str code, str message);
int json_read_mode(const TypeInfo *type, void *obj, char *json, size_t len, int partial);
int patch_apply(const TypeInfo *type, void *target, const void *changes);
void request_fail(int status, str message);
void request_fail_field(str field);
int request_failed(void);
int request_fail_status(void);
str request_fail_message(void);
void request_fail_reset(void);
int request_active(void);
void transaction_abort_if_open(void);
int transaction_depth(void);
int dispatch_task(void (*handler)(void));
int dispatch_route(DispatchFn handler, const RequestParams *params, char *body, size_t len, char *out, size_t cap);
int url_decode(char *text);
int json_read_struct(const TypeInfo *type, void *obj, char *json, size_t len);

#endif
