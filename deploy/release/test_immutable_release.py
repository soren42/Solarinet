#!/usr/bin/env python3
"""Temporary-root coverage for immutable daemon packaging and cutover."""
from __future__ import annotations

import importlib.util
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "deploy/dashboard/package-release.py"
INSTALLER = ROOT / "deploy/dashboard/install-release.py"
CUTOVER = ROOT / "deploy/host-configs/xenon/cutover-immutable.sh"
VERIFY = ROOT / "deploy/host-configs/xenon/verify-immutable-processes.py"
UNIT_NAMES = [
    "sor-emit", "sor-apply-dns", "sor-apply-pihole", "sor-reconcile", "notify",
    "alertbridge", "unifipolld", "solarinet-client", "solarinet-monitor",
    "solarinet-snmp", "solari-mdns-inspect", "solarinet-server",
]


def import_file(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ImmutableReleaseTests(unittest.TestCase):
    def test_legacy_dashboard_bundle_still_installs(self):
        installer = import_file("legacy_installer", INSTALLER)
        with tempfile.TemporaryDirectory(prefix="solarinet-legacy-release-") as temp:
            base = Path(temp)
            bundle = base / "legacy.tar.gz"
            payloads = {
                "bin/solariServer": b"server",
                "bin/solariScanWorker": b"worker",
                "dashboard/api/index.php": b"<?php\n",
                "dashboard/public/index.html": b"<!doctype html>\n",
                "dashboard/public/rackwire/index.html": b"<!doctype html>\n",
            }
            metadata = {
                "format": 1, "version": "legacy-dashboard", "sourceTreeDirty": False,
                "files": {name: hashlib.sha256(data).hexdigest() for name, data in payloads.items()},
            }
            payloads["RELEASE.json"] = (json.dumps(metadata, sort_keys=True, indent=2) + "\n").encode()
            with tarfile.open(bundle, "w:gz") as archive:
                for name, data in payloads.items():
                    member = tarfile.TarInfo(name)
                    member.size = len(data)
                    member.mode = 0o644
                    archive.addfile(member, io.BytesIO(data))
            root = base / "install"
            with patch.object(sys, "argv", [str(INSTALLER), "--root", str(root), "--bundle", str(bundle)]):
                self.assertEqual(installer.main(), 0)
            release = root / "releases/legacy-dashboard"
            self.assertTrue((root / "current").is_symlink())
            self.assertFalse((release / ".venv").exists())

    def test_package_install_and_manifest_include_daemons(self):
        installer = import_file("immutable_installer", INSTALLER)
        with tempfile.TemporaryDirectory(prefix="solarinet-immutable-package-") as temp:
            base = Path(temp)
            binaries = base / "binaries"
            binaries.mkdir()
            for name in ("server", "worker", "client", "monitor", "snmp"):
                path = binaries / name
                path.write_text("#!/bin/sh\nexit 0\n")
                path.chmod(0o755)
            bundle = base / "release.tar.gz"
            dirty = bool(subprocess.check_output(
                ["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all"], text=True
            ).strip())
            command = [sys.executable, str(PACKAGER), "--version", "test-daemons",
                       "--binary", str(binaries / "server"),
                       "--scan-worker-binary", str(binaries / "worker"),
                       "--client-binary", str(binaries / "client"),
                       "--monitor-binary", str(binaries / "monitor"),
                       "--snmp-binary", str(binaries / "snmp"),
                       "--output", str(bundle)]
            if dirty:
                command.append("--allow-dirty")
            subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
            with tarfile.open(bundle, "r:gz") as archive:
                names = set(archive.getnames())
                metadata = json.load(archive.extractfile("RELEASE.json"))
            for name in ("bin/solariClient", "bin/solariMonitor", "bin/solariSnmpPoll",
                         "apps/sorsync/sor_emitd.py", "apps/notify/notifyd.py",
                         "apps/alertbridge/alertbridge.py", "apps/unifi/unifipolld.py",
                         "apps/discovery/mdns_inspect.py", "apps/requirements.txt"):
                self.assertIn(name, names)
            self.assertFalse(any(Path(name).name.lower().startswith(".env")
                                 or name.endswith((".key", ".pem")) for name in names))
            self.assertTrue(metadata["files"])

            root = base / "install"
            def fake_venv(release: Path) -> None:
                python_venv = release / ".venv/bin/python3"
                python_venv.parent.mkdir(parents=True)
                python_venv.write_text("#!/bin/sh\nexit 0\n")
                python_venv.chmod(0o755)

            with patch.object(sys, "argv", [str(INSTALLER), "--root", str(root), "--bundle", str(bundle),
                                             *( ["--allow-dirty"] if dirty else [] )]), \
                 patch.object(installer, "ensure_python_venv", fake_venv):
                self.assertEqual(installer.main(), 0)
            release = root / "releases/test-daemons"
            self.assertEqual(installer.validate_installed(release, allow_dirty=dirty), "test-daemons")
            self.assertEqual((root / "current").resolve(), release.resolve())

    def make_mock_systemctl(self, path: Path, log: Path) -> None:
        path.write_text("""#!/usr/bin/env bash
set -u
printf '%s\\n' "$*" >> "$CALL_LOG"
case "$1" in
  is-active) exit 0 ;;
  restart|start)
    service="${2%.service}"
    if [[ "${FAIL_SERVICE:-}" == "$service" && ! -e "$FAIL_ONCE" ]]; then touch "$FAIL_ONCE"; exit 1; fi
    exit 0 ;;
  show) if [[ "$*" == *Result* ]]; then echo success; else echo 0; fi ;;
  daemon-reload|stop) exit 0 ;;
  *) exit 2 ;;
esac
""")
        path.chmod(0o755)
        self.addCleanup(lambda: None)

    def test_cutover_dry_run_and_failure_restore_with_temp_root(self):
        for fail_service in (None, "notify"):
            with self.subTest(fail_service=fail_service), tempfile.TemporaryDirectory(prefix="solarinet-cutover-") as temp:
                base = Path(temp)
                fake_root = base / "root"
                unit_dir = fake_root / "etc/systemd/system"
                unit_dir.mkdir(parents=True)
                config = fake_root / "home/jason/Code/Solarinet/deploy/alertbridge/alertbridge.conf"
                config.parent.mkdir(parents=True)
                original_config = "[state]\n# file = /var/lib/solari/alertbridge.state.json\n"
                config.write_text(original_config)
                for name in UNIT_NAMES:
                    (unit_dir / f"{name}.service").write_text(f"old-unit={name}\n")
                mock = base / "systemctl"
                call_log = base / "calls.log"
                self.make_mock_systemctl(mock, call_log)
                env = os.environ.copy()
                env["CALL_LOG"] = str(call_log)
                env["FAIL_ONCE"] = str(base / "failed-once")
                if fail_service:
                    env["FAIL_SERVICE"] = fail_service
                result = subprocess.run([str(CUTOVER), "--dry-run", "--root", str(fake_root),
                                         "--systemctl", str(mock)], capture_output=True, text=True, env=env)
                if fail_service:
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((unit_dir / "notify.service").read_text(), "old-unit=notify\n")
                    self.assertEqual(config.read_text(), original_config)
                else:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    for name in UNIT_NAMES:
                        installed = (unit_dir / f"{name}.service").read_text()
                        self.assertIn("/opt/solarinet/current", installed)
                    self.assertIn("file = /home/jason/Code/Solarinet/deploy/alertbridge/alertbridge.state.json",
                                  config.read_text())
                backups = list((fake_root / "var/backups").glob("solarinet-immutable-*/units"))
                self.assertTrue(backups)
                self.assertEqual((backups[0] / "notify.service").read_text(), "old-unit=notify\n")
                self.assertIn("daemon-reload", call_log.read_text())
                if not fail_service:
                    backup = backups[0].parent
                    rollback = ROOT / "deploy/host-configs/xenon/rollback-immutable.sh"
                    result = subprocess.run([str(rollback), str(backup), "--dry-run", "--root", str(fake_root),
                                             "--systemctl", str(mock)], capture_output=True, text=True, env=env)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((unit_dir / "notify.service").read_text(), "old-unit=notify\n")
                    self.assertEqual(config.read_text(), original_config)

    def test_process_verifier_detects_checkout_script_argument(self):
        with tempfile.TemporaryDirectory(prefix="solarinet-proc-") as temp:
            proc_root = Path(temp)
            proc = proc_root / "123"
            proc.mkdir()
            (proc / "comm").write_text("python3\n")
            (proc / "cmdline").write_bytes(
                b"/usr/bin/python3\0/home/jason/Code/Solarinet/deploy/sorsync/sor_emitd.py\0"
            )
            (proc / "cwd").symlink_to("/opt/solarinet/releases/r1/apps/sorsync")
            (proc / "exe").symlink_to("/usr/bin/python3")
            result = subprocess.run([sys.executable, str(VERIFY), "--proc-root", str(proc_root)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("script=/home/jason/Code/Solarinet/deploy/sorsync/sor_emitd.py", result.stdout)
            (proc / "comm").write_text("editor\n")
            (proc / "cmdline").write_bytes(b"editor\0/home/jason/Code/Solarinet\0")
            result = subprocess.run([sys.executable, str(VERIFY), "--proc-root", str(proc_root)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
