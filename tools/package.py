#!/usr/bin/env python3
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

FRAMEWORK = Path(__file__).resolve().parent.parent
VERSION = re.search(r'GARGANTUA_VERSION "([^"]+)"', (FRAMEWORK / 'include/version.h').read_text())[1]
CATALOG = json.loads((FRAMEWORK / 'tools/dependencies.json').read_text())
ASSETS = ('eat', 'include', 'src', 'tools', 'LICENSE', 'examples')
HELP = '''Gargantua: small C applications, generated routing and SQLite CRUD.

  eat init my-api       Create a separate Hello application with pinned SQLite.
  cd my-api
  eat run              Build and run; server.port is in app/application.properties.
  eat dev              Rebuild after source changes.
  eat build            Write .build/server, a normal executable.
  eat clean            Remove only the owned build directory.
  eat lock             Explicitly accept dependency / framework changes.
  eat install [PREFIX] Install versioned framework and PREFIX/bin/eat.
  eat version          Show framework version.
  eat test             Run framework tests from its repository.

Requires a C11 GCC/Clang compiler, Bash and Python 3 (build tools only).
New projects download verified SQLite sources on the first build. Later builds
work offline. Optional deps.packages uses installed pkg-config packages;
eat lock pins their versions and flags on this OS/architecture. It does not
install those system packages or promise identical binaries across compilers.
Commit gargantua.lock with your application. Never edit it to bypass a mismatch.

app/application.properties:
  framework.version = VERSION
  deps.sqlite = bundled
  features =
  fetch.tls = off
  server.port = 8100
  database.url = app.db

Put $table(User) in app/entity/user.c, $json(UserView) in app/dto/user_view.c,
and normal service functions in app/service/. Each model closes with };.
Inside $json: $pick(User, id, name), then any extra fields. Convert with
$UserView_from(user); extra fields start at zero. Strings live for the current
request; do not retain them in a global or a background task. Use $store for
bounded persistent copies. Models are values, so no malloc is needed for a DTO.
CRUD: $User_find(id), $User_save(user), $User_page(page, size), $User_search(query).
Use $format("Hello %s", name) and $response_header("Location", value).
Raw C, pointers and low-level headers remain available for advanced code.
API names are case sensitive. Framework annotations/functions begin with $;
C types and application functions keep their normal C names.

Deployment examples: examples/deployment/ (nginx HTTPS, systemd, properties).
The app binds loopback. nginx handles public TLS and per-client rate limits.
Forwarded headers are not treated as authenticated client identity by Gargantua.
Use an unprivileged service account. Set an actual database path and migrations.
'''.replace('VERSION', VERSION)


def fail(message):
    raise ValueError(message)


def properties(path):
    result = {}
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.split('#', 1)[0].strip()
            if line and '=' in line:
                key, value = line.split('=', 1)
                result[key.strip()] = value.strip()
    return result


def digest_file(path):
    if path.is_symlink():
        fail(f'symlink dependency is not supported: {path}')
    return hashlib.sha256(path.read_bytes()).hexdigest()


def asset_files(root):
    for name in ASSETS:
        entry = root / name
        if entry.is_file():
            yield entry
        elif entry.is_dir():
            yield from sorted(p for p in entry.rglob('*') if p.is_file() and '__pycache__' not in p.parts)


def framework_hash(root=FRAMEWORK):
    digest = hashlib.sha256()
    for path in asset_files(root):
        digest.update(str(path.relative_to(root)).encode() + b'\0')
        digest.update(digest_file(path).encode() + b'\0')
    return digest.hexdigest()


def command(args):
    return subprocess.check_output(args, text=True).strip()


def snapshot(props):
    version = props.get('framework.version', VERSION)
    if version != VERSION:
        fail(f'project needs Gargantua {version}; running {VERSION}')
    sqlite = props.get('deps.sqlite', 'system')
    if sqlite not in ('system', 'bundled'):
        fail('deps.sqlite must be bundled or system')
    packages = props.get('deps.packages', '').split()
    features = props.get('features', 'fetch scheduler templates').replace(',', ' ').split()
    tls = props.get('fetch.tls', 'auto')
    if 'fetch' in features and tls != 'off':
        if shutil.which('pkg-config') and subprocess.run(['pkg-config', '--exists', 'openssl']).returncode == 0:
            packages.append('openssl')
        elif tls == 'on':
            fail('fetch.tls=on needs OpenSSL via pkg-config')
    if sqlite == 'system':
        packages.append('sqlite3')
    resolved = {}
    for name in sorted(set(packages)):
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.+-]{0,127}', name):
            fail(f'invalid package name: {name}')
        resolved[name] = {key: command(['pkg-config', '--' + flag, name])
                          for key, flag in [('version', 'modversion'), ('cflags', 'cflags'), ('libs', 'libs')]}
    vendor = {}
    if Path('vendor').is_dir():
        for path in sorted(Path('vendor').rglob('*')):
            if path.is_symlink():
                fail(f'symlink dependency is not supported: {path}')
            if path.is_file():
                vendor[str(path)] = digest_file(path)
    unmanaged = {key: props.get(key, '') for key in ('deps.libs', 'deps.include', 'deps.libpath')}
    if any(unmanaged.values()):
        fail('locked builds require deps.packages or vendored sources, not unversioned deps.libs/include/libpath')
    return {'format': 1, 'framework': {'version': VERSION, 'sha256': framework_hash()},
            'sqlite': CATALOG['sqlite'] if sqlite == 'bundled' else 'system',
            'features': sorted(set(features)), 'fetch.tls': tls, 'vendor': vendor,
            'platform': platform.system() + '-' + platform.machine() if resolved else None,
            'packages': resolved}


