#include <gargantua.h>

$post("/notes")
Note note_create(Note body)
{
    body.id = $Note_insert(body);
    return body;
}
