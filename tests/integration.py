#!/usr/bin/env python3
import concurrent.futures
import contextlib
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def run(args, **kwargs):
    return subprocess.run(args, cwd=ROOT, text=True, capture_output=True, **kwargs)


@contextlib.contextmanager
def server(binary, db, migrations, log):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    env = dict(os.environ, SERVER_PORT=str(port), DATABASE_URL=str(db),
               DATABASE_MIGRATIONS=str(migrations))
    with log.open("w") as output:
        process = subprocess.Popen([str(binary)], cwd=ROOT, env=env,
                                   stdout=output, stderr=subprocess.STDOUT)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise AssertionError("startup failed:\n" + log.read_text())
                try:
                    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=.2)
                    connection.request("GET", "/health/ready")
                    response = connection.getresponse()
                    response.read()
                    connection.close()
                    if response.status == 200:
                        break
                except OSError:
                    pass
                time.sleep(.05)
            else:
                raise AssertionError("startup timeout")
            yield port
        finally:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def request(port, method, path, body=None, token=None, headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    h = dict(headers or {})
    if token:
        h["Authorization"] = "Bearer " + token
    if body is not None:
        h["Content-Type"] = "application/json"
    try:
        connection.request(method, path, json.dumps(body) if body is not None else None, h)
        response = connection.getresponse()
        data = response.read()
        parsed = json.loads(data) if data and "application/json" in response.getheader("Content-Type", "") else data.decode()
        return response.status, parsed, dict(response.getheaders())
    finally:
        connection.close()


def exercises(binary, folder):
    db = folder / "app.db"
    migrations = ROOT / "tests/integration_app/migrations"
    with server(binary, db, migrations, folder / "server.log") as port:
        status, data, _ = request(port, "GET", "/total")
        check(status == 200 and data == 7, "ordinary services, whitespace signatures and static helpers")
        check(request(port, "GET", "/void")[0] == 204, "void transactional service")
        check(request(port, "POST", "/inline")[0] == 204, "nested inline transaction and string tokens")
        check(request(port, "GET", "/protected")[0] == 401, "missing credentials")
        check(request(port, "GET", "/protected", token="1" * 32)[0] == 200, "valid credentials")
        check(request(port, "GET", "/admin", token="1" * 32)[0] == 403, "wrong role")
        check(request(port, "GET", "/admin", token="2" * 32)[0] == 200, "admin role")
        status, department, _ = request(port, "POST", "/departments", {"name": "engineering"})
        check(status == 201, "create parent")
        status, person, headers = request(port, "POST", "/people", {
            "name": "Ada", "age": 0, "score": 0, "weight": 0.0,
            "active": False, "department_id": department["id"]})
        check(status == 201 and person["age"] == 0 and person["active"] is False, "zero and false preserved")
        check(headers.get("Location") == f'/people/{person["id"]}', "Location")
        ident = person["id"]
        check(request(port, "GET", f"/people/{ident}")[1] == person, "SQLite roundtrip")
        status, view, _ = request(port, "GET", f"/person-view/{ident}")
        check(status == 200 and view == {"id": ident, "name": "Ada", "age": 0, "label": "Ada profile", "extra": 0}, "projected DTO, copied optional zero, extra fields, hidden fields")
        check(request(port, "PATCH", f"/people/{ident}", {"age": None})[1]["age"] is None, "PATCH null")
        check(request(port, "PATCH", f"/people/{ident}", {"weight": 2.5})[1]["age"] is None, "PATCH omission")
        check(request(port, "GET", f"/person-view/{ident}")[1]["age"] is None, "DTO projection preserves null")
        check(request(port, "PATCH", f"/people/{ident}", {"name": None})[0] == 400, "required null rejected")
        check(request(port, "PATCH", f"/people/{ident}", {"age": -1})[0] == 400, "nullable validation")
        check(request(port, "PATCH", f"/people/{ident}", {"age": "bad"})[0] == 400, "nullable type validation")
        status, _, headers = request(port, "POST", "/people", {"name": "Ada"})
        check(status == 409 and "Location" not in headers, "UNIQUE conflict without Location")
        status, _, headers = request(port, "POST", "/people", {"name": "invalid-parent", "department_id": 999})
        check(status == 409 and "Location" not in headers, "FK insert without Location")
        check(request(port, "DELETE", f'/departments/{department["id"]}')[0] == 409, "FK delete")
        status, summary, _ = request(port, "GET", f"/summary/{ident}")
        check(status == 200 and summary["department"] == department and summary["extra"], "nested DTO and relationship")
        check(request(port, "POST", "/rollback")[0] == 409, "nested service throws")
        check(request(port, "GET", "/named?name=rollback")[1] == [], "nested inserts rolled back")
        check(request(port, "POST", "/commit", {"name": "committed", "age": 30})[0] == 201, "nested commit")
        check(len(request(port, "GET", "/named?name=committed")[1]) == 1, "committed row visible")
        check(request(port, "GET", "/people?order=name%22%3BDELETE%20FROM%20Person--")[0] == 400, "sort injection rejected")
        check(request(port, "GET", "/named?name=%27%20OR%201%3D1--")[1] == [], "filter values bound")
        check(request(port, "GET", "/people?page=-1")[0] == 400, "negative page")
        check(request(port, "GET", "/people?size=101")[0] == 400, "page size bounded")
        check(len(request(port, "GET", "/unknown-age")[1]) == 1, "IS NULL query")
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            results = list(pool.map(lambda i: request(port, "POST", "/people", {"name": f"parallel-{i}", "age": 20 + i}), range(18)))
        check(all(result[0] == 201 for result in results), "concurrent transactions")
        ids = [result[1]["id"] for result in results]
        check(len(set(ids)) == 18, "unique inserted ids across threads")
        page = request(port, "GET", "/adults")[1]
        check(page["hasMore"] is True, "pagination lookahead")
        check([p["age"] for p in page["items"]] == [37, 36], "numeric descending order")
        check(request(port, "HEAD", f"/people/{ident}")[1] == "", "HEAD no body")
        check(request(port, "OPTIONS", "/people", headers={
            "Origin": "http://localhost:3000", "Access-Control-Request-Method": "POST"})[0] == 204, "CORS preflight")
        document = request(port, "GET", "/openapi.json")[1]
        check("null" in document["components"]["schemas"]["Person"]["properties"]["age"]["type"], "nullable OpenAPI")
        check(document["components"]["schemas"]["Person"]["properties"]["id"].get("readOnly") is True, "read-only id OpenAPI")
        check("204" in document["paths"]["/void"]["get"]["responses"], "void OpenAPI")
    with server(binary, db, migrations, folder / "restart.log") as port:
        check(len(request(port, "GET", "/named?name=committed")[1]) == 1, "migration replay and persistence")
    altered = folder / "changed"
    shutil.copytree(migrations, altered)
    with (altered / "1__schema.sql").open("a") as f:
        f.write("\nSELECT 1;\n")
    for directory in [altered, folder / "missing"]:
        env = dict(os.environ, DATABASE_URL=str(db), DATABASE_MIGRATIONS=str(directory))
        p = run([str(binary)], env=env, timeout=8)
        check(p.returncode != 0, "changed or missing migrations fail startup")


def schema_cases(binary, folder):
    sql = (ROOT / "tests/integration_app/migrations/1__schema.sql").read_text()
    cases = [
        ("missing-column", sql.replace("    age INTEGER,\n", "")),
        ("wrong-type", sql.replace("age INTEGER", "age TEXT")),
        ("wrong-nullability", sql.replace("age INTEGER", "age INTEGER NOT NULL")),
        ("missing-unique", sql.replace("name TEXT NOT NULL UNIQUE", "name TEXT NOT NULL")),
        ("missing-reference", sql.replace(" REFERENCES Department(id)", "")),
        ("wrong-primary-key", sql.replace("id INTEGER PRIMARY KEY", "id BIGINT PRIMARY KEY")),
        ("descending-primary-key", sql.replace("id INTEGER PRIMARY KEY", "id INTEGER PRIMARY KEY DESC")),
        ("extra-required", sql.replace("    age INTEGER,", "    hidden TEXT NOT NULL, age INTEGER,")),
    ]
    for name, definition in cases:
        db = folder / (name + ".db")
        with sqlite3.connect(db) as connection:
            connection.executescript(definition)
        p = run([str(binary)], env=dict(os.environ, DATABASE_URL=str(db), DATABASE_MIGRATIONS=""), timeout=8)
        check(p.returncode == 1 and "schema " in p.stderr, name + ": " + p.stderr)
    compatible = folder / "compatible.db"
    with sqlite3.connect(compatible) as connection:
        connection.executescript(sql.replace("    age INTEGER,", "    extra TEXT, age INTEGER,"))
    with server(binary, compatible, "", folder / "compatible.log") as port:
        check(request(port, "POST", "/people", {"name": "existing"})[0] == 201,
              "existing compatible database with extra nullable column")

def startup_case(folder):
    app = folder / "startup-app"
    shutil.copytree(ROOT / "tests/integration_app", app)
    properties = app / "application.properties"
    properties.write_text(properties.read_text().replace("features =", "features = scheduler"))
    (app / "startup.c").write_text("""#include <gargantua.h>
#include <stdatomic.h>

static _Atomic int ticks;

$on_start
void create_tables(void)
{
    if ($Department_create_table() != 0 || $Person_create_table() != 0)
    {
        $throw(500, "startup schema creation failed");
    }
    $log("%s", str_format("startup %d", 1));
}

$repeat("100ms")
void count_tick(void)
{
    ticks++;
}

$get("/ticks")
int tick_count(void)
{
    return ticks;
}
""")
    out = ".build-startup-" + uuid.uuid4().hex[:12]
    try:
        p = run(["./eat", "build"], env=dict(os.environ, APP=str(app), OUT=out))
        check(p.returncode == 0, "startup build: " + p.stdout + p.stderr)
        with server(ROOT / out / "server", folder / "startup.db", "", folder / "startup.log") as port:
            check(request(port, "POST", "/people", {"name": "from-startup"})[0] == 201,
                  "on_start creates schema before validation")
            check(request(port, "GET", "/ticks")[1] > 0, "periodic tasks started after validation")
    finally:
        run(["./eat", "clean"], env=dict(os.environ, OUT=out))

def main():
    out = ".build-integration-" + uuid.uuid4().hex[:12]
    try:
        built = run(["./eat", "build"], env=dict(os.environ, APP="tests/integration_app", OUT=out))
        check(built.returncode == 0, "integration build:\n" + built.stdout + built.stderr)
        with tempfile.TemporaryDirectory(prefix="gargantua-integration-") as temp:
            exercises(ROOT / out / "server", Path(temp))
            schema_cases(ROOT / out / "server", Path(temp))
            startup_case(Path(temp))
        print(f"  integration: {checks} checks passed")
    finally:
        run(["./eat", "clean"], env=dict(os.environ, OUT=out))


if __name__ == "__main__":
    main()
