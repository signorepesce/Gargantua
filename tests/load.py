#!/usr/bin/env python3
import argparse
import concurrent.futures
import http.client
import json
import math
import os
import hashlib
from pathlib import Path
import platform
import socket
import shutil
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent


def percentile(histogram, fraction):
    threshold = math.ceil(sum(histogram) * fraction)
    total = 0
    for index, count in enumerate(histogram):
        total += count
        if total >= threshold:
            return round((index + 1) / 10, 1)
    return None


def health(port):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=1)
    try:
        connection.request('GET', '/health/ready', headers={'Connection': 'close'})
        response = connection.getresponse()
        response.read()
        return response.status == 200
    finally:
        connection.close()


def workload(port, seconds, concurrency, keep_alive, process, close_rate):
    stop_at = time.monotonic() + seconds
    barrier = threading.Barrier(concurrency)

    def worker(index):
        histogram = [0] * 100001
        errors = {}
        statuses = {}
        slowest = 0
        count = 0
        connection = None
        barrier.wait(timeout=10)
        while time.monotonic() < stop_at:
            started = time.perf_counter()
            try:
                if connection is None:
                    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
                path = ['/hello', '/users/1', '/users?page=0&size=10'][(count + index) % 3]
                connection.request('GET', path, headers={'Connection': 'keep-alive' if keep_alive else 'close'})
                response = connection.getresponse()
                data = response.read()
                statuses[str(response.status)] = statuses.get(str(response.status), 0) + 1
                if response.status != 200:
                    raise ValueError('HTTP ' + str(response.status))
                if path == '/hello':
                    assert data == b'Hello'
                elif path == '/users/1':
                    body = json.loads(data)
                    assert body['name'] == 'Ada' and 'email' not in body and body['age'] == 36
                else:
                    body = json.loads(data)
                    assert body['items'][0]['name'] == 'Ada'
                if response.will_close or not keep_alive:
                    connection.close()
                    connection = None
            except (OSError, http.client.HTTPException, ValueError, AssertionError) as error:
                label = type(error).__name__ + ': ' + str(error)
                errors[label] = errors.get(label, 0) + 1
                if connection is not None:
                    connection.close()
                connection = None
                time.sleep(.01)
            elapsed = (time.perf_counter() - started) * 1000
            histogram[min(100000, int(elapsed * 10))] += 1
            slowest = max(slowest, elapsed)
            count += 1
            if not keep_alive:
                remaining = concurrency / close_rate - (time.perf_counter() - started)
                if remaining > 0:
                    time.sleep(remaining)
        if connection is not None:
            connection.close()
        return histogram, errors, statuses, count, slowest

    rss = []
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(worker, i) for i in range(concurrency)]
        while not all(future.done() for future in futures):
            if process.poll() is not None:
                raise AssertionError('server exited during load')
            probe = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            try:
                probe.request('GET', '/benchmark/rss', headers={'Connection': 'close'})
                response = probe.getresponse()
                data = response.read()
                value = int(data) if response.status == 200 else -1
            finally:
                probe.close()
            rss.append({'seconds': round(time.monotonic() - started, 2), 'kib': int(value)})
            time.sleep(.5)
        results = [future.result() for future in futures]
    elapsed = time.monotonic() - started
    histogram = [sum(result[0][i] for result in results) for i in range(100001)]
    errors, statuses = {}, {}
    for _, failure, status, _, _ in results:
        for key, value in failure.items():
            errors[key] = errors.get(key, 0) + value
        for key, value in status.items():
            statuses[key] = statuses.get(key, 0) + value
    counts = sum(result[3] for result in results)
    warm = [point['kib'] for point in rss if point['seconds'] >= min(5, seconds / 4)]
    return {'mode': 'keep-alive' if keep_alive else 'close', 'concurrency': concurrency, 'rate_limit': None if keep_alive else close_rate,
            'seconds': round(elapsed, 2), 'requests': counts, 'requests_per_second': round(counts / elapsed, 1),
            'statuses': statuses, 'errors': errors, 'p50_ms': percentile(histogram, .50),
            'p95_ms': percentile(histogram, .95), 'p99_ms': percentile(histogram, .99),
            'max_ms': round(max(result[4] for result in results), 2),
            'rss_peak_kib': max(point['kib'] for point in rss),
            'rss_warm_growth_kib': warm[-1] - warm[0] if warm else None, 'rss_samples': rss}


