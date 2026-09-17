#include <rest_controller.h>

$get("/hello")
str hello(void)
{
    return "Hello";
}

$get("/users/{id}")
UserView show_user(int id)
{
    return user_info(id);
}

$get("/users")
$page(User) show_users(int page, int size)
{
    return $User_page(page, size);
}

$post("/users")
User add_user(User user)
{
    User saved = create_user(user);
    $location($format("/users/%d", saved.id));
    return saved;
}
