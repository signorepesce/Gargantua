#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "store.h"

#include <assert.h>
#include <string.h>

static size_t store_field_size(FieldKind kind)
{
    switch (kind)
    {
        case FIELD_INT: return sizeof(int);
        case FIELD_LONG: return sizeof(long);
        case FIELD_DOUBLE: return sizeof(double);
        case FIELD_BOOL: return sizeof(bool);
        case FIELD_STR: return sizeof(str);
        default: return 0u;
    }
}

static int store_type_valid(const TypeInfo *type)
{
    if ((type == NULL) || (type->size == 0u) || (type->fields == NULL) || (type->field_count == 0u) || (type->field_count > STORE_MAX_FIELDS)) { return 0; }
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        const FieldInfo *field = &type->fields[i];
        size_t size = store_field_size(field->kind);
        if ((size == 0u) || (field->size != size) || (field->nested != NULL) || (field->offset > type->size) || (size > (size_t)type->size - field->offset)) { return 0; }
        for (unsigned j = 0u; j < i; j++)
        {
            const FieldInfo *prior = &type->fields[j];
            if (((size_t)field->offset < (size_t)prior->offset + prior->size) && ((size_t)prior->offset < (size_t)field->offset + size)) { return 0; }
        }
    }
    return 1;
}

int store_text_fields(const TypeInfo *type)
{
    if (store_type_valid(type) == 0) { return 0; }
    int count = 0;
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        if (type->fields[i].kind == FIELD_STR) { count++; }
    }
    return count;
}

static int store_valid(const Store *store)
{
    if ((store == NULL) || (store_type_valid(store->type) == 0) || (store->rows == NULL) || (store->capacity < 1) || (store->capacity > STORE_MAX_ROWS)) { return 0; }
    int texts = store_text_fields(store->type);
    return ((texts == store->text_fields) && ((texts == 0) || (store->text != NULL))) ? 1 : 0;
}

static int store_count_valid(const Store *store)
{
    return ((store->count >= 0) && (store->count <= store->capacity)) ? 1 : 0;
}

static int store_value_valid(const TypeInfo *type, const void *value)
{
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        const FieldInfo *field = &type->fields[i];
        if (field->kind == FIELD_STR)
        {
            str text = NULL;
            memcpy(&text, (const char *)value + field->offset, sizeof(text));
            if ((text != NULL) && (strnlen(text, STORE_TEXT_LEN) >= STORE_TEXT_LEN)) { return 0; }
        }
    }
    return 1;
}

static void store_copy_value(Store *store, int index, const void *value)
{
    char *row = (char *)store->rows + ((size_t)index * store->type->size);
    memmove(row, value, store->type->size);
    size_t slot = 0u;
    for (unsigned i = 0u; i < store->type->field_count; i++)
    {
        const FieldInfo *field = &store->type->fields[i];
        if (field->kind != FIELD_STR) { continue; }
        char *target = store->text +
            (((size_t)index * (size_t)store->text_fields + slot) * STORE_TEXT_LEN);
        str source = NULL;
        memcpy(&source, row + field->offset, sizeof(source));
        if (source != NULL)
        {
            size_t length = strnlen(source, STORE_TEXT_LEN);
            assert(length < STORE_TEXT_LEN);
            memmove(target, source, length + 1u);
            source = target;
        }
        else
        {
            target[0] = '\0';
        }
        memcpy(row + field->offset, &source, sizeof(source));
        slot++;
    }
}

static int store_copy_row(const Store *store, int index, void *out)
{
    const char *row = (const char *)store->rows + ((size_t)index * store->type->size);
    memcpy(out, row, store->type->size);
    int result = 0;
    for (unsigned i = 0u; i < store->type->field_count; i++)
    {
        const FieldInfo *field = &store->type->fields[i];
        if (field->kind != FIELD_STR) { continue; }
        str source = NULL;
        memcpy(&source, row + field->offset, sizeof(source));
        str copy = (source != NULL) ? arena_intern(source) : NULL;
        if ((source != NULL) && (copy == NULL)) { result = -1; }
        memcpy((char *)out + field->offset, &copy, sizeof(copy));
    }
    if (result != 0) { memset(out, 0, store->type->size); }
    return result;
}

int store_count(Store *store)
{
    if ((store_valid(store) == 0) || (pthread_mutex_lock(&store->lock) != 0)) { return 0; }
    int count = store_count_valid(store) ? store->count : 0;
    (void)pthread_mutex_unlock(&store->lock);
    return count;
}

static int store_write(Store *store, int index, const void *value, int append)
{
    if ((store_valid(store) == 0) || (value == NULL) || (pthread_mutex_lock(&store->lock) != 0)) { return -1; }
    int result = -1;
    if (append != 0) { index = store->count; }
    if (store_count_valid(store) && (index >= 0) && (index < store->capacity) && ((append != 0) || (index < store->count)) && store_value_valid(store->type, value))
    {
        store_copy_value(store, index, value);
        if (append != 0) { store->count++; }
        result = index;
    }
    (void)pthread_mutex_unlock(&store->lock);
    return result;
}

int store_add(Store *store, const void *value)
{
    return store_write(store, 0, value, 1);
}

int store_put(Store *store, int index, const void *value)
{
    return (store_write(store, index, value, 0) >= 0) ? 0 : -1;
}

int store_get(Store *store, int index, void *out)
{
    if ((store_valid(store) == 0) || (out == NULL)) { return -1; }
    memset(out, 0, store->type->size);
    if (pthread_mutex_lock(&store->lock) != 0) { return -1; }
    int result = -1;
    if (store_count_valid(store) && (index >= 0) && (index < store->count)) { result = store_copy_row(store, index, out); }
    (void)pthread_mutex_unlock(&store->lock);
    return result;
}

RowList store_list(Store *store)
{
    RowList list = {0};
    if ((store_valid(store) == 0) || (pthread_mutex_lock(&store->lock) != 0))
    {
        request_fail(500, "invalid store");
        return list;
    }
    int count = store->count;
    if ((store_count_valid(store) == 0) || (count > PAGE_MAX))
    {
        (void)pthread_mutex_unlock(&store->lock);
        request_fail(500, "store exceeds collection limit");
        return list;
    }
    void *rows = (count > 0) ? request_alloc((size_t)count * store->type->size) : NULL;
    int failed = ((count > 0) && (rows == NULL));
    for (int i = 0; (i < count) && (failed == 0); i++)
    {
        char *out = (char *)rows + ((size_t)i * store->type->size);
        failed = (store_copy_row(store, i, out) != 0);
    }
    (void)pthread_mutex_unlock(&store->lock);
    if (failed != 0)
    {
        request_fail(500, "store snapshot allocation failed");
        return list;
    }
    list.type = store->type;
    list.items = rows;
    list.count = count;
    list.size = (count > 0) ? count : 20;
    return list;
}

void store_clear(Store *store)
{
    if ((store_valid(store) == 0) || (pthread_mutex_lock(&store->lock) != 0)) { return; }
    memset(store->rows, 0, (size_t)store->capacity * store->type->size);
    if (store->text != NULL) { memset(store->text, 0, (size_t)store->capacity * (size_t)store->text_fields * STORE_TEXT_LEN); }
    store->count = 0;
    (void)pthread_mutex_unlock(&store->lock);
}
