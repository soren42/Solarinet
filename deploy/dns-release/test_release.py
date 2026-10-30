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


class BindFixture(unittest.TestCase):
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
        self.prepareInitialLayout()
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

    def prepareInitialLayout(self):
        pass

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

class ReleaseTest(BindFixture):
    def test_staged_n_plus_two_validation_failure_keeps_n_plus_one_served(self):
        n_plus_one = self.candidate("stage-n-plus-one", 2026102002, "192.0.2.2")
        result = release.activate(self.config, n_plus_one)
        self.assertEqual("applied", result["phase"])
        active = release.loadManifest(self.link.resolve())
        release.verifyServed(self.config, active)
        served = subprocess.run(
            ["dig", "@127.0.0.1", "-p", str(self.port), "example.test", "SOA",
             "+norecurse", "+noall", "+answer"], capture_output=True, text=True, timeout=3)
        self.assertIn(f" {active['zones'][0]['serial']} ", served.stdout)

        n_plus_two = self.candidate("stage-n-plus-two-invalid", 2026102003, "192.0.2.3")
        with (n_plus_two / "db.example.test").open("a", encoding="utf-8") as output:
            output.write("@ IN CNAME forbidden.example.test.\n")
        with self.assertRaises(RuntimeError):
            release.activate(self.config, n_plus_two)

        rollback = release.loadManifest(self.link.resolve())
        # Depending on the BIND build the bad zone is rejected either by
        # prevalidation (state untouched) or after reload (rolled back).
        state = json.loads((self.root / "state.json").read_text())
        self.assertIn(state["phase"], ("applied", "failed"))
        self.assertEqual(active["generation"], state["applied"])
        self.assertEqual(active["generation"], rollback["generation"])
        self.assertEqual("192.0.2.2", next(z for z in rollback["zones"]
                                             if z["name"] == "example.test")["checkValue"])
        release.verifyServed(self.config, rollback)
        restored = subprocess.run(
            ["dig", "@127.0.0.1", "-p", str(self.port), "example.test", "SOA",
             "+norecurse", "+noall", "+answer"], capture_output=True, text=True, timeout=3)
        self.assertIn(f" {rollback['zones'][0]['serial']} ", restored.stdout)

    def test_ns_only_zone_is_verified_after_activation(self):
        candidate = self.folder / "ns-only"
        candidate.mkdir()
        (candidate / "db.ns-only.test").write_text(
            "$ORIGIN ns-only.test.\n$TTL 60\n"
            "@ IN SOA ns.ns-only.test. hostmaster.ns-only.test. ( 2026102102 60 60 300 60 )\n"
            "@ IN NS ns.external.test.\n")
        previous_mask = os.umask(0o077)
        try:
            result = release.activate(self.config, candidate)
        finally:
            os.umask(previous_mask)
        self.assertEqual("applied", result["phase"])
        manifest = release.loadManifest(self.link.resolve())
        zone = next(item for item in manifest["zones"] if item["name"] == "ns-only.test")
        self.assertEqual("NS", zone["checkType"])
        self.assertEqual(0o755, (self.link.resolve() / "zones").stat().st_mode & 0o777)
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


