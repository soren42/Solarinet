"""Test DNS target state integrity and release proof handling without live DNS."""
import importlib.util
import json
import pathlib
import sys
import tempfile
import types
import unittest


# pika is only used by the long-running consumer loop.
sys.modules.setdefault("pika", types.ModuleType("pika"))
MODULE_PATH = pathlib.Path(__file__).with_name("sor_apply_dnsd.py")
SPEC = importlib.util.spec_from_file_location("sor_apply_dnsd_unit", MODULE_PATH)
dns = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dns)


class DnsApplyStateTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.state_dir = self.root / "state"
        self.state_dir.mkdir(mode=0o700)
        self.state_path = self.state_dir / "targets.json"
        import configparser
        self.cfg = configparser.ConfigParser()
        self.cfg["dns"] = {"target_state_file": str(self.state_path)}

    def test_target_updates_are_durable_private_and_merge_by_target(self):
        dns.target_status(self.cfg, "primary", {"phase": "pending", "desiredSource": "a" * 64})
        dns.target_status(self.cfg, "primary", {"phase": "applied", "appliedGeneration": "b" * 64})
        dns.target_status(self.cfg, "secondary:chlorine", {"phase": "pending"})

        state = json.loads(self.state_path.read_text(encoding="utf-8"))
        self.assertEqual(1, state["version"])
        self.assertEqual("applied", state["targets"]["primary"]["phase"])
        self.assertEqual("a" * 64, state["targets"]["primary"]["desiredSource"])
        self.assertEqual("b" * 64, state["targets"]["primary"]["appliedGeneration"])
        self.assertEqual("pending", state["targets"]["secondary:chlorine"]["phase"])
        self.assertEqual(0o600, self.state_path.stat().st_mode & 0o777)
        self.assertEqual(0o600, self.state_path.with_suffix(".json.lock").stat().st_mode & 0o777)
        self.assertEqual(["targets.json", "targets.json.lock"], sorted(p.name for p in self.state_dir.iterdir()))

    def test_world_accessible_state_directory_is_rejected(self):
        self.state_dir.chmod(0o755)
        with self.assertRaisesRegex(ValueError, "directory must be private"):
            dns.target_status(self.cfg, "primary", {"phase": "pending"})

    def test_world_accessible_state_file_is_rejected(self):
        self.state_path.write_text('{"version":1,"targets":{}}', encoding="utf-8")
        self.state_path.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "state file must be private"):
            dns.target_status(self.cfg, "primary", {"phase": "pending"})

    def test_symlinked_state_file_is_rejected(self):
        outside = self.root / "outside.json"
        outside.write_text('{"version":1,"targets":{}}', encoding="utf-8")
        self.state_path.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "must not be a symlink"):
            dns.target_status(self.cfg, "primary", {"phase": "pending"})

    def test_symlinked_lock_file_is_rejected(self):
        outside = self.root / "outside.lock"
        outside.touch()
        self.state_path.with_suffix(".json.lock").symlink_to(outside)
        with self.assertRaises(OSError):
            dns.target_status(self.cfg, "primary", {"phase": "pending"})

    def test_empty_or_symlinked_render_set_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "contains no zone files"):
            dns.source_hash([])
        zone = self.root / "db.example"
        zone.write_text("zone", encoding="utf-8")
        self.assertEqual(64, len(dns.source_hash([zone])))
        link = self.root / "db.link"
        link.symlink_to(zone)
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            dns.source_hash([link])

    def test_release_proof_requires_the_requested_generation_to_be_applied(self):
        import subprocess
        digest = "a" * 64
        valid = subprocess.CompletedProcess([], 0, json.dumps({
            "phase": "applied", "desired": digest, "applied": digest,
        }), "")
        self.assertEqual(digest, dns.proof(valid, "primary"))
        mismatch = subprocess.CompletedProcess([], 0, json.dumps({
            "phase": "applied", "desired": digest, "applied": "b" * 64,
        }), "")
        with self.assertRaisesRegex(RuntimeError, "did not prove"):
            dns.proof(mismatch, "primary")


if __name__ == "__main__":
    unittest.main()
