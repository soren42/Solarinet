#!/usr/bin/env python3
"""Package, install, and browser-test the exact dashboard release payload."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "deploy/dashboard/package-release.py"
INSTALLER = ROOT / "deploy/dashboard/install-release.py"
BROWSER_TEST = ROOT / "tests/browser/test_scanner_import_e2e.cjs"


def main() -> int:
    env = os.environ.copy()
    if env.get("SOLARI_TEST_SOR_DB") != "1" or env.get("SOR_DB_HOST") != "127.0.0.1":
        print("Refusing package browser test without explicit loopback scratch SoR opt-in.", file=sys.stderr)
        return 2
    if not re.fullmatch(r"solarinet_[a-z0-9_]+", env.get("SOR_DB_NAME", "")):
        print("Refusing package browser test: database name is not an isolated solarinet_* schema.", file=sys.stderr)
        return 2

    binary_dir = Path(env.get("SOLARI_RELEASE_BIN_DIR", ROOT / "build-release/src/server"))
    binaries = [binary_dir / "solariServer", binary_dir / "solariScanWorker"]
    if not all(path.is_file() and os.access(path, os.X_OK) for path in binaries):
        print("Release server and scan-worker binaries are required.", file=sys.stderr)
        return 2

    dirty = bool(subprocess.check_output(
        ["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all"], text=True,
    ).strip())
    with tempfile.TemporaryDirectory(prefix="solarinet-package-browser-") as temp:
        temp_root = Path(temp)
        archive = temp_root / "dashboard.tar.gz"
        install_root = temp_root / "install"
        version = "browser-e2e"
        package = [
            sys.executable, str(PACKAGER), "--version", version,
            "--binary", str(binaries[0]), "--scan-worker-binary", str(binaries[1]),
            "--output", str(archive),
        ]
        install = [
            sys.executable, str(INSTALLER), "--root", str(install_root), "--bundle", str(archive),
        ]
        if dirty:
            package.append("--allow-dirty")
            install.append("--allow-dirty")
        subprocess.run(package, cwd=ROOT, env=env, check=True)
        subprocess.run(install, cwd=ROOT, env=env, check=True)
        env["SOLARI_TEST_ROOT"] = str(install_root / "current")
        subprocess.run(["node", str(BROWSER_TEST)], cwd=ROOT, env=env, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
