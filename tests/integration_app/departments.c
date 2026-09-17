#include <gargantua.h>

$post("/departments")
Department create_department(Department body)
{
    body.id = $Department_insert(body);
    return body;
}

$delete("/departments/{id}")
void delete_department(int id)
{
    if ($Department_delete(id) != 1)
    {
        $throw(409, "department is referenced or absent");
    }
}

$post("/inline")
void inline_transaction(void)
{
    $transaction
    {
        $log("return { is text }");
        int return_value = 1;
        $transaction
        {
            (void)return_value;
        }
    }
}
