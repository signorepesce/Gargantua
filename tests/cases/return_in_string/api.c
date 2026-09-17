#include <gargantua.h>

$post("/notes")
Note note_create(Note body)
{
    $transaction
    {
        $log("return accepted");
        body.id = $Note_insert(body);
    }
    return body;
}
