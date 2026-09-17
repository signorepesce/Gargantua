#!/usr/bin/env python3
import importlib.util
import json
import os
from pathlib import Path
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('gargantua_package', ROOT / 'tools/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def rejects(call, text):
    try:
        call()
    except ValueError as error:
        assert text in str(error), str(error)
    else:
        raise AssertionError('accepted: ' + text)


previous = Path.cwd()
try:
    with tempfile.TemporaryDirectory(prefix='gargantua-package-') as temp:
        root = Path(temp)
        os.chdir(root)
        package.init('my api')
        app = root / 'my api'
        assert not (app / 'src').exists() and not (app / 'include').exists()
        assert '$get("/hello")' in (app / 'app/hello.c').read_text()
        rejects(lambda: package.init('my api'), 'overwrite')
        os.chdir(app)
        props = package.properties(Path('app/application.properties'))
        original = json.loads(Path('gargantua.lock').read_text())
        assert original == package.snapshot(props)
        Path('vendor').mkdir()
        Path('vendor/math.c').write_text('int square(int n) { return n * n; }')
        assert original != package.snapshot(props)
        package.lock(props)
        assert json.loads(Path('gargantua.lock').read_text()) == package.snapshot(props)
        Path('vendor/link.c').symlink_to('math.c')
        rejects(lambda: package.snapshot(props), 'symlink')
        Path('vendor/link.c').unlink()
        rejects(lambda: package.snapshot(dict(props, **{'framework.version': '99.0.0'})), 'needs Gargantua')
        rejects(lambda: package.snapshot(dict(props, **{'deps.libs': 'm'})), 'unversioned')
        out = Path('.out'); out.mkdir()
        archive = out / ('sqlite-' + package.CATALOG['sqlite']['version'] + '.zip')
        archive.write_bytes(b'corrupted')
        rejects(lambda: package.sqlite_sources(out), 'checksum mismatch')
        archive.unlink()
        archive.symlink_to(Path('gargantua.lock').absolute())
        rejects(lambda: package.sqlite_sources(out), 'symlink')
        archive.unlink()
        original_framework = package.FRAMEWORK
        minimal = root / 'framework'; minimal.mkdir()
        for name in package.ASSETS:
            if name in ('eat', 'LICENSE'):
                (minimal / name).write_text('sample')
            else:
                (minimal / name).mkdir()
        with patch.object(package, 'FRAMEWORK', minimal), patch.object(package, 'framework_hash', return_value='same'):
            package.install(root / 'prefix')
            package.install(root / 'prefix')
        (root / 'prefix/bin/eat').write_text('preserve me')
        rejects(lambda: package.install(root / 'prefix'), 'overwrite')
        assert (root / 'prefix/bin/eat').read_text() == 'preserve me'
    print('  packaging: init, lock drift, version pin, checksum, symlinks and install ownership passed')
finally:
    os.chdir(previous)
