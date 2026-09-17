# Gargantua <img width="42" height="42" alt="hat" src="resources/hat.png" />

A small vibe-coded web framework for C. You write annotated structs and plain C
functions; the generator writes the entry point, the routing table, the JSON
binding and the SQLite layer for you.

## Requirements

A C11 compiler (GCC or Clang), Bash and Python 3. Python is only used by the
build tools, never at runtime. SQLite is either bundled (downloaded and verified
on the first build) or the system `libsqlite3`. OpenSSL is optional and enables
https in the outbound client.

## Quick start

```bash
./eat install ~/.local
```

```bash
eat init my-api
```

```bash
cd my-api && eat run
```

`GET http://127.0.0.1:8100/hello` answers `Hello`.

Inside this repository `eat` works on `app/` directly:

    ./eat run        build and start the server
    ./eat dev        rebuild and restart after source changes
    ./eat build      write .build/server
    ./eat clean      remove the owned build directory
    ./eat test       run the test suite
    ./eat version    show the framework version

`eat` is the only build entry point. It compiles the generator, runs it over
the application, then compiles the generated code with the framework into
`.build/server`. `APP=examples/users ./eat run` builds another application and
`OUT=.build-x` writes somewhere else.

## An application

Each file has one role, named by the header it includes:

    app/
      application.properties
      entity/user.c          #include <entity.h>           $table
      dto/user_view.c        #include <dto.h>              $json
      service/users.c        #include <service.h>          plain functions
      controller/users.c     #include <rest_controller.h>  routes

The folder names are a convention; the rules are that models and functions live
in separate files, each model closes with `};`, and every annotation sits alone
on its own line. The full example is in [examples/users](examples/users).

The entity:

```c
#include <entity.h>

$table(User)
{
    $id
    int id;

    $not_null
    str name;

    $not_null
    $unique
    str email;

    $nullable(int) age;
};
```

A DTO that copies some fields and adds its own:

```c
#include <dto.h>

$json(UserView)
{
    $pick(User, id, name, age)
    str greeting;
};
```

A service:

```c
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
}
```

The controller:

```c
#include <rest_controller.h>

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
```

The routes are live:

    GET  /users/1
      -> 200 {"id":1,"name":"Ada","age":36,"greeting":"Hello Ada"}

    POST /users  {"name":"Grace","email":"grace@example.com"}
      -> 201 Location: /users/2
         {"id":2,"name":"Grace","email":"grace@example.com","age":null}

    GET  /users?page=0&size=10
      -> 200 {"items":[{"id":1,...},{"id":2,...}],"page":0,"size":10,"hasMore":false}

    GET  /users/42
      -> 404 {"status":404,"code":"not_found","error":"User not found","fields":[],"truncated":false}

Validation comes from the field annotations and runs before the handler:

    POST /users  {"name":"Grace"}
      -> 400 {"status":400,"code":"validation_failed","error":"request validation failed",
              "fields":[{"field":"email","code":"required","message":"field is required"}],
              "truncated":false}

Models are values, so a DTO needs no `malloc`. Strings live for the current
request; do not keep them in a global or a background task. Raw C, pointers and
the low-level headers stay available for advanced code. Framework names begin
with `$`; your types and functions keep normal C names.

## Annotations

All annotations are lowercase.

**Types**

| | |
|---|---|
| `$table(T)` | Persisted entity. Generates a SQLite table and the data API below. |
| `$json(T)` | JSON-only type, for request and response shapes. No table. |
| `$pick(T, a, b, ...)` | Inside `$json`: copies fields `a`, `b` from `$table(T)` with their validation rules. Generates `$Name_from(T)`. |

Field types are `int`, `long`, `double`, `bool`, `str`, `$nullable(int|long|double|bool)`
or a nested `$table`/`$json` type. A `str` without `$not_null` accepts `null`;
other scalars are never null unless declared `$nullable`.

**Fields** — checked on every request before the handler runs.

| | |
|---|---|
| `$id` | Primary key. `int`, one per table, assigned by the database. |
| `$not_null` | Required. A missing value returns 400 naming the field. |
| `$unique` | Unique across the table. A duplicate returns 409. |
| `$email` | Must look like an email address. |
| `$min(v)` / `$max(v)` | Numeric bounds. |
| `$size(a, b)` | Length bounds for `str`. |
| `$references(T)` | Foreign key to another `$table`. A broken reference returns 409. |

**Routes** — the path is quoted and the annotation is alone on the line above the function.

