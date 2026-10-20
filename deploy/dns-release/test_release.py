"""Exercise release/rollback against a real BIND on an isolated loopback port."""
import importlib.util
import json
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("dns_release", pathlib.Path(__file__).with_name("release.py"))
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


def zoneText(origin, serial, address):
    return (f"$ORIGIN {origin}.\n$TTL 60\n"
            f"@ IN SOA ns.{origin}. hostmaster.{origin}. ( {serial} 60 60 300 60 )\n"
            f"@ IN NS ns.{origin}.\n@ IN A {address}\nns IN A 127.0.0.1\n")


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        for command in ("named", "named-checkconf", "named-checkzone", "dig"):
            if shutil.which(command) is None:
                self.fail(f"{command} is required for DNS release integration tests")
        self.temporary = tempfile.TemporaryDirectory(prefix="solari-dns-release-")
        self.addCleanup(self.temporary.cleanup)
        self.folder = pathlib.Path(self.temporary.name)
        self.root = self.folder / "releases"
        self.link = self.folder / "current"
        self.main = self.folder / "named.conf"
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]
        self.config = {
            "releaseRoot": str(self.root), "currentLink": str(self.link),
            "checkZoneCommand": ["named-checkzone"],
            "expandCommand": ["named-checkconf", "-p", str(self.main)],
            "checkConfigCommand": ["named-checkconf", "-z"],
            "queryAddress": "127.0.0.1", "queryPort": self.port,
            "healthTimeout": 4,
        }
        initial = self.candidate("initial", 2026102001, "192.0.2.1", obsolete=True)
        self.previous, self.previousManifest = release.stageGeneration(self.config, initial)
        self.link.symlink_to(self.previous)
        unmanaged = self.folder / "db.external.test"
        unmanaged.write_text(zoneText("external.test", 2026102001, "192.0.2.99"))
        self.main.write_text(
            f'options {{ directory "{self.folder}"; listen-on port {self.port} {{ 127.0.0.1; }}; '
            'listen-on-v6 { none; }; recursion no; dnssec-validation no; '
            f'pid-file "{self.folder}/named.pid"; session-keyfile "{self.folder}/session.key"; }};\n'
            'controls {};\n'
            f'include "{self.link}/named.conf.managed";\n'
            f'zone "external.test" {{ type primary; file "{unmanaged}"; notify no; allow-transfer {{ none; }}; }};\n')
        self.log = (self.folder / "named.log").open("w")
        # Debian/Ubuntu attach AppArmor to /usr/sbin/named and deny temporary
        # config paths. A private unprivileged test copy leaves that profile and
        # the production process untouched while serving only a high loopback port.
        binary = self.folder / "named-test"
        shutil.copy2(shutil.which("named"), binary)
        self.process = subprocess.Popen([str(binary), "-g", "-c", str(self.main), "-n", "1", "-U", "1"],
                                        stdout=self.log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stopNamed)
        self.config["reloadCommand"] = ["kill", "-HUP", str(self.process.pid)]
        try:
            release.verifyServed(self.config, self.previousManifest)
        except Exception:
            self.log.flush()
            self.fail((self.folder / "named.log").read_text()[-3000:])

    def stopNamed(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.log.close()

    def candidate(self, label, serial, address, obsolete=False):
        directory = self.folder / label
        directory.mkdir()
        (directory / "db.example.test").write_text(zoneText("example.test", serial, address))
        if obsolete:
            (directory / "db.bare-oldhost").write_text(zoneText("oldhost", serial, "192.0.2.8"))
        return directory

    def assertRollbackServing(self):
        manifest = release.loadManifest(self.link.resolve())
        zone = next(zone for zone in manifest["zones"] if zone["name"] == "example.test")
        self.assertEqual("192.0.2.1", zone["checkValue"])
        self.assertTrue(release.newerSerial(zone["serial"], 2026102002))
        release.verifyServed(self.config, manifest)

    def test_activate_proves_serving_and_removes_retired_zone(self):
        candidate = self.candidate("next", 2026102002, "192.0.2.2")
        result = release.activate(self.config, candidate)
        self.assertEqual("applied", result["phase"])
        self.assertEqual([], list(self.root.glob(".validation-*")))
        self.assertNotEqual(self.previous, self.link.resolve())
        release.verifyServed(self.config, release.loadManifest(self.link.resolve()))
        query = subprocess.run(["dig", "@127.0.0.1", "-p", str(self.port),
                                "oldhost", "SOA", "+norecurse", "+noall", "+comments"],
                               text=True, capture_output=True)
        self.assertNotRegex(query.stdout, r"flags:[^;]*\baa\b")
        # The unmanaged zone survives the replacement of the entire managed set.
        query = subprocess.run(["dig", "@127.0.0.1", "-p", str(self.port),
                                "external.test", "A", "+short"], text=True, capture_output=True)
        self.assertEqual("192.0.2.99", query.stdout.strip())

    def test_invalid_zone_does_not_switch(self):
        candidate = self.candidate("invalid", 2026102002, "192.0.2.2")
        with (candidate / "db.example.test").open("a") as output:
            output.write("@ IN CNAME elsewhere.test.\n")
        with self.assertRaises(RuntimeError):
            release.activate(self.config, candidate)
        self.assertEqual(self.previous, self.link.resolve())
        release.verifyServed(self.config, self.previousManifest)

    def test_served_health_failure_rolls_back(self):
        candidate = self.candidate("health-failure", 2026102002, "192.0.2.2")
        def health(config, manifest):
            release.verifyServed(config, manifest)
            if any(zone["checkValue"] == "192.0.2.2" for zone in manifest["zones"]):
                raise RuntimeError("injected health failure after successful load")
        with self.assertRaises(RuntimeError):
            release.activate(self.config, candidate, health)
        self.assertRollbackServing()
        self.assertEqual("failed", json.loads((self.root / "state.json").read_text())["phase"])

    def test_reload_failure_rolls_back(self):
        candidate = self.candidate("reload-failure", 2026102002, "192.0.2.2")
        original = release.runCommand
        calls = []
        def command(config, key, arguments=()):
            if key == "reloadCommand":
                calls.append(key)
                if len(calls) == 1:
                    raise RuntimeError("injected reload failure")
            return original(config, key, arguments)
        with mock.patch.object(release, "runCommand", side_effect=command):
            with self.assertRaises(RuntimeError):
                release.activate(self.config, candidate)
        self.assertEqual(2, len(calls))
        self.assertRollbackServing()

    def test_interrupted_switch_recovers_previous(self):
        candidate = self.candidate("interrupted", 2026102002, "192.0.2.2")
        destination, _ = release.stageGeneration(self.config, candidate)
        release.writeJson(self.root / "state.json", {"phase": "pending",
            "previous": self.previous.name, "desired": destination.name})
        release.switchLink(self.config, destination)
        result = release.recoverPending(self.config)
        self.assertEqual("recovered", result["phase"])
        self.assertRollbackServing()

    def test_unmanaged_zone_cannot_be_claimed(self):
        candidate = self.candidate("collision", 2026102002, "192.0.2.2")
        (candidate / "db.external.test").write_text(zoneText("external.test", 2026102002, "192.0.2.3"))
        with self.assertRaises(ValueError):
            release.activate(self.config, candidate)
        self.assertEqual(self.previous, self.link.resolve())

    def test_candidate_symlink_is_rejected(self):
        candidate = self.folder / "symlink"
        candidate.mkdir()
        (candidate / "db.example.test").symlink_to(self.folder / "db.external.test")
        with self.assertRaises(OSError):
            release.stageGeneration(self.config, candidate)

    def test_candidate_owner_is_checked_on_open_descriptor(self):
        candidate = self.candidate("wrong-owner", 2026102002, "192.0.2.2")
        with self.assertRaises(ValueError):
            release.inspectZone(candidate / "db.example.test", os.getuid() + 1)

    def test_reintroduced_zone_advances_and_unchanged_poll_keeps_serial(self):
        removed = self.candidate("removed", 2026102002, "192.0.2.2")
        release.activate(self.config, removed)
        reintroduced = self.candidate("reintroduced", 1, "192.0.2.2", obsolete=True)
        first = release.activate(self.config, reintroduced)
        manifest = release.loadManifest(self.link.resolve())
        oldhost = next(zone for zone in manifest["zones"] if zone["name"] == "oldhost")
        self.assertTrue(release.newerSerial(oldhost["serial"], 2026102001))
        second = release.activate(self.config, reintroduced)
        self.assertEqual(first["applied"], second["applied"])

    def test_failed_rollback_is_retried_from_its_recorded_generation(self):
        candidate = self.candidate("rollback-failed", 2026102002, "192.0.2.2")
        original = release.runCommand
        def command(config, key, arguments=()):
            if key == "reloadCommand":
                raise RuntimeError("injected persistent reload outage")
            return original(config, key, arguments)
        with mock.patch.object(release, "runCommand", side_effect=command):
            with self.assertRaises(RuntimeError):
                release.activate(self.config, candidate)
        state = json.loads((self.root / "state.json").read_text())
        self.assertEqual("rollbackFailed", state["phase"])
        rollback = state["rollback"]
        result = release.recoverPending(self.config)
        self.assertEqual(rollback, result["applied"])
        self.assertRollbackServing()


if __name__ == "__main__":
    unittest.main()
