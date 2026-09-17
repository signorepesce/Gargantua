#include <dto.h>

$json(UserView)
{
    $pick(User, id, name, age)
    str greeting;
};
