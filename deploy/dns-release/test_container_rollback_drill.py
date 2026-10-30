"""Staged release/rollback drill served by a disposable BIND container.

Run with unittest discovery. The test creates and always removes its own
container; it never reads or contacts host DNS configuration.
"""
import importlib.util
import json
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import uuid

SPEC = importlib.util.spec_from_file_location("dns_release", pathlib.Path(__file__).with_name("release.py"))
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)
DOCKER = ["sudo", "-n", "docker"]


def zone(serial, address):
    return ("$ORIGIN drill.invalid.\n$TTL 30\n"
            f"@ IN SOA ns.drill.invalid. hostmaster.drill.invalid. ( {serial} 30 30 300 30 )\n"
            "@ IN NS ns.drill.invalid.\n"
            f"@ IN A {address}\nns IN A 127.0.0.1\n")


@unittest.skipUnless(shutil.which("docker"), "Docker is required for the isolated BIND container drill")
class ContainerRollbackDrill(unittest.TestCase):
    def clean_temp(self, path):
        subprocess.run(["sudo", "-n", "chown", "-R", f"{os.getuid()}:{os.getgid()}", str(path)],
                       capture_output=True, text=True, timeout=20)
        shutil.rmtree(path, ignore_errors=True)

    def test_n_n_plus_one_failed_n_plus_two_served_rollback(self):
        with tempfile.TemporaryDirectory(prefix="solari-bind-container-", ignore_cleanup_errors=True) as temporary:
            root = pathlib.Path(temporary)
            self.addCleanup(self.clean_temp, root)
            root.chmod(0o755)
            (root / "runtime").mkdir()
            (root / "runtime").chmod(0o777)
            (root / "releases").mkdir()
            (root / "incoming").mkdir()
            (root / "current").symlink_to(root / "releases" / "seed")
            seed = root / "seed"
            seed.mkdir()
            (seed / "db.drill.invalid").write_text(zone(2026103001, "192.0.2.1"))
            config = {
                "releaseRoot": str(root / "releases"), "runtimeRoot": str(root / "releases"),
                "currentLink": str(root / "current"), "incomingRoot": str(root / "incoming"),
                "candidateUser": "root", "queryAddress": "127.0.0.1", "queryPort": 0,
                "healthTimeout": 20, "digCommand": ["dig"],
                "checkZoneCommand": DOCKER + ["exec", "PLACEHOLDER", "named-checkzone"],
                "expandCommand": DOCKER + ["exec", "PLACEHOLDER", "named-checkconf", "-p", str(root / "named.conf")],
                "checkConfigCommand": DOCKER + ["exec", "PLACEHOLDER", "named-checkconf", "-z"],
            }
            initial, manifest = release.stageGeneration(config, seed)
            link = root / "current"
            link.unlink()
            link.symlink_to(initial)
            main = root / "named.conf"
            main.write_text(
                f'options {{ directory "{root / "runtime"}"; listen-on port 53 {{ any; }}; '
                'listen-on-v6 { none; }; allow-query { any; }; recursion no; '
                f'dnssec-validation no; pid-file "{root / "runtime/named.pid"}"; '
                f'session-keyfile "{root / "runtime/session.key"}"; }}; controls {{}}; '
                f'include "{link}/named.conf.managed";\n')
            main.chmod(0o644)
            name = "solari-bind-" + uuid.uuid4().hex[:10]
            checker = root / "check-zone.sh"
            checker.write_text(
                "#!/bin/sh\n"
                "if grep -q '2026103003' \"$2\"; then echo injected-invalid-N-plus-two >&2; exit 1; fi\n"
                "exec named-checkzone \"$@\"\n")
            checker.chmod(0o755)
            command = DOCKER + ["run", "--detach", "--name", name, "--entrypoint", "/bin/sh", "--volume",
                                f"{root}:{root}", "--publish", "127.0.0.1::53/udp",
                                "--publish", "127.0.0.1::53/tcp",
                                "internetsystemsconsortium/bind9:9.20",
                                "-c", f"chown -R bind:bind {root / 'runtime'} && exec named -g -c {main} -u bind"]
            started = subprocess.run(command, capture_output=True, text=True, timeout=90)
            self.assertEqual(0, started.returncode, "could not start isolated BIND container")
            self.addCleanup(subprocess.run, DOCKER + ["rm", "--force", name],
                            capture_output=True, text=True, timeout=30)
            ports = subprocess.run(DOCKER + ["port", name, "53/udp"], capture_output=True,
                                   text=True, timeout=10)
            if ports.returncode:
                logs = subprocess.run(DOCKER + ["logs", name], capture_output=True,
                                      text=True, timeout=10)
                self.fail("isolated BIND container exited: " + logs.stdout[-2500:] + logs.stderr[-500:])
            port = int(ports.stdout.strip().rsplit(":", 1)[1])
            config["queryPort"] = port
            config["reloadCommand"] = DOCKER + ["kill", "--signal", "HUP", name]
            config["checkZoneCommand"] = DOCKER + ["exec", name, "/bin/sh", str(checker)]
            for key in ("checkZoneCommand", "expandCommand", "checkConfigCommand"):
                config[key] = [name if part == "PLACEHOLDER" else part for part in config[key]]
            original_run = release.runCommand
            def fixture_run(cfg, key, arguments=()):
                result = subprocess.run(cfg[key] + list(arguments), capture_output=True,
                                        text=True, timeout=cfg.get("commandTimeout", 30))
                if result.returncode:
                    raise RuntimeError(f"isolated {key} failed: {result.stderr[-1200:]}")
                return result.stdout
            release.runCommand = fixture_run
            self.addCleanup(setattr, release, "runCommand", original_run)
            deadline = time.monotonic() + 30
            while True:
                try:
                    release.verifyServed(config, manifest)
                    break
                except Exception:
                    if time.monotonic() >= deadline:
                        logs = subprocess.run(DOCKER + ["logs", name], capture_output=True,
                                              text=True, timeout=10)
                        self.fail("container BIND did not serve its seed: " + logs.stdout[-2000:])
                    time.sleep(0.2)

            next_candidate = root / "n-plus-one"
            next_candidate.mkdir()
            (next_candidate / "db.drill.invalid").write_text(zone(2026103002, "192.0.2.2"))
            state = release.activate(config, next_candidate)
            self.assertEqual("applied", state["phase"])
            active = release.loadManifest(link.resolve())
            release.verifyServed(config, active)
            n2 = root / "n-plus-two-invalid"
            n2.mkdir()
            (n2 / "db.drill.invalid").write_text(zone(2026103003, "192.0.2.3"))
            with self.assertRaises(RuntimeError):
                release.activate(config, n2)
            rolled_back = release.loadManifest(link.resolve())
            self.assertEqual("applied", json.loads((root / "releases/state.json").read_text())["phase"])
            self.assertEqual("192.0.2.2", rolled_back["zones"][0]["checkValue"])
            release.verifyServed(config, rolled_back)
            served_soa = subprocess.run(
                ["dig", "@127.0.0.1", "-p", str(port), "drill.invalid", "SOA", "+short"],
                capture_output=True, text=True, timeout=5).stdout.split()[2]
            self.assertEqual(str(rolled_back["zones"][0]["serial"]), served_soa)

            # Exercise the post-switch rollback path too: let N+3 load, fail
            # its served-health proof, and require restoration of N+1's data.
            n3 = root / "n-plus-three-health-failure"
            n3.mkdir()
            (n3 / "db.drill.invalid").write_text(zone(2026103004, "192.0.2.4"))
            def fail_after_load(cfg, candidate_manifest):
                release.verifyServed(cfg, candidate_manifest)
                if candidate_manifest["zones"][0]["checkValue"] == "192.0.2.4":
                    raise RuntimeError("injected post-load health failure")
            with self.assertRaises(RuntimeError):
                release.activate(config, n3, health=fail_after_load)
            restored = release.loadManifest(link.resolve())
            self.assertEqual("192.0.2.2", restored["zones"][0]["checkValue"])
            release.verifyServed(config, restored)
            self.assertEqual(str(restored["zones"][0]["serial"]), subprocess.run(
                ["dig", "@127.0.0.1", "-p", str(port), "drill.invalid", "SOA", "+short"],
                capture_output=True, text=True, timeout=5).stdout.split()[2])


if __name__ == "__main__":
    unittest.main()
