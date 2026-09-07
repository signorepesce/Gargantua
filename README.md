# Gargantua <img width="42" height="42" alt="hat" src="resources/hat.png" />

A small vibe-coded web framework for C. You write annotated structs and handlers; the
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
    $id      int id;
    $not_null str title;
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

`Name_insert` returns the new id and requires `$id` to be `0` on input.

## Annotations

Everything the generator understands. All annotations are lowercase.

**Types** — one per struct.

| | |
|---|---|
| `$table(T)` | Persisted entity. Generates a SQL table and the full CRUD API. |
| `$json(T)` | JSON-only type. No table, no CRUD — use it for request and response shapes. |

Field types are `int`, `long`, `double`, `bool`, `str` (`const char *`), or a
nested `$table`/`$json` type.

**Fields** — constraints, checked on every request before the handler runs.

| | |
|---|---|
| `$id` | Primary key. Must be `int`, one per table, assigned by the database. Insert requires it to be `0`. |
| `$not_null` | Required. A missing value returns 400 with the field named. |
| `$unique` | Unique across the table. |
| `$email` | Must look like an email address. |
| `$min(v)` / `$max(v)` | Numeric bounds. |
| `$size(a, b)` | Length bounds for `str`. |
| `$references(T)` | Foreign key to another `$table`. |

**Routes** — the path is quoted; `{name}` binds to a handler parameter of the
same name.

| | |
|---|---|
| `$get(path)` `$post(path)` `$put(path)` `$patch(path)` `$delete(path)` | Map a handler to a method and path. |

A handler returns a `$table`/`$json` type, `$list(T)` for an array, `$page(T)`
for a paginated collection, or `void` for no body.

**Security**

| | |
|---|---|
| `$authenticated` | Requires a valid token. |
| `$role(name)` | Requires a role — `admin` or `user`. |
| `$public` | Explicitly public, overriding a stricter default. |

**Behaviour**

| | |
|---|---|
| `$produces(type)` | Sets the response content type. |
| `$on_start` | Runs once at startup. Signature `void f(void)`. |
| `$repeat(interval)` | Runs periodically, from `100ms` to `24h`, e.g. `"10s"`. Signature `void f(void)`. |
| `$transactional` | Wraps the whole handler or service in one transaction. |
| `$transaction { }` | An inline transaction block. Never `return` inside it — the commit would be skipped. |
| `$store(T, count)` | An in-memory store of `T` holding `count` entries. |
| `$throw(status, message)` | Aborts the request. In a function that returns a value: `$throw(status, message, value)`. |

`$fail`, `$abort` and `$return` are reserved and deliberately fail to compile —
they exist to point you at `$throw`.

**Generated for every `$table(T)`**

```
T_create_table()   T_insert(row) -> new id   T_update(row)   T_save(row)
T_delete(id)       T_find(id)                T_get(id, &out) T_exists(id)
T_all(rows, max)   T_list(limit)             T_page(page, size)
T_at(list, i)      T_apply(current, changes)
```

## Tests

```bash
./eat test
```

Builds and runs the suite in `tests/`. Each file is a standalone binary with no
framework outside `tests/test.h`. Covers the arena (including pointer stability
across chunk growth), the growable buffer, HTTP request parsing (including
malformed input and duplicate `Content-Length`), and the JSON parser.

## Database

SQLite, configured in `app/application.properties`:

```
database.url = gargantua.db
```

The path is relative to the working directory. In the container it is set to
`/data/gargantua.db` through the `DATABASE_URL` environment variable, so the
database lives on the mounted volume and survives rebuilds.

## Features

Parts of the framework are optional. Name the ones you want in
`app/application.properties`:

```
features = fetch, scheduler, templates
```

Leave the line out entirely and you get all of them. Write it empty and you get
none. A feature that is off is neither compiled into the binary nor called from
the generated `main()`, so it costs nothing at runtime.

| feature | what it gives you |
|---|---|
| `fetch` | `$fetch` — the outbound http client |
| `scheduler` | `$repeat` and `$on_start` background tasks |
| `templates` | template rendering |

Dropping `fetch` is the one that shows: it pulls in OpenSSL, whose start-up
allocations dominate the idle footprint.

| features | binary | idle RSS |
|---|---|---|
| all three | 176 kB | 6.6 MB |
| none | 156 kB | **2.1 MB** |

## Container

The image is only the backend — a static musl build in a `scratch` image,
450 kB.

```bash
./deploy.sh
```

Opens at http://localhost:8100 and bind-mounts `./data` for the database.

To build without the UPX packing step:

```bash
podman build --build-arg PACK=0 -t gargantua .
```

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

## License

MIT — see [LICENSE](LICENSE).
