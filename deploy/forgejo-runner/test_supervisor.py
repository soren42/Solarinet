from __future__ import annotations

import importlib.util
import os
import pty
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("render_user_data", HERE / "render-user-data.py")
render_module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(render_module)


def wait_until(predicate, timeout: float = 5) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()


class ForgejoRunnerSupervisorTests(unittest.TestCase):
    def test_registration_advertises_workflow_label_name(self):
        registration = (HERE / "register-ephemeral.sh").read_text()
        guest = (HERE / "guest-runner.sh").read_text()
        workflow = (HERE.parent.parent / ".forgejo/workflows/ci.yml").read_text()
        self.assertIn("--labels solarinet-ci-host \\", registration)
        self.assertNotIn("--labels solarinet-ci-host:host", registration)
        self.assertIn("--label solarinet-ci-host:host", guest)
        self.assertIn("runs-on: solarinet-ci-host", workflow)

    def test_cloud_init_contains_scoped_single_job_credentials_and_bootstrap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            token_file = root / "token"
            uuid_file = root / "uuid"
            bootstrap_file = root / "bootstrap.sh"
            token_file.write_text("534f4c4152494349" + "a" * 24)
            uuid_file.write_text("12345678-1234-1234-1234-1234567890ab")
            bootstrap_file.write_text((HERE / "guest-runner.sh").read_text())
            result = render_module.render(token_file, uuid_file, bootstrap_file)

        import yaml

        config = yaml.safe_load(result.removeprefix("#cloud-config\n"))
        files = {entry["path"]: entry for entry in config["write_files"]}
        self.assertEqual(files["/etc/solarinet-ci/runner-token"]["owner"], "root:root")
        self.assertEqual(files["/etc/solarinet-ci/runner-token"]["permissions"], "0600")
        self.assertEqual(files["/etc/solarinet-ci/runner-uuid"]["owner"], "root:root")
        self.assertEqual(files["/etc/solarinet-ci/runner-uuid"]["permissions"], "0600")
        bootstrap = files["/usr/local/sbin/solarinet-ci-guest-bootstrap"]["content"]
        self.assertIn("chown runner:runner /etc/solarinet-ci/runner-token", bootstrap)
        self.assertIn("forgejo-runner one-job", bootstrap)
        self.assertIn("SOLARINET_CI_EXIT", bootstrap)
        self.assertIn("single task poller successfully fetched one task", bootstrap)
        self.assertIn("SOLARINET_CI_STARTED", bootstrap)
        self.assertIn("serial-getty@ttyS0.service", bootstrap)
        self.assertIn("node_version='24.10.0'", bootstrap)
        self.assertIn("2642f4428869aca32443660fd71b3918e2be1277a899bdcaeb64c93b54b5af17", bootstrap)
        self.assertIn("node --version", bootstrap)
        self.assertIn("guest_status runner-download", bootstrap)
        self.assertIn("guest_status node-download", bootstrap)
        self.assertIn("guest_status docker-start", bootstrap)
        self.assertIn("SOLARINET_CI_RUNNER_CONNECTING", bootstrap)
        self.assertIn("sudo -n /usr/bin/tee /dev/ttyS0", bootstrap)
        self.assertEqual(config["runcmd"], [["bash", "/usr/local/sbin/solarinet-ci-guest-bootstrap"]])
        self.assertFalse(config["package_update"])
        self.assertNotIn("packages", config)
        self.assertIn("SOLARINET_CI_STAGE=cloud-init-bootcmd", config["bootcmd"][0][2])
        self.assertEqual(len(config["bootcmd"]), 1)
        self.assertIn("NOPASSWD:ALL", config["users"][0]["sudo"][0])
        self.assertIn("apt-get", bootstrap)
        self.assertIn("guest_status package-update", bootstrap)
        self.assertIn("guest_status package-install", bootstrap)
        self.assertIn("guest_status packages-installed", bootstrap)
        self.assertIn("iproute2", bootstrap)
        self.assertIn("nftables", bootstrap)

    def test_rejects_invalid_registration_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            token_file = root / "token"
            uuid_file = root / "uuid"
            bootstrap_file = root / "bootstrap.sh"
            token_file.write_text("a" * 40)
            uuid_file.write_text("12345678-1234-1234-1234-1234567890ab")
            bootstrap_file.write_text("#!/bin/bash\n")
            with self.assertRaisesRegex(ValueError, "token"):
                render_module.render(token_file, uuid_file, bootstrap_file)

    def test_deployment_shell_scripts_parse(self):
        scripts = (
            "register-ephemeral.sh",
            "guest-runner.sh",
            "supervisor.sh",
            "install-supervisor.sh",
            "install-host-network.sh",
            "install-cesium-registration-key.sh",
            "test_guard_namespace.sh",
        )
        subprocess.run(["bash", "-n", *(str(HERE / script) for script in scripts)], check=True)

    def test_registration_command_is_fixed_and_repository_scoped(self):
        source = (HERE / "register-ephemeral.sh").read_text()
        self.assertIn("--scope jason/Solarinet", source)
        self.assertIn("--ephemeral", source)
        self.assertRegex(source, r"534f4c4152494349\[0-9a-f\]\{24\}")

    def test_supervisor_pins_runner_interface_to_guarded_tap(self):
        source = (HERE / "supervisor.sh").read_text()
        self.assertIn("target=ci-tap0", source)
        self.assertIn("<target dev='ci-tap0'", source)
        self.assertIn("seed_iso=\"$image_dir/seed.iso\"", source)
        self.assertIn('chown qemu:qemu "$image_dir/overlay.qcow2"', source)
        self.assertIn("timeout_seconds=86400", source)
        self.assertIn("bootstrap_timeout_seconds=2700", source)
        self.assertIn("timeout_kind=bootstrap", source)
        self.assertIn('timeout_kind=job', source)
        self.assertIn('install -d -o root -g root -m 0755 "$image_dir"', source)
        self.assertIn("StrictHostKeyChecking=yes", source)
        self.assertIn("UserKnownHostsFile=/etc/solarinet-ci/known_hosts", source)
        self.assertIn("timeout 60 ssh", source)
        self.assertIn("virsh ttyconsole", source)
        self.assertIn("job-exit", source)
        self.assertIn("SOLARINET_CI_EXIT", (HERE / "serial-capture.py").read_text())
        self.assertIn("run_cancellable", source)
        self.assertIn("job_started=0", source)
        self.assertIn('stty -F "$serial_device" raw -echo', source)

    def test_cesium_authorizes_only_forced_noninteractive_registration_command(self):
        source = (HERE / "install-cesium-registration-key.sh").read_text()
        self.assertIn('from=\\"$benzene_source_ip\\",restrict,command=\\"/usr/local/sbin/solarinet-ci-register-ephemeral\\"', source)
        self.assertIn("benzene_source_ip=10.5.2.50", source)
        self.assertIn("expected one ed25519 public key", source)
        self.assertIn("sudo -u jason /usr/bin/sudo -n -u forgejo /bin/bash -c true", source)
        self.assertIn("/usr/bin/timeout 10 /usr/bin/cat", (HERE / "register-ephemeral.sh").read_text())
        self.assertIn("solarinet-ci-nftables.service.d.conf", (HERE / "install-supervisor.sh").read_text())

    def test_cesium_host_key_is_pinned_for_benzene_ssh(self):
        known_hosts = (HERE / "cesium.known_hosts").read_text()
        self.assertIn(" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5", known_hosts)
        result = subprocess.run(
            ["ssh-keygen", "-F", "cesium.akoria.net", "-f", str(HERE / "cesium.known_hosts")],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("cesium.akoria.net found", result.stdout)
        source = (HERE / "install-supervisor.sh").read_text()
        self.assertIn("known_hosts", source)

    def test_supervisor_installer_does_not_start_global_nftables_service(self):
        source = (HERE / "solarinet-ci-guard.service").read_text()
        self.assertIn("Wants=network-pre.target", source)
        self.assertNotIn("Wants=nftables.service", source)
        supervisor = (HERE / "solarinet-ci-supervisor.service").read_text()
        self.assertIn("WantedBy=multi-user.target solarinet-ci-network.service", supervisor)

    def test_nftables_start_reapplies_guard_before_network_start_without_cycle(self):
        source = (HERE / "solarinet-ci-nftables.service.d.conf").read_text()
        self.assertIn("Wants=solarinet-ci-network.service", source)
        self.assertNotIn("Wants=solarinet-ci-guard.service", source)
        self.assertIn("Before=solarinet-ci-network.service", source)
        self.assertNotIn("Before=solarinet-ci-guard.service", source)
        self.assertIn("ExecStartPre=/usr/bin/test -r /etc/nftables/rules/main.nft", source)
        self.assertIn("ExecStartPost=/usr/sbin/nft -f /etc/solarinet-ci/solarinet-ci-guard.nft", source)
        installer = (HERE / "install-supervisor.sh").read_text()
        self.assertIn("nftables.service.d/solarinet-ci-guard.conf", installer)
        self.assertIn("systemctl reload solarinet-ci-guard.service", installer)
        self.assertIn("solarinet-ci.network.xml", installer)
        self.assertIn("virsh net-define /etc/solarinet-ci/solarinet-ci.network.xml", installer)
        self.assertIn("virsh net-autostart --disable solarinet-ci", installer)
        self.assertIn("systemctl restart solarinet-ci-network.service", installer)

    def test_network_unit_checks_the_firewall_bridge_name(self):
        source = (HERE / "solarinet-ci-network.service").read_text()
        self.assertIn("ExecStartPre=", source)
        self.assertIn("^Bridge:.*virbr-ci0$", source)

    def test_serial_capture_caps_logs_and_extracts_crlf_markers_under_flood(self):
        with tempfile.TemporaryDirectory(dir="/var/tmp") as temporary:
            root = Path(temporary)
            start_path = root / "started"
            exit_path = root / "exit"
            log_path = root / "serial.log"
            master, slave = pty.openpty()
            device = os.ttyname(slave)
            process = subprocess.Popen(
                [
                    "python3",
                    str(HERE / "serial-capture.py"),
                    device,
                    str(log_path),
                    str(start_path),
                    str(exit_path),
                ]
            )
            try:
                time.sleep(0.05)
                os.write(master, b"\nSOLARINET_CI_STARTED\r\n")
                payload = b"x" * 65536
                for _ in range(80):
                    view = memoryview(payload)
                    while view:
                        view = view[os.write(master, view) :]
                os.write(master, b"\nSOLARINET_CI_EXIT=0\r\n")
                self.assertTrue(
                    wait_until(lambda: start_path.exists() and exit_path.exists()),
                    "serial markers were not extracted",
                )
                self.assertEqual(exit_path.read_text(), "0")
                self.assertTrue(wait_until(lambda: log_path.exists() and log_path.stat().st_size <= 4 * 1024 * 1024))
            finally:
                os.close(master)
                os.close(slave)
                process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