| | |
|---|---|
| `$get(path)` `$post(path)` `$put(path)` `$patch(path)` `$delete(path)` | Map a function to a method and path. |

Parameters are `int` or `str`. A `{name}` in the path binds the parameter of the
same name; any other `int`/`str` parameter comes from the query string; one
`$table`/`$json` parameter is the JSON body. A route returns a model, `$page(T)`,
`$list(T)`, `int`, `str` or `void`. POST answers 201, `void` answers 204,
everything else 200.

**Security**

| | |
|---|---|
| `$authenticated` | Requires a valid bearer token. |
| `$role(name)` | Requires a role, `admin` or `user`. |
| `$public` | Open even when `security.enabled = true`. |

With `security.enabled = true` every route without `$public` needs a token.
`security.token` grants `user`, `security.admin_token` grants `admin`; each is at
least 32 characters and they must differ.

**Behaviour**

| | |
|---|---|
| `$transactional` | Runs a service function in one transaction; a `$throw` rolls it back. Nested calls use savepoints. |
| `$transaction { }` | An inline transaction block. A `return` inside it is rejected by the generator. |
| `$on_start` | Runs once at startup. `void f(void)`. |
| `$repeat("10s")` | Runs periodically, from `100ms` to `24h`. `void f(void)`. |
| `$store(T, count)` | A bounded in-memory store of `T` that outlives the request. |
| `$produces(type)` | Sets the response content type. |
| `$throw(status, message)` | Ends the request with an error. In a function that returns a value: `$throw(status, message, value)`. |

## Data API

Generated for every `$table(T)`:

```
$T_create_table()          $T_check_schema()
$T_insert(row) -> id       $T_update(row) -> 1       $T_save(row) -> id
$T_find(id)                $T_get(id, &out)          $T_exists(id)
$T_delete(id) -> 1         $T_all(rows, max)         $T_list(limit)
$T_page(page, size)        $T_search(query)          $T_by_<field>(value, page, size)
$T_at(list, i)             $T_apply(current, changes)
```

`$T_save` inserts when the id is `0` and updates otherwise. `$T_find` answers 404
when the row does not exist. A `$references(P)` field `f` also gets
`$T_page_by_f(id, page, size)` and `$T_fetch_f(row)`. `$T_apply` merges a PATCH
body: omitted fields keep their value and `null` clears a nullable one.

Filtering, ordering and paging:

```c
$get("/notes")
$page(Note) list_notes(str title, int page, int size)
{
    return $Note_search($query(
        .filters = { $filter(title, "LIKE", $format("%%%s%%", title)), $filter_not_null(stars) },
        .count = 2,
        .order = "stars",
        .descending = true,
        .page = page,
        .size = size));
}
```

Operators are `=`, `!=`, `<`, `<=`, `>`, `>=` and `LIKE` (for `str`), plus
`$filter_null(f)` and `$filter_not_null(f)`. Up to 8 filters; `size` is 1 to 100
and defaults to 20. Values are always bound, never pasted into SQL, and an
unknown field, operator or type answers 400.

## Helpers

| | |
|---|---|
| `$format(fmt, ...)` `$concat(a, b)` | Build a request-lifetime string. |
| `$location(uri)` `$status(code)` `$response_header(name, value)` | Shape the response. `Location` is dropped from error responses. |
| `$header(name)` | Read a request header. |
| `$has(field)` `$is_null(field)` | Inspect the raw JSON body, e.g. for PATCH. |
| `$some(int, 36)` `$null(int)` | Build a `$nullable` value. |
| `$user_id()` `$require_role(r)` `$require_owner(id)` | Check the caller inside a function. |
| `$eq(a, b)` `$empty(s)` `$now()` `$log(...)` `$warn(...)` | Small utilities. |
| `$fetch(url)` `$fetch_in(url, ms)` `$send(url, type, body)` `$send_json(url, body)` | Outbound http client; `$ok(r)`, `$json_text(r, key)`, `$json_int(r, key, fallback)` read the reply. |
| `$render(name, rows)` | Render a template from `templates.root` with `{{field}}` and `{{#each}}...{{/each}}`. |

## Database

SQLite, configured in `application.properties`:

    database.url        = app.db
    database.migrations = app/migrations

Migrations are `1__schema.sql`, `2__more.sql` and so on, applied in order and
recorded. Editing or deleting an applied migration stops the server at startup.
After migrations every `$table` is compared with the real schema: a missing
column, a different type or nullability, a missing `UNIQUE` or foreign key stops
the server with a message naming the field. `$T_create_table()` is the shortcut
for examples and tests.