def atomic_write(path, data):
    path = Path(path)
    if path.is_symlink():
        fail(f'refusing symlink: {path}')
    fd, temp = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(data)
        os.replace(temp, path)
    finally:
        if Path(temp).exists():
            Path(temp).unlink()


def lock(props):
    atomic_write('gargantua.lock', (json.dumps(snapshot(props), indent=2) + '\n').encode())


def sqlite_sources(out):
    spec = CATALOG['sqlite']
    archive = out / ('sqlite-' + spec['version'] + '.zip')
    if archive.is_symlink():
        fail('refusing symlink SQLite archive')
    if not archive.exists():
        print('  downloading SQLite ' + spec['version'], flush=True)
        with urllib.request.urlopen(spec['url'], timeout=30) as response:
            if response.geturl() != spec['url']:
                fail('unexpected SQLite redirect')
            data = response.read(8 * 1024 * 1024 + 1)
        if len(data) > 8 * 1024 * 1024 or hashlib.sha3_256(data).hexdigest() != spec['sha3_256']:
            fail('SQLite download checksum mismatch')
        atomic_write(archive, data)
    if archive.stat().st_size > 8 * 1024 * 1024:
        fail('cached SQLite archive exceeds size limit')
    data = archive.read_bytes()
    if hashlib.sha3_256(data).hexdigest() != spec['sha3_256']:
        fail('cached SQLite checksum mismatch; remove the corrupted archive and rebuild')
    source_dir = out / 'sqlite'
    if source_dir.is_symlink():
        fail('refusing symlink SQLite directory')
    source_dir.mkdir(exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as source:
        for name in ('sqlite3.c', 'sqlite3.h'):
            info = source.getinfo(spec['directory'] + '/' + name)
            if info.file_size > 16 * 1024 * 1024:
                fail('SQLite source exceeds size limit')
            expected = source.read(info)
            target = source_dir / name
            if not target.exists() or target.read_bytes() != expected:
                atomic_write(target, expected)
            elif target.is_symlink():
                fail('refusing symlink SQLite source')
    return source_dir


def prepare(app, out):
    props = properties(Path(app) / 'application.properties')
    pinned = 'framework.version' in props
    if Path('gargantua.lock').exists():
        wanted = snapshot(props)
        if json.loads(Path('gargantua.lock').read_text()) != wanted:
            fail('gargantua.lock mismatch: dependencies or framework changed; review changes, then run eat lock')
    elif pinned:
        fail('missing gargantua.lock; run eat lock')
    cflags, libs = [], ['-lsqlite3']
    if Path('gargantua.lock').exists() and props.get('deps.sqlite', 'system') == 'system':
        sqlite = wanted['packages']['sqlite3']
        cflags = shlex.split(sqlite['cflags'])
        libs = shlex.split(sqlite['libs'])
    if props.get('deps.sqlite', 'system') == 'bundled':
        directory = sqlite_sources(out)
        obj = out / 'sqlite.o'
        if obj.is_symlink():
            fail('refusing symlink SQLite object')
        compiler = os.environ.get('CC', 'cc')
        flags = ['-std=c11', '-O2', '-pthread', '-DSQLITE_THREADSAFE=1', '-DSQLITE_DQS=0',
                 '-DSQLITE_DEFAULT_MEMSTATUS=0', '-DSQLITE_OMIT_LOAD_EXTENSION',
                 '-DSQLITE_MAX_LENGTH=16777216', '-DSQLITE_MAX_SQL_LENGTH=1048576',
                 '-DSQLITE_MAX_VARIABLE_NUMBER=1024', '-fstack-protector-strong']
        flags += shlex.split(os.environ.get('EXTRA_CFLAGS', ''))
        key = hashlib.sha256((command([compiler, '--version']) + json.dumps(flags) + CATALOG['sqlite']['sha3_256']).encode()).hexdigest()
        cached = out / 'sqlite.object.sha256'
        if not obj.exists() or not cached.exists() or cached.read_text() != key + ':' + digest_file(obj):
            subprocess.run([compiler, *flags, '-c', str(directory / 'sqlite3.c'), '-o', str(obj)], check=True)
            atomic_write(cached, (key + ':' + digest_file(obj)).encode())
        cflags = ['-I' + str(directory)]
        libs = [str(obj), '-lm']
        if platform.system() == 'Linux':
            libs.append('-ldl')
    atomic_write(out / 'deps.cflags', b''.join(arg.encode() + b'\0' for arg in cflags))
    atomic_write(out / 'deps.ldflags', b''.join(arg.encode() + b'\0' for arg in libs))


LAUNCHER = '''#!/usr/bin/env python3
import os
from pathlib import Path
import re
import sys
root = Path(__file__).resolve().parent.parent / 'lib/gargantua'
versions = sorted((p for p in root.iterdir() if re.fullmatch(r"[0-9]+\\.[0-9]+\\.[0-9]+", p.name)), key=lambda p: tuple(map(int, p.name.split('.'))))
version = versions[-1].name
props = Path('app/application.properties')
if props.exists():
    for line in props.read_text().splitlines():
        if '=' in line and line.split('=', 1)[0].strip() == 'framework.version':
            version = line.split('=', 1)[1].split('#', 1)[0].strip()
if not re.fullmatch(r"[0-9]+\\.[0-9]+\\.[0-9]+", version):
    sys.exit('invalid framework.version')
runner = root / version / 'eat'
if not runner.exists():
    sys.exit('install Gargantua ' + version + ' first')
os.environ['GARGANTUA_PROJECT'] = str(Path.cwd())
os.execv(str(runner), [str(runner), *sys.argv[1:]])
'''


def install(prefix):
    prefix = Path(prefix).expanduser().absolute()
    target = prefix / 'lib/gargantua' / VERSION
    launcher = prefix / 'bin/eat'
    if launcher.exists() and (launcher.is_symlink() or launcher.read_text() != LAUNCHER):
        fail(f'refusing to overwrite {launcher}')
    if target.exists():
        if target.is_symlink() or framework_hash(target) != framework_hash():
            fail(f'{VERSION} already installed with different contents; use a new version/prefix')
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix='.install-', dir=target.parent))
        try:
            for name in ASSETS:
                path = FRAMEWORK / name
                if path.is_dir():
                    shutil.copytree(path, staging / name, ignore=shutil.ignore_patterns('__pycache__'))
                else:
                    shutil.copy2(path, staging / name)
            staging.rename(target)
        finally:
            if staging.exists():
                shutil.rmtree(staging)
    launcher.parent.mkdir(parents=True, exist_ok=True)
    atomic_write(launcher, LAUNCHER.encode())
    launcher.chmod(0o755)
    print(f'Installed Gargantua {VERSION}. Add {prefix / "bin"} to PATH.')


