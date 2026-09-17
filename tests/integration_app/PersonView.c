#include <dto.h>

$json(PersonView)
{
    $pick(Person, id, name, age)
    str label;
    int extra;
};