class BootstrapTest(BindFixture):
    def prepareInitialLayout(self):
        self.legacyRoot = self.folder / "legacy-zones"
        self.legacyRoot.mkdir()
        includes = []
        for bare, label in ((False, "akoria"), (True, "bare")):
            declarations = []
            for zone in self.previousManifest["zones"]:
                if zone["bare"] != bare:
                    continue
                content = (self.previous / "zones" / zone["file"]).read_text()
                if bare:
                    content = "\n".join(content.splitlines()[1:]) + "\n"
                file = self.legacyRoot / zone["file"]
                file.write_text(content)
                declarations.append(f'zone "{zone["name"]}" {{ type master; file "{file}"; notify no; }};')
            path = self.folder / ("named.conf." + label)
            path.write_text("\n".join(declarations) + "\n")
            includes.append({"hostPath": str(path), "runtimePath": str(path)})
        self.parent = self.folder / "named.conf.local"
        self.parent.write_text("// Preserve this unmanaged comment.\n" +
                              "".join(f'include "{item["runtimePath"]}";\n' for item in includes))
        self.parent.chmod(0o640)
        self.originalParent = self.parent.read_bytes()
        self.originalZones = {path.name: path.read_bytes() for path in self.legacyRoot.iterdir()}
        text = self.main.read_text().replace(f'include "{self.link}/named.conf.managed";',
                                            f'include "{self.parent}";')
        self.main.write_text(text)
        self.link.unlink()
        self.config.update(includeParent=str(self.parent), legacyZoneRoot=str(self.legacyRoot),
                           legacyIncludes=includes)

    def assertLegacyRestored(self):
        self.assertEqual(self.originalParent, self.parent.read_bytes())
        self.assertFalse(self.link.exists())
        self.assertEqual(0o640, self.parent.stat().st_mode & 0o777)
        release.verifyServed(self.config, self.previousManifest)

    def test_bootstrap_preserves_legacy_data_and_is_idempotent(self):
        result = release.bootstrap(self.config)
        self.assertEqual("applied", result["phase"])
        self.assertEqual(0o640, self.parent.stat().st_mode & 0o777)
        self.assertIn("Preserve this unmanaged comment.", self.parent.read_text())
        self.assertIn(str(self.link) + "/named.conf.managed", self.parent.read_text())
        for name, content in self.originalZones.items():
            self.assertEqual(content, (self.legacyRoot / name).read_bytes())
        _, manifest = release.currentGeneration(self.config)
        release.verifyServed(self.config, manifest)
        self.assertEqual(result, release.bootstrap(self.config))

    def test_bootstrap_reload_failure_restores_exact_parent(self):
        original = release.runCommand
        calls = []
        def command(config, key, arguments=()):
            if key == "reloadCommand":
                calls.append(key)
                if len(calls) == 1:
                    raise RuntimeError("injected cutover reload failure")
            return original(config, key, arguments)
        with mock.patch.object(release, "runCommand", side_effect=command):
            with self.assertRaises(RuntimeError):
                release.bootstrap(self.config)
        self.assertLegacyRestored()

    def test_bootstrap_serving_failure_restores_exact_parent(self):
        calls = []
        def health(config, manifest):
            calls.append(manifest["generation"])
            release.verifyServed(config, manifest)
            if len(calls) == 2:
                raise RuntimeError("injected cutover serving failure")
        with self.assertRaises(RuntimeError):
            release.bootstrap(self.config, health)
        self.assertEqual(3, len(calls))
        self.assertLegacyRestored()

    def test_interrupted_cutover_blocks_activation_until_recovered(self):
        original = release.runCommand
        def crash(config, key, arguments=()):
            if key == "reloadCommand":
                raise KeyboardInterrupt("simulated process death")
            return original(config, key, arguments)
        with mock.patch.object(release, "runCommand", side_effect=crash):
            with self.assertRaises(KeyboardInterrupt):
                release.bootstrap(self.config)
        candidate = self.candidate("blocked", 2026102002, "192.0.2.2")
        with self.assertRaises(RuntimeError):
            release.activate(self.config, candidate)
        journal = json.loads((self.root / "bootstrap.json").read_text())
        self.assertEqual("pending", journal["phase"])
        release.restoreBootstrap(self.config, journal)
        self.assertLegacyRestored()
        self.assertEqual("applied", release.bootstrap(self.config)["phase"])

    def test_missing_legacy_include_fails_before_cutover(self):
        self.parent.write_text("// no managed includes\n")
        with self.assertRaises(ValueError):
            release.bootstrap(self.config)
        self.assertEqual("// no managed includes\n", self.parent.read_text())
        self.assertFalse(self.link.exists())

    def test_concurrent_legacy_change_fails_before_cutover(self):
        def health(config, manifest):
            release.verifyServed(config, manifest)
            with (self.legacyRoot / "db.example.test").open("a") as output:
                output.write("; concurrent legacy writer\n")
        with self.assertRaises(ValueError):
            release.bootstrap(self.config, health)
        self.assertEqual(self.originalParent, self.parent.read_bytes())
        self.assertFalse(self.link.exists())
        self.assertFalse((self.root / "bootstrap.json").exists())


class TransferPolicyTest(unittest.TestCase):
    def manifest(self, **extra):
        zones = [{"name": "akoria.net", "file": "db.akoria.net", "bare": False},
                 {"name": "xenon", "file": "db.bare-xenon", "bare": True}]
        return {"version": 1, "runtimeRoot": "/r", "generation": "g",
                "transfer": ["192.0.2.1", "192.0.2.2"], "zones": zones, **extra}

    def test_bare_zones_transfer_to_bare_targets(self):
        text = release.configurationText(self.manifest(bareTransfer=["192.0.2.2"]))
        self.assertIn('zone "xenon" { type primary; file "/r/g/zones/db.bare-xenon"; '
                      'allow-transfer { 192.0.2.2; }; also-notify { 192.0.2.2; }; notify yes; };', text)
        self.assertIn("allow-transfer { 192.0.2.1; 192.0.2.2; }", text)

    def test_legacy_manifest_keeps_bare_zones_local(self):
        text = release.configurationText(self.manifest())
        self.assertIn('db.bare-xenon"; allow-transfer { none; }; notify no; };', text)


if __name__ == "__main__":
    unittest.main()
