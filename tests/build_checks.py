#!/usr/bin/env python3
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix="gargantua-clean-") as name:
    root = Path(name)
    shutil.copy2(ROOT / "eat", root / "eat")
    for dirname in ["src", "app", "include", "tests", ".git", "unowned"]:
        (root / dirname).mkdir()
        sentinel = root / dirname / "sentinel.c"
        sentinel.write_text("preserve")
        result = subprocess.run(["./eat", "clean"], cwd=root, env=dict(os.environ, OUT=dirname), capture_output=True)
        assert result.returncode != 0 and sentinel.exists(), dirname
    (root / "linked").symlink_to(root / "src")
    for value in ["linked", "linked/nested", "../", "/", ".", ""]:
        result = subprocess.run(["./eat", "clean"], cwd=root, env=dict(os.environ, OUT=value), capture_output=True)
        if value:
            assert result.returncode != 0, value
        assert (root / "src/sentinel.c").exists()
    owned = root / ".owned"
    owned.mkdir()
    (owned / ".gargantua-build").touch()
    (owned / "generated.c").touch()
    result = subprocess.run(["./eat", "clean"], cwd=root, env=dict(os.environ, OUT=".owned"), capture_output=True)
    assert result.returncode == 0 and not owned.exists()
print("  clean: protected, unowned, traversal, symlink and owned-directory cases passed")
