#include <gargantua.h>

$post("/notes")
Note note_create(Note body)
{
    $transaction
    {
        return body;
    }
    return body;
}
