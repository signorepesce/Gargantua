#!/usr/bin/env python3
import contextlib
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def run(args, cwd, expected=0):
    env = dict(os.environ)
    for key in ('APP', 'OUT', 'GARGANTUA_PROJECT', 'GARGANTUA_CONFIG'):
        env.pop(key, None)
    result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, capture_output=True, text=True)
    if (result.returncode == 0) != (expected == 0):
        raise AssertionError(result.stdout + result.stderr)
    return result


def main():
    with tempfile.TemporaryDirectory(prefix='gargantua install ') as folder:
        work = Path(folder)
        prefix = work / 'prefix'
        run([ROOT / 'eat', 'install', prefix], ROOT)
        eat = prefix / 'bin/eat'
        project = work / 'my api'
        run([eat, 'init', project], work)
        run([eat, 'build'], project)
        assert not (project / 'src').exists() and not (project / 'include').exists()
        lock = json.loads((project / 'gargantua.lock').read_text())
        archive = project / '.build' / ('sqlite-' + lock['sqlite']['version'] + '.zip')
        assert archive.is_file()
        env = dict(os.environ, HTTPS_PROXY='http://127.0.0.1:1', https_proxy='http://127.0.0.1:1')
        for key in ('APP', 'OUT', 'GARGANTUA_PROJECT', 'GARGANTUA_CONFIG'):
            env.pop(key, None)
        result = subprocess.run([str(eat), 'build'], cwd=project, env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        with (work / 'server.log').open('w') as log:
            process = subprocess.Popen([str(project / '.build/server')], cwd=project,
                                       env=dict(env, SERVER_PORT=str(port)), stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if process.poll() is not None:
                        raise AssertionError((work / 'server.log').read_text())
                    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=.5)
                    try:
                        connection.request('GET', '/hello')
                        response = connection.getresponse()
                        assert response.status == 200 and response.read() == b'Hello'
                        break
                    except OSError:
                        time.sleep(.05)
                    finally:
                        connection.close()
                else:
                    raise AssertionError('installed app not ready')
            finally:
                process.terminate()
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        backup = archive.read_bytes()
        archive.write_bytes(b'corrupt')
        result = run([eat, 'build'], project, expected=1)
        assert 'checksum mismatch' in result.stderr
        archive.write_bytes(backup)
        props = project / 'app/application.properties'
        props.write_text(props.read_text().replace('features =', 'features = scheduler'))
        result = run([eat, 'build'], project, expected=1)
        assert 'lock mismatch' in result.stderr
        run([eat, 'lock'], project)
        run([eat, 'build'], project)
        props.write_text(props.read_text().replace('framework.version = 0.1.0', 'framework.version = 99.0.0'))
        result = run([eat, 'build'], project, expected=1)
        assert 'install Gargantua 99.0.0' in result.stderr
        print('installation: versioned install, separate project with spaces, verified bundled SQLite, offline rebuild, Hello, corrupt archive rejection, lock drift and version selection passed')


if __name__ == '__main__':
    main()
