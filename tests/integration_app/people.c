#include <gargantua.h>

$post("/people")
Person create_person(Person body)
{
    Person result = save_person(body);
    $location($format("/people/%d", result.id));
    return result;
}

$get("/people/{id}")
Person show_person(int id)
{
    return $Person_find(id);
}

$patch("/people/{id}")
Person patch_person(int id, Person body)
{
    Person current = $Person_find(id);
    Person updated = $Person_apply(current, body);
    if ($Person_update(updated) != 1)
    {
        $throw(500, "update failed", updated);
    }
    return updated;
}

$get("/people")
$page(Person) find_people(int page, int size, str name, str order)
{
    DbQuery query = $query(.page = page, .size = size,
        .order = $empty(order) ? "id" : order);
    if (!$empty(name))
    {
        query.filters[0] = $filter(name, "=", name);
        query.count = 1;
    }
    return $Person_search(query);
}

$get("/adults")
$page(Person) adults(void)
{
    return $Person_search($query(.filters = { $filter(age, ">=", 18) },
        .count = 1, .order = "age", .descending = true, .size = 2));
}

$get("/unknown-age")
$list(Person) unknown_age(void)
{
    return $Person_search($query(.filters = { $filter_null(age) }, .count = 1));
}

$get("/named")
$list(Person) named(str name)
{
    return $Person_by_name(name, 0, 20);
}

$get("/summary/{id}")
Summary summary(int id)
{
    Person person = $Person_find(id);
    return (Summary){ person, $Person_fetch_department_id(person), "extra DTO field" };
}

$post("/rollback")
void rollback(void)
{
    reject_person();
}

$post("/commit")
$transactional
Person commit(Person body)
{
    return save_person(body);
}

$get("/total")
int total_route(void)
{
    return total(4);
}

$get("/void")
void empty_route(void)
{
    empty_transaction();
}

$get("/protected")
$authenticated
str protected_route(void)
{
    return "protected";
}

$get("/admin")
$role("admin")
str admin_route(void)
{
    return "admin";
}
