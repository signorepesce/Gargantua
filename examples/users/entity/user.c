#include <entity.h>

$table(User)
{
    $id
    int id;

    $not_null
    str name;

    $not_null
    $unique
    str email;

    $nullable(int) age;
};