In the container `DATABASE_URL=/data/gargantua.db`, so the database lives on
the mounted volume.

## Built-in endpoints

| | |
|---|---|
| `GET /health/live` | The process is up. |
| `GET /health/ready` | The database answers and the server is not shutting down; 503 otherwise. |
| `GET /openapi.json` | OpenAPI 3.1 for every route and model. With `security.enabled = true` it needs a token unless `openapi.public = true`. |

## Features

Parts of the framework are optional:

    features = fetch, scheduler, templates

Leave the line out to get all of them; write it empty to get none. A feature
that is off is not compiled into the binary and not called from the generated
`main()`.

| feature | gives you |
|---|---|
| `fetch` | `$fetch`, `$send` and the outbound http client |
| `scheduler` | `$repeat` and `$on_start` |
| `templates` | `$render` |

## Configuration

`application.properties` is the only configuration file. Every key can be
overridden by the environment: dots become underscores and the name is
upper-cased, so `server.port` is `SERVER_PORT`. `GARGANTUA_CONFIG` points at a
different file.

    server.port                = 8100
    server.address             = 127.0.0.1
    server.workers             = 8
    server.drain_ms            = 0
    server.connections_per_ip  = 16
    server.requests_per_minute = 600

    database.url        = gargantua.db
    database.migrations =

    openapi.public = true

    security.enabled     = false
    security.token       =
    security.admin_token =

    cors.origins     = http://localhost:3000
    cors.headers     = Content-Type, Authorization
    cors.credentials = false

    static.root    =
    static.prefix  = /static
    templates.root =

    fetch.tls        = auto
    fetch.allow      =
    fetch.link_local = deny

## Versions and dependencies

A project made with `eat init` pins the framework and its dependencies:

    framework.version = 0.1.0
    deps.sqlite       = bundled
    deps.packages     =

`eat install PREFIX` installs a versioned copy of the framework under
`PREFIX/lib/gargantua/VERSION` and the `eat` launcher in `PREFIX/bin`; each
project builds with the version it names. `deps.sqlite = bundled` downloads and
verifies the SQLite amalgamation once, then builds offline. `deps.packages` uses
installed pkg-config packages. `eat lock` writes `gargantua.lock`; commit it, and
run `eat lock` again to accept a deliberate change. C files under `vendor/` are
compiled in.

## Deployment

The image is only the backend: a static musl build in a `scratch` image.

```bash
./deploy.sh
```

It opens at http://localhost:8100 and bind-mounts `./data` for the database.
To build without the UPX packing step:

```bash
podman build --build-arg PACK=0 -t gargantua .
```

For a plain server, [examples/deployment](examples/deployment) has a hardened
systemd unit, an nginx front with TLS and rate limits, and production properties.

## Tests

```bash
./eat test
```

Runs, in order:

- unit tests for the arena, configuration, database, growable buffer, HTTP
  parsing, JSON, responses and routing
- generator cases in `tests/cases`, applications that must build or be rejected
  with a given message
- packaging checks for `init`, `install`, `lock` and version pins
- `eat clean` guard checks
- integration tests against `tests/integration_app`: CRUD, projections,
  transactions and rollback, migrations, schema drift, auth, CORS, OpenAPI

Longer runs, also in CI:

```bash
./tests/fuzz.sh 20
```

```bash
python3 tests/load.py --seconds 30
```

```bash
python3 tests/installation.py
```

`fuzz.sh` fuzzes the generator, HTTP and JSON parsers with libFuzzer.
`load.py` measures throughput, latency and memory with and without keep-alive,
checks slowloris clients and graceful shutdown. `installation.py` installs the
framework, builds a project in a path with spaces, rebuilds offline and rejects
a corrupt download. CI runs the suite on GCC/glibc, Clang/macOS, static
GCC/musl, with address and undefined behaviour sanitizers, and builds the
container image.

## Layout

    include/          public headers
    src/core/         arena allocator, growable buffer, configuration
    src/http/         http, json, routing, responses, static files, templates
    src/db/           sqlite, migrations, schema check, search, key-value store
    src/runtime/      request runtime, auth, scheduler, http client
    src/server/       tcp server and worker pool
    src/generator/    the code generator
    tools/            init, install and lock
    app/              your application
    examples/         a users API and deployment files
    tests/            the test suite

## License

MIT — see [LICENSE](LICENSE).
