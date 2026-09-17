#include "framework_internal.h"
#include <string.h>

static size_t value_offset(FieldKind kind)
{
    switch (kind)
    {
    case FIELD_INT:
        return offsetof(Nullable_int, value);
    case FIELD_LONG:
        return offsetof(Nullable_long, value);
    case FIELD_DOUBLE:
        return offsetof(Nullable_double, value);
    case FIELD_BOOL:
        return offsetof(Nullable_bool, value);
    default:
        return 0u;
    }
}

size_t field_storage_size(const FieldInfo *field)
{
    int nullable = (field->flags & FIELD_NULLABLE) != 0u;
    switch (field->kind)
    {
    case FIELD_INT:
        return nullable ? sizeof(Nullable_int) : sizeof(int);
    case FIELD_LONG:
        return nullable ? sizeof(Nullable_long) : sizeof(long);
    case FIELD_DOUBLE:
        return nullable ? sizeof(Nullable_double) : sizeof(double);
    case FIELD_BOOL:
        return nullable ? sizeof(Nullable_bool) : sizeof(bool);
    case FIELD_STR:
        return nullable ? 0u : sizeof(str);
    default:
        return 0u;
    }
}

FieldInfo field_scalar(const FieldInfo *field)
{
    FieldInfo scalar = *field;
    if ((field->flags & FIELD_NULLABLE) != 0u)
    {
        scalar.offset = (unsigned short)(scalar.offset + value_offset(scalar.kind));
        scalar.flags &= ~(unsigned)FIELD_NULLABLE;
        scalar.size = (unsigned short)field_storage_size(&scalar);
    }
    return scalar;
}

int field_is_null(const FieldInfo *field, const void *obj)
{
    if ((field->flags & FIELD_NULLABLE) != 0u)
    {
        bool present = false;
        memcpy(&present, (const char *)obj + field->offset, sizeof(present));
        return !present;
    }
    if (field->kind == FIELD_STR)
    {
        str value = NULL;
        memcpy(&value, (const char *)obj + field->offset, sizeof(value));
        return value == NULL;
    }
    return 0;
}

void field_set_present(const FieldInfo *field, void *obj)
{
    if ((field->flags & FIELD_NULLABLE) != 0u)
    {
        bool present = true;
        memcpy((char *)obj + field->offset, &present, sizeof(present));
    }
}