def init(path):
    target = Path(path).absolute()
    if target.exists():
        fail(f'refusing to overwrite existing project: {target}')
    target.mkdir(parents=True)
    app = target / 'app'
    app.mkdir()
    (app / 'hello.c').write_text('#include <rest_controller.h>\n\n$get("/hello")\nstr hello(void)\n{\n    return "Hello";\n}\n')
    (app / 'application.properties').write_text(f'framework.version = {VERSION}\ndeps.sqlite = bundled\nfeatures =\nfetch.tls = off\nserver.port = 8100\nserver.address = 127.0.0.1\ndatabase.url = app.db\n')
    (target / '.gitignore').write_text('.build*/\n*.db\n*.db-shm\n*.db-wal\n')
    previous = Path.cwd()
    try:
        os.chdir(target)
        lock(properties(app / 'application.properties'))
    finally:
        os.chdir(previous)
    print(f'Created {target}\n  cd {shlex.quote(str(target))}\n  eat run\n  GET http://127.0.0.1:8100/hello')


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('action', choices=['init', 'install', 'version', 'help', 'lock', 'prepare'])
    parser.add_argument('args', nargs='*')
    options = parser.parse_args()
    expected = {'init': (1, 1), 'install': (0, 1), 'version': (0, 0), 'help': (0, 0), 'lock': (0, 0), 'prepare': (2, 2)}
    low, high = expected[options.action]
    if not low <= len(options.args) <= high:
        fail('wrong arguments; run eat help')
    if options.action == 'help':
        print(HELP)
    elif options.action == 'version':
        print('Gargantua ' + VERSION)
    elif options.action == 'install':
        install(options.args[0] if options.args else '~/.local')
    elif options.action == 'init':
        init(options.args[0])
    elif options.action == 'lock':
        lock(properties(Path(os.environ.get('APP', 'app')) / 'application.properties'))
        print('Updated gargantua.lock; review and commit it.')
    else:
        prepare(options.args[0], Path(options.args[1]))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError, KeyError, zipfile.BadZipFile) as error:
        print('gargantua: ' + str(error), file=sys.stderr)
        sys.exit(1)
