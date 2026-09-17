#include <service.h>

UserView user_info(int id)
{
    UserView view = $UserView_from($User_find(id));
    view.greeting = $format("Hello %s", view.name);
    return view;
}

$transactional
User create_user(User user)
{
    int id = $User_save(user);
    if (id <= 0)
    {
        $throw(409, "Could not save user", user);
    }
    return $User_find(id);
}

$on_start
void prepare_example(void)
{
    if ($User_create_table() != 0)
    {
        $throw(500, "Could not create example table");
    }
    if ($User_exists(1) == 0)
    {
        User user = { .name = "Ada", .email = "ada@example.com", .age = $some(int, 36) };
        (void)create_user(user);
    }
}
