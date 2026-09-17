#include <gargantua.h>
# include "local.h"

static int helper(void)
{
    return SERVICE_BONUS;
}

int total (int value)
{
    return increment(value) + helper();
}

$transactional
void empty_transaction(void)
{
}

$transactional
Person save_person(Person person)
{
    empty_transaction();
    person.id = $Person_insert(person);
    return person;
}

$transactional
void reject_person(void)
{
    Person person = { .name = "rollback" };
    (void)save_person(person);
    $throw(409, "intentional rollback");
}
