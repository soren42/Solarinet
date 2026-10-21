#!/usr/bin/env python3
"""Hermetic tests for the root-owned SolariNet backup runner."""
from __future__ import annotations

import datetime
import gzip
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "deploy/backups/solari-backup.sh"


def can_run_root() -> bool:
    if os.geteuid() == 0:
        return True
    return shutil.which("sudo") is not None and subprocess.run(
        ["sudo", "-n", "true"], stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0


@unittest.skipUnless(can_run_root(), "backup runner requires root-owned config validation")
class BackupRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solari-backup-test-")
        self.root = Path(self.temp.name)
        self.target = self.root / "target"
        self.target.mkdir(mode=0o700)
        self.config = self.root / "backup.conf"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def runAsRoot(self, args: list[str]) -> subprocess.CompletedProcess[str]:
        command = args if os.geteuid() == 0 else ["sudo", "-n", *args]
        return subprocess.run(command, cwd=ROOT, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def chown(self, path: Path, owner: str) -> None:
        if os.geteuid() == 0:
            return
        result = subprocess.run(["sudo", "-n", "chown", "-R", owner, str(path)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def configure(self, dump_body: str, exit_code: int = 0,
                  run_as: str = "", report_user: bool = False,
                  mountpoint_required: bool = False) -> None:
        self.root.chmod(0o755)
        fake_dump = self.root / "fake-mysqldump"
        report_user_line = ('printf \'\\n-- executed-as=%s\\n\' "$(id -un)"\n'
                            if report_user else "")
        fake_dump.write_text(
            "#!/bin/sh\n"
            f"printf '%s' {shlex.quote(dump_body)}\n"
            + report_user_line
            + f"exit {exit_code}\n",
            encoding="utf-8",
        )
        fake_dump.chmod(0o755)
        self.config.write_text(
            "\n".join((
                'DB_NAMES="sor"',
                f"TARGET_DIR={shlex.quote(str(self.target))}",
                'RETENTION_DAYS="1"',
                f"MYSQLDUMP_BIN={shlex.quote(str(fake_dump))}",
                f"MYSQLDUMP_RUN_AS={shlex.quote(run_as)}",
                f"TARGET_MOUNTPOINT_REQUIRED={1 if mountpoint_required else 0}",
                "",
            )),
            encoding="utf-8",
        )
        self.config.chmod(0o600)

    def runBackup(self) -> subprocess.CompletedProcess[str]:
        if os.geteuid() != 0:
            self.chown(self.config, "root:root")
            self.chown(self.target, "root:root")
        try:
            return self.runAsRoot([str(RUNNER), str(self.config)])
        finally:
            if os.geteuid() != 0:
                self.chown(self.root, f"{os.getuid()}:{os.getgid()}")

    def testSuccessfulDumpIsValidAtomicAndPrunesOnlyMatchingExpiredFiles(self) -> None:
        sql = "-- hermetic backup fixture\nCREATE TABLE fixture(id INT);\n"
        self.configure(sql)
        stale = self.target / "sor-2000-01-01.sql.gz"
        stale.write_bytes(gzip.compress(b"old"))
        old = (datetime.datetime.now().timestamp() - 10 * 86400)
        os.utime(stale, (old, old))
        unrelated = self.target / "keep-this.txt"
        unrelated.write_text("keep", encoding="utf-8")

        result = self.runBackup()

        dest = self.target / f"sor-{datetime.date.today().isoformat()}.sql.gz"
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(gzip.decompress(dest.read_bytes()).decode(), sql)
        self.assertEqual(dest.stat().st_mode & 0o777, 0o600)
        self.assertFalse(stale.exists())
        self.assertEqual(unrelated.read_text(encoding="utf-8"), "keep")
        self.assertEqual(list(self.target.glob("*.tmp.*")), [])

    def testSuccessfulEmptyDumpIsRejectedAndNotPublished(self) -> None:
        self.configure("")

        result = self.runBackup()

        dest = self.target / f"sor-{datetime.date.today().isoformat()}.sql.gz"
        self.assertNotEqual(result.returncode, 0, "empty dump was incorrectly accepted")
        self.assertFalse(dest.exists())
        self.assertEqual(list(self.target.glob("*.tmp.*")), [])

    def testWorldAccessibleTargetIsRejectedBeforeDumping(self) -> None:
        self.configure("must not be written\n")
        self.target.chmod(0o755)

        result = self.runBackup()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("root-owned mode 0700", result.stderr)
        self.assertEqual(list(self.target.iterdir()), [])

    def testMissingRequiredMountFailsBeforeDumping(self) -> None:
        self.configure("must not be written\\n", mountpoint_required=True)

        result = self.runBackup()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("target must be a mounted filesystem", result.stderr)
        self.assertEqual(list(self.target.iterdir()), [])

    def testDumpCanRunAsUnprivilegedUnixAccount(self) -> None:
        if shutil.which("runuser") is None and shutil.which("su") is None:
            self.skipTest("runuser/su unavailable")
        self.configure("-- hermetic backup fixture\n", run_as="nobody", report_user=True)

        result = self.runBackup()

        dest = self.target / f"sor-{datetime.date.today().isoformat()}.sql.gz"
        self.assertEqual(result.returncode, 0, result.stderr)
        restored = gzip.decompress(dest.read_bytes()).decode()
        self.assertIn("-- executed-as=nobody", restored)
        self.assertEqual(dest.stat().st_mode & 0o777, 0o600)

    def testFailedDumpPreservesExistingBackupAndRemovesPartialOutput(self) -> None:
        self.configure("partial dump output\n", exit_code=17)
        dest = self.target / f"sor-{datetime.date.today().isoformat()}.sql.gz"
        prior = gzip.compress(b"previous known backup")
        dest.write_bytes(prior)

        result = self.runBackup()

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(dest.read_bytes(), prior)
        self.assertEqual(list(self.target.glob("*.tmp.*")), [])

    def testFailedForgejoDumpRemovesRunAsTemporaryDirectory(self) -> None:
        if shutil.which("runuser") is None and shutil.which("su") is None:
            self.skipTest("runuser/su unavailable")
        self.configure("-- hermetic backup fixture\n")
        forgejo_config = self.root / "forgejo.ini"
        forgejo_config.write_text("[database]\n", encoding="utf-8")
        report_path = self.root / "forgejo-temp-path"
        report_path.touch()
        report_path.chmod(0o666)
        fake_forgejo = self.root / "fake-forgejo"
        fake_forgejo.write_text(
            "#!/bin/sh\n"
            "while [ \"$#\" -gt 0 ]; do\n"
            "  if [ \"$1\" = --file ]; then shift; output=$1; fi\n"
            "  shift\n"
            "done\n"
            f"dirname -- \"$output\" > {shlex.quote(str(report_path))}\n"
            "printf partial > \"$output\"\n"
            "exit 17\n",
            encoding="utf-8",
        )
        fake_forgejo.chmod(0o755)
        with self.config.open("a", encoding="utf-8") as config:
            config.write(
                f"FORGEJO_BIN={shlex.quote(str(fake_forgejo))}\n"
                f"FORGEJO_CONFIG={shlex.quote(str(forgejo_config))}\n"
                'FORGEJO_RUN_AS="nobody"\n'
            )

        result = self.runBackup()

        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(report_path.is_file(), result.stderr)
        leaked_dir = Path(report_path.read_text(encoding="utf-8").strip())
        self.assertFalse(leaked_dir.exists(), "failed Forgejo dump left its temporary directory")


if __name__ == "__main__":
    unittest.main()
