#include "db.h"

SqlArg sql_option_int(Nullable_int value)
{
    return value.has_value ? SQL_INT(value.value) : SQL_NULL();
}

SqlArg sql_option_long(Nullable_long value)
{
    return value.has_value ? SQL_LONG(value.value) : SQL_NULL();
}

SqlArg sql_option_double(Nullable_double value)
{
    return value.has_value ? SQL_DOUBLE(value.value) : SQL_NULL();
}

SqlArg sql_option_bool(Nullable_bool value)
{
    return value.has_value ? SQL_BOOL(value.value) : SQL_NULL();
}

SqlArg sql_value_int(int value)
{
    return SQL_INT(value);
}

SqlArg sql_value_long(long value)
{
    return SQL_LONG(value);
}

SqlArg sql_value_double(double value)
{
    return SQL_DOUBLE(value);
}

SqlArg sql_value_bool(bool value)
{
    return SQL_BOOL(value);
}

SqlArg sql_value_text(str value)
{
    return SQL_TEXT(value);
}
