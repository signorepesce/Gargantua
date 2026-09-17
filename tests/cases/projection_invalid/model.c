#include <dto.h>

$table(Person)
{
    $id int id;
};

$json(View)
{
    $pick(Missing, id)
};
