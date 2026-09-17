#include <gargantua.h>

$table(Person)
{
    $id int id;
    $not_null
    $unique str name;
    $min(0)
    $nullable(int) age;
    $nullable(long) score;
    $nullable(double) weight;
    $nullable(bool) active;
    $references(Department)
    $nullable(int) department_id;
};
