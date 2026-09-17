#include <gargantua.h>

$table(Department)
{
    $id int id;
    $unique
    $not_null str name;
};
