# Gargantua

A small web framework for C. You write annotated structs and handlers; the
generator writes the entry point, the routing table, the JSON binding and the
CRUD layer for you.

## Requirements

A C11 compiler and `libsqlite3`. OpenSSL is optional and enables https in the
outbound client.

## Build and run

    ./eat            build app/ and start the server
    ./eat build      build only, do not run
    ./eat clean      remove build output

There is no Makefile: `eat` is the only build entry point. It compiles the
generator from source, runs it over `app/`, then compiles the generated code
together with the framework into `.build/server`.

## Writing an application

Application code lives in `app/`. Models and handlers must be in **separate
files** — the generator includes the models first, emits the CRUD
declarations, and only then includes the handlers.

`app/Note.c` — the model:

```c
#include <gargantua.h>

$table(Note)
{
    $Id      int id;
    $NotNull str title;
    str body;
};
```

`app/note_controller.c` — the handlers:

```c
#include <gargantua.h>

$on_start
void note_setup(void)
{
    (void)Note_create_table();
}

$get("/notes/{id}")
Note note_show(int id)
{
    return Note_find(id);
}

$post("/notes")
Note note_create(Note body)
{
    body.id = Note_insert(body);
    return body;
}

$get("/notes")
$list(Note) note_index(void)
{
    return Note_list(50);
}
```

Run `./eat` and the routes are live:

    POST /notes  {"title":"first","body":"hello"}
      -> 201 {"id":1,"title":"first","body":"hello"}

    GET  /notes/1
      -> 200 {"id":1,"title":"first","body":"hello"}

    GET  /notes
      -> 200 [{"id":1,...},{"id":2,...}]

    GET  /openapi.json
      -> 200

Validation comes from the field annotations. Posting without `title` returns:

    400 {"code":"validation_failed",
         "fields":[{"field":"title","code":"required"}]}

`Name_insert` returns the new id and requires `$Id` to be `0` on input.

## Configuration

`app/application.properties` is the only configuration file:

    server.port  = 8100
    database.url = gargantua.db

Every key can be overridden by an environment variable: dots become
underscores and the name is upper-cased, so `server.port` is `SERVER_PORT`.
The environment wins over the file. `GARGANTUA_CONFIG` points at a different
properties file.

## Layout

    include/        public headers
    src/core/       arena allocator, configuration
    src/http/       http, json, routing, responses, static files, templates
    src/db/         sqlite backend, migrations, key-value store
    src/runtime/    request runtime, auth, scheduler, http client
    src/server/     tcp server and worker pool
    src/generator/  the code generator
    app/            your application
