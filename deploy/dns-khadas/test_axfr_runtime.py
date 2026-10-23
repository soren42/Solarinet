#!/usr/bin/env python3
"""Opt-in loopback BIND primary/secondary transfer and refresh test.

Run with SOLARI_DNS_AXFR_TEST=1 on a Linux test host with BIND and passwordless
sudo. All DNS listeners bind to 127.0.0.1 on ephemeral ports. The fixture runs
under /var/cache/bind for compatibility with the standard named AppArmor profile.
"""
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import uuid


ENABLED = os.environ.get("SOLARI_DNS_AXFR_TEST") == "1"


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def query(server_port, zone, record_type):
    return subprocess.run(
        ["dig", "+time=1", "+tries=1", "+short", "@127.0.0.1", "-p",
         str(server_port), zone, record_type],
        capture_output=True, text=True, timeout=3)


def serial(server_port, zone):
    result = query(server_port, zone, "SOA")
    fields = result.stdout.split()
    if result.returncode != 0 or len(fields) < 3 or not fields[2].isdigit():
        return None
    return fields[2]


@unittest.skipUnless(ENABLED, "set SOLARI_DNS_AXFR_TEST=1 for isolated BIND transfer test")
class AxfrRuntimeTest(unittest.TestCase):
    zone = "axfr-fixture.invalid"

    def setUp(self):
        for command in ("named", "named-checkconf", "named-checkzone", "dig"):
            if shutil.which(command) is None:
                self.skipTest(f"{command} is required")
        probe = subprocess.run(["sudo", "-n", "true"], capture_output=True, timeout=5)
        if probe.returncode:
            self.skipTest("passwordless sudo is required for AppArmor-compatible fixture setup")
        self.root = pathlib.Path("/var/cache/bind") / f"solarinet-axfr-{uuid.uuid4().hex}"
        subprocess.run(["sudo", "-n", "install", "-d", "-m", "0755", "-o",
                        str(os.getuid()), "-g", str(os.getgid()), str(self.root)],
                       check=True, timeout=5)
        self.addCleanup(self.cleanup_runtime_dir)
        self.primary_port = free_port()
        self.secondary_port = free_port()
        while self.secondary_port == self.primary_port:
            self.secondary_port = free_port()
        self.primary_runtime = self.root
        self.secondary_runtime = self.root
        self.secondary_zones = self.root
        self.primary_zone = self.root / "primary-zone.db"
        self.secondary_file = self.root / f"db.{self.zone}"
        self.primary_config = self.root / "primary.conf"
        self.secondary_config = self.root / "secondary.conf"
        self.primary_log = self.root / "primary.log"
        self.secondary_log = self.root / "secondary.log"
        self.primary_pid_file = self.root / "primary.pid"
        self.secondary_pid_file = self.root / "secondary.pid"
        self.write_zone(2026102101, "192.0.2.21")
        self.primary_config.write_text(
            self.options(self.primary_runtime, self.primary_port, self.primary_pid_file,
                         self.root / "primary.session-key")
            + f'zone "{self.zone}" {{ type primary; file "{self.primary_zone}"; '
              'allow-transfer { 127.0.0.1; }; also-notify '
              f'{{ 127.0.0.1 port {self.secondary_port}; }}; notify yes; }};\n',
            encoding="utf-8")
        self.secondary_config.write_text(
            self.options(self.secondary_runtime, self.secondary_port,
                         self.secondary_pid_file,
                         self.root / "secondary.session-key")
            + f'zone "{self.zone}" {{ type secondary; primaries '
              f'{{ 127.0.0.1 port {self.primary_port}; }}; '
              f'file "{self.secondary_file}"; }};\n',
            encoding="utf-8")
        for path in (self.primary_zone, self.primary_config, self.secondary_config):
            path.chmod(0o644)
        self.assert_command(["named-checkconf", "-z", str(self.primary_config)])
        self.assert_command(["named-checkconf", str(self.secondary_config)])
        self.assert_command(["named-checkzone", self.zone, str(self.primary_zone)])
        self.primary_process = None
        self.secondary_process = None
        self.addCleanup(self.stop_servers)

    @staticmethod
    def options(runtime, port, pid_file, session_key):
        return ("options {\n"
                f'  directory "{runtime}";\n'
                f"  listen-on port {port} {{ 127.0.0.1; }};\n"
                "  listen-on-v6 { none; };\n"
                "  recursion no;\n"
                "  allow-query { 127.0.0.1; };\n"
                f'  pid-file "{pid_file}";\n'
                f'  session-keyfile "{session_key}";\n'
                "};\n")

    def write_zone(self, serial_value, address):
        self.primary_zone.write_text(
            f"$ORIGIN {self.zone}.\n$TTL 60\n"
            f"@ IN SOA ns.{self.zone}. hostmaster.{self.zone}. "
            f"( {serial_value} 60 60 300 60 )\n"
            f"@ IN NS ns.{self.zone}.\n@ IN A {address}\n"
            "ns IN A 127.0.0.1\n",
            encoding="utf-8")

    def assert_command(self, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertEqual(0, result.returncode,
                         f"{command[0]} failed: {(result.stderr or result.stdout)[-1000:]}")

    def cleanup_runtime_dir(self):
        shutil.rmtree(self.root, ignore_errors=True)
        subprocess.run(["sudo", "-n", "rmdir", str(self.root)],
                       capture_output=True, check=False, timeout=5)

    def start_server(self, config, pid_file, log_path):
        log = log_path.open("w", encoding="utf-8")
        process = subprocess.Popen(
            [shutil.which("named"), "-g", "-c", str(config), "-n", "1", "-U", "1"],
            stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if process.poll() is not None:
                log.close()
                self.fail(f"named exited early: {log_path.read_text()[-2000:]}")
            if pid_file.exists():
                return process, log
            time.sleep(0.05)
        process.terminate()
        process.wait(timeout=5)
        log.close()
        self.fail(f"named did not create its pid file: {log_path.read_text()[-2000:]}")

    def stop_servers(self):
        for role, process, pid_file, log_path in (
                ("secondary", getattr(self, "secondary_process", None),
                 getattr(self, "secondary_pid_file", None),
                 getattr(self, "secondary_log", None)),
                ("primary", getattr(self, "primary_process", None),
                 getattr(self, "primary_pid_file", None),
                 getattr(self, "primary_log", None))):
            if process is not None:
                if process.poll() is None and pid_file and pid_file.exists():
                    pid = pid_file.read_text(encoding="ascii").strip()
                    process.send_signal(15)
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=5)
            log_handle = getattr(self, role + "_log_handle", None)
            if log_handle is not None:
                log_handle.close()

    def wait_for_convergence(self, expected_serial, expected_address, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if serial(self.secondary_port, self.zone) == str(expected_serial):
                answer = query(self.secondary_port, self.zone, "A")
                if expected_address in answer.stdout.split():
                    return
            time.sleep(0.1)
        self.fail(
            f"secondary did not converge to SOA {expected_serial} and {expected_address}; "
            f"primary log: {self.primary_log.read_text()[-1500:]}; "
            f"secondary log: {self.secondary_log.read_text()[-1500:]}")

    def test_axfr_and_notify_update_a_live_secondary(self):
        self.primary_process, self.primary_log_handle = self.start_server(
            self.primary_config, self.primary_pid_file, self.primary_log)
        self.secondary_process, self.secondary_log_handle = self.start_server(
            self.secondary_config, self.secondary_pid_file, self.secondary_log)
        self.wait_for_convergence(2026102101, "192.0.2.21")

        self.write_zone(2026102102, "192.0.2.22")
        self.assert_command(["named-checkzone", self.zone, str(self.primary_zone)])
        primary_pid = self.primary_pid_file.read_text(encoding="ascii").strip()
        self.assertEqual(int(primary_pid), self.primary_process.pid)
        self.primary_process.send_signal(1)
        self.wait_for_convergence(2026102102, "192.0.2.22")


if __name__ == "__main__":
    unittest.main()