def slowloris(port, process):
    connection = socket.create_connection(('127.0.0.1', port), timeout=2)
    connection.sendall(b'GET /hello HTTP/1.1\r\nHost: localhost\r\nX-Slow: ')
    started = time.monotonic()
    closed = False
    try:
        for _ in range(16):
            connection.settimeout(.3)
            try:
                data = connection.recv(1)
                if not data:
                    closed = True
                    break
            except socket.timeout:
                pass
            try:
                connection.sendall(b'a')
            except OSError:
                closed = True
                break
            time.sleep(.2)
    finally:
        connection.close()
    elapsed = time.monotonic() - started
    assert closed and elapsed < 7, ('slowloris deadline', elapsed, closed)
    assert process.poll() is None and health(port)
    return round(elapsed, 2)


def main():
    parser = argparse.ArgumentParser(description='Local closed-loop load/soak test: Hello, SQLite page and projected DTO.')
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--concurrency', type=int, default=4)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--close-rate', type=int, default=100)
    parser.add_argument('--output', type=Path, default=Path('/tmp/gargantua-load.json'))
    args = parser.parse_args()
    if not 2 <= args.seconds <= 3600 or not 1 <= args.concurrency <= 64 or not 1 <= args.close_rate <= 10000:
        parser.error('seconds: 2..3600; concurrency: 1..64')
    if args.binary is None:
        with tempfile.TemporaryDirectory(prefix='gargantua-load-app-') as folder:
            source = Path(folder) / 'app'
            shutil.copytree(ROOT / 'examples/users', source)
            shutil.copy2(ROOT / 'tests/load_probe.c', source / 'probe.c')
            subprocess.run([str(ROOT / 'eat'), 'build'], cwd=ROOT,
                           env=dict(os.environ, APP=str(source), OUT='.build-load'), check=True)
    binary = (args.binary or ROOT / '.build-load/server').resolve()
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='gargantua-load-') as temp:
        with (Path(temp) / 'server.log').open('w') as log:
            process = subprocess.Popen([str(binary)], cwd=ROOT, stdout=log, stderr=log,
                                       env=dict(os.environ, SERVER_PORT=str(port), DATABASE_URL=':memory:',
                                                GARGANTUA_CONFIG=str(ROOT / 'examples/users/application.properties')))
            try:
                for _ in range(100):
                    if process.poll() is not None:
                        raise AssertionError((Path(temp) / 'server.log').read_text())
                    try:
                        if health(port):
                            break
                    except OSError:
                        pass
                    time.sleep(.05)
                else:
                    raise AssertionError('server not ready')
                report = {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'platform': platform.platform(), 'python': platform.python_version(),
                          'compiler': subprocess.check_output([os.environ.get('CC', 'cc'), '--version'], text=True).splitlines()[0],
                          'flags': os.environ.get('EXTRA_CFLAGS', '-O2'),
                          'workload': 'equal mix: Hello, SQLite page, projected DTO; Python closed-loop on loopback; logging enabled',
                          'percentile_resolution_ms': .1, 'runs': []}
                for keep in [False, True]:
                    report['runs'].append(workload(port, args.seconds, args.concurrency, keep, process, args.close_rate))
                args.output.write_text(json.dumps(report, indent=2) + '\n')
                report['slowloris_closed_seconds'] = slowloris(port, process)
                idle = [socket.create_connection(('127.0.0.1', port), timeout=2) for _ in range(8)]
                time.sleep(.1)
                started = time.monotonic()
                process.terminate()
                time.sleep(.15)
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.3):
                        raise AssertionError('still accepts new connections during shutdown')
                except (ConnectionRefusedError, socket.timeout):
                    pass
                process.wait(timeout=3)
                for sock in idle:
                    sock.close()
                report['shutdown_with_idle_clients_seconds'] = round(time.monotonic() - started, 2)
                report['exit_status'] = process.returncode
                args.output.write_text(json.dumps(report, indent=2) + '\n')
                print(json.dumps({**report, 'runs': [{k: v for k, v in run.items() if k != 'rss_samples'} for run in report['runs']]}, indent=2))
                assert process.returncode == 0
                assert all(not run['errors'] for run in report['runs']), 'load errors; see report'
                assert all(run['requests'] >= 10 for run in report['runs']), 'insufficient requests'
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=4)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == '__main__':
    main()
