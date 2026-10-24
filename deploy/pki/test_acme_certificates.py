from __future__ import annotations

import json
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import acme_certificates as acme


def openssl(*args: str) -> None:
    subprocess.run(["openssl", *args], check=True, capture_output=True, text=True)


class AcmeCertificateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solarinet-acme-test-")
        self.base = pathlib.Path(self.temp.name)
        self.ca_dir = self.base / "ca"
        self.ca_dir.mkdir()
        self.root = self.ca_dir / "root.pem"
        self.root_key = self.ca_dir / "root.key"
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3",
                "-keyout", str(self.root_key), "-out", str(self.root),
                "-subj", "/CN=ACME Test Root",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")

        self.intermediate_key = self.ca_dir / "intermediate.key"
        intermediate_csr = self.ca_dir / "intermediate.csr"
        self.intermediate = self.ca_dir / "intermediate.pem"
        openssl("req", "-new", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(self.intermediate_key), "-out", str(intermediate_csr),
                "-subj", "/CN=ACME Test Intermediate")
        intermediate_ext = self.ca_dir / "intermediate.cnf"
        intermediate_ext.write_text("[v3_ca]\nbasicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n", encoding="utf-8")
        openssl("x509", "-req", "-in", str(intermediate_csr), "-CA", str(self.root),
                "-CAkey", str(self.root_key), "-CAcreateserial", "-days", "2",
                "-out", str(self.intermediate), "-extfile", str(intermediate_ext),
                "-extensions", "v3_ca")
        self._make_leaf("test.internal")

        self.certbot_root = self.base / "certbot"
        archive = self.certbot_root / "archive" / "test.internal"
        live = self.certbot_root / "live"
        archive.mkdir(parents=True)
        live.mkdir()
        live_lineage = live / "test.internal"
        live_lineage.mkdir()
        (live_lineage / "README").write_text("This directory contains Certbot live symlinks.\n", encoding="utf-8")
        self._install_lineage(archive)
        self._link_live_lineage(live_lineage)
        self.webroot = self.base / "webroot"
        self.webroot.mkdir()
        self.target_parent = self.base / "installed"
        self.target_parent.mkdir()
        self.target = self.target_parent / "test-service"
        self.registry = self.base / "registry.json"
        self.config = {
            "schema_version": 1,
            "acme": {
                "directory_url": "https://ca.example.invalid/acme/test/directory",
                "root_ca": str(self.root),
                "webroot": str(self.webroot),
                "certbot_root": str(self.certbot_root),
                "target_root": str(self.target_parent),
            },
            "services": [{
                "id": "test_web",
                "lineage": "test.internal",
                "domains": ["test.internal"],
                "target": str(self.target),
                "check_command": ["/usr/bin/true"],
                "reload_command": ["/usr/bin/true"],
                "group": None,
            }],
        }
        self._write_registry()
        os.chmod(self.root, 0o644)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _make_leaf(self, domain: str, label: str | None = None, extra_san: bool = False,
                   critical_san: bool = False,
                   basic_constraints: str | None = "CA:FALSE", days: int = 7) -> None:
        stem = label or domain
        self.leaf_key = self.ca_dir / f"{stem}.key"
        csr = self.ca_dir / f"{stem}.csr"
        self.leaf = self.ca_dir / f"{stem}.pem"
        openssl("req", "-new", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(self.leaf_key), "-out", str(csr),
                "-subj", f"/CN={domain}")
        ext = self.ca_dir / f"{stem}.cnf"
        extensions = "[server]\n"
        if basic_constraints:
            extensions += f"basicConstraints=critical,{basic_constraints}\n"
        extensions += (
            "keyUsage=critical,digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\n"
            f"subjectAltName={('critical,' if critical_san else '')}DNS:{domain}"
            + (",IP:127.0.0.1\n" if extra_san else "\n")
        )
        ext.write_text(extensions, encoding="utf-8")
        openssl("x509", "-req", "-in", str(csr), "-CA", str(self.intermediate),
                "-CAkey", str(self.intermediate_key), "-CAcreateserial", "-days", str(days),
                "-out", str(self.leaf), "-extfile", str(ext), "-extensions", "server")

    def _install_lineage(self, archive: pathlib.Path, generation: int = 1) -> None:
        (archive / f"cert{generation}.pem").write_bytes(self.leaf.read_bytes())
        (archive / f"chain{generation}.pem").write_bytes(self.intermediate.read_bytes())
        (archive / f"fullchain{generation}.pem").write_bytes(self.leaf.read_bytes() + self.intermediate.read_bytes())
        (archive / f"privkey{generation}.pem").write_bytes(self.leaf_key.read_bytes())
        (archive / f"privkey{generation}.pem").chmod(0o600)

    def _link_live_lineage(self, live_lineage: pathlib.Path, generation: int = 1) -> None:
        for name in ("cert", "chain", "fullchain", "privkey"):
            link = live_lineage / f"{name}.pem"
            link.unlink(missing_ok=True)
            link.symlink_to(f"../../archive/test.internal/{name}{generation}.pem")

    def _write_registry(self) -> None:
        self.registry.write_text(json.dumps(self.config), encoding="utf-8")
        self.registry.chmod(0o600)

    def _env(self, domains: str = "test.internal") -> dict[str, str]:
        return {
            "RENEWED_LINEAGE": str(self.certbot_root / "live" / "test.internal"),
            "RENEWED_DOMAINS": domains,
        }

    @staticmethod
    def _ok(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(args, 0, "", "")

    def test_deploys_valid_lineage_with_atomic_current_pointer_and_safe_modes(self) -> None:
        result = acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)
        self.assertIn("activated certificate", result)
        current = self.target / "current"
        self.assertTrue(current.is_symlink())
        self.assertEqual((current / "fullchain.pem").read_bytes(), self.leaf.read_bytes() + self.intermediate.read_bytes())
        self.assertEqual((current / "privkey.pem").read_bytes(), self.leaf_key.read_bytes())
        self.assertEqual((current / "fullchain.pem").stat().st_mode & 0o777, 0o644)
        self.assertEqual((current / "privkey.pem").stat().st_mode & 0o777, 0o600)

    def test_rejects_unregistered_domains_before_creating_target(self) -> None:
        with self.assertRaisesRegex(acme.AcmeError, "domains do not match"):
            acme.deploy(acme.load_registry(self.registry), self._env("other.internal"), self._ok)
        self.assertFalse(self.target.exists())

    def test_registry_rejects_target_dot_segments_and_duplicate_targets(self) -> None:
        self.config["services"][0]["target"] = str(self.target_parent) + "/../escape"
        self._write_registry()
        with self.assertRaisesRegex(acme.AcmeError, "absolute path"):
            acme.load_registry(self.registry)
        self.config["services"][0]["target"] = str(self.target)
        duplicate = dict(self.config["services"][0])
        duplicate.update(id="other_web", lineage="other-lineage", domains=["other.internal"])
        self.config["services"].append(duplicate)
        self._write_registry()
        with self.assertRaisesRegex(acme.AcmeError, "target paths must be unique"):
            acme.load_registry(self.registry)

    def test_rejects_group_writable_certificate_target_root(self) -> None:
        self.target_parent.chmod(0o770)
        with self.assertRaisesRegex(acme.AcmeError, "group- or world-writable"):
            acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)
        self.assertFalse(self.target.exists())

    def test_rejects_certificate_and_private_key_mismatch(self) -> None:
        archive = self.certbot_root / "archive" / "test.internal"
        other_key = self.ca_dir / "other.key"
        openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256",
                "-out", str(other_key))
        (archive / "privkey1.pem").write_bytes(other_key.read_bytes())
        (archive / "privkey1.pem").chmod(0o600)
        with self.assertRaisesRegex(acme.AcmeError, "do not match"):
            acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)

    def test_rejects_expired_leaf(self) -> None:
        self._make_leaf("test.internal", "expired", days=0)
        archive = self.certbot_root / "archive" / "test.internal"
        self._install_lineage(archive, generation=2)
        self._link_live_lineage(self.certbot_root / "live" / "test.internal", generation=2)
        with self.assertRaisesRegex(acme.AcmeError, "lifetime is below"):
            acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)

    def test_rejects_short_lifetime_leaf(self) -> None:
        self._make_leaf("test.internal", "short-lived", days=2)
        archive = self.certbot_root / "archive" / "test.internal"
        self._install_lineage(archive, generation=2)
        self._link_live_lineage(self.certbot_root / "live" / "test.internal", generation=2)
        with self.assertRaisesRegex(acme.AcmeError, "six-day minimum"):
            acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)

    def test_rejects_group_readable_certbot_private_key(self) -> None:
        key = self.certbot_root / "archive" / "test.internal" / "privkey1.pem"
        key.chmod(0o640)
        with self.assertRaisesRegex(acme.AcmeError, "not be accessible to group"):
            acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)

    def test_rejects_live_artifact_symlink_outside_archive(self) -> None:
        live_cert = self.certbot_root / "live" / "test.internal" / "cert.pem"
        outside = self.base / "outside-cert.pem"
        outside.write_bytes(self.leaf.read_bytes())
        live_cert.unlink()
        live_cert.symlink_to(outside)
        with self.assertRaisesRegex(acme.AcmeError, "outside its matching archive"):
            acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)

    def test_rejects_ip_address_in_dns_registry(self) -> None:
        self.config["services"][0]["domains"] = ["192.0.2.10"]
        self._write_registry()
        with self.assertRaisesRegex(acme.AcmeError, "DNS names, not IP addresses"):
            acme.load_registry(self.registry)

    def test_rejects_lineage_signed_by_another_root(self) -> None:
        other_root = self.ca_dir / "other-root.pem"
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3",
                "-keyout", str(self.ca_dir / "other-root.key"), "-out", str(other_root),
                "-subj", "/CN=Wrong ACME Test Root",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        other_root.chmod(0o644)
        self.config["acme"]["root_ca"] = str(other_root)
        self._write_registry()
        with self.assertRaisesRegex(acme.AcmeError, "openssl failed"):
            acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)

    def test_rejects_tampered_fullchain(self) -> None:
        archive = self.certbot_root / "archive" / "test.internal"
        (archive / "fullchain1.pem").write_bytes(self.intermediate.read_bytes())
        with self.assertRaisesRegex(acme.AcmeError, "fullchain"):
            acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)

    def test_rejects_certificate_with_unregistered_ip_san(self) -> None:
        self._make_leaf("test.internal", "extra-san", extra_san=True)
        archive = self.certbot_root / "archive" / "test.internal"
        self._install_lineage(archive, generation=2)
        self._link_live_lineage(self.certbot_root / "live" / "test.internal", generation=2)
        with self.assertRaisesRegex(acme.AcmeError, "non-DNS Subject Alternative Name"):
            acme.deploy(acme.load_registry(self.registry), self._env(), self._ok)

    def test_accepts_critical_san_extension(self) -> None:
        self._make_leaf("test.internal", "critical-san", critical_san=True)
        self.assertEqual(acme._certificate_sans(self.leaf), {"test.internal"})

    def test_accepts_step_ca_shaped_leaf_without_basic_constraints(self) -> None:
        self._make_leaf("test.internal", "no-basic-constraints", basic_constraints=None)
        acme._require_tls_leaf(self.leaf)

    def test_rejects_leaf_marked_as_ca(self) -> None:
        self._make_leaf("test.internal", "ca-leaf", basic_constraints="CA:TRUE")
        with self.assertRaisesRegex(acme.AcmeError, "must not mark a TLS leaf as a CA"):
            acme._require_tls_leaf(self.leaf)

    def test_failed_reload_restores_previous_certificate_pointer(self) -> None:
        config = acme.load_registry(self.registry)
        acme.deploy(config, self._env(), self._ok)
        old_current = os.readlink(self.target / "current")
        self._make_leaf("test.internal", "renewed")
        archive = self.certbot_root / "archive" / "test.internal"
        self._install_lineage(archive, generation=2)
        self._link_live_lineage(self.certbot_root / "live" / "test.internal", generation=2)
        return_codes = iter((0, 1, 0))

        def fail_reload(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
            code = next(return_codes)
            return subprocess.CompletedProcess(args, code, "", "")

        with self.assertRaisesRegex(acme.AcmeError, "reload failed"):
            acme.deploy(config, self._env(), fail_reload)
        self.assertEqual(os.readlink(self.target / "current"), old_current)

    def test_failed_first_reload_reports_unknown_service_state(self) -> None:
        config = acme.load_registry(self.registry)
        return_codes = iter((0, 1))

        def fail_first_activation(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(args, next(return_codes), "", "reload failed")

        with self.assertRaisesRegex(acme.AcmeError, "no previous release existed and service state may be unknown"):
            acme.deploy(config, self._env(), fail_first_activation)
        self.assertFalse((self.target / "current").exists())
        self.assertFalse((self.target / "current").is_symlink())

    def test_failed_service_check_restores_previous_pointer(self) -> None:
        config = acme.load_registry(self.registry)
        acme.deploy(config, self._env(), self._ok)
        old_current = os.readlink(self.target / "current")
        self._make_leaf("test.internal", "check-failure")
        archive = self.certbot_root / "archive" / "test.internal"
        self._install_lineage(archive, generation=2)
        self._link_live_lineage(self.certbot_root / "live" / "test.internal", generation=2)
        return_codes = iter((1,))
        calls: list[tuple[object, ...]] = []

        def fail_check(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append(args)
            code = next(return_codes)
            return subprocess.CompletedProcess(
                args, code, "", (
                    "nginx: [emerg] invalid certificate path\n"
                    "nginx: configuration file /etc/nginx/nginx.conf test failed"
                    if code else ""
                )
            )

        with self.assertRaisesRegex(acme.AcmeError, "check failed.*invalid certificate path"):
            acme.deploy(config, self._env(), fail_check)
        self.assertEqual(os.readlink(self.target / "current"), old_current)
        self.assertEqual(len(calls), 1, "failed config check must not trigger a service reload")

    def test_failed_restore_reload_reports_degraded_service_state(self) -> None:
        config = acme.load_registry(self.registry)
        acme.deploy(config, self._env(), self._ok)
        old_current = os.readlink(self.target / "current")
        self._make_leaf("test.internal", "restore-failure")
        archive = self.certbot_root / "archive" / "test.internal"
        self._install_lineage(archive, generation=2)
        self._link_live_lineage(self.certbot_root / "live" / "test.internal", generation=2)
        return_codes = iter((0, 1, 1))

        def fail_both_reloads(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(args, next(return_codes), "", "")

        with self.assertRaisesRegex(acme.AcmeError, "previous pointer is active but the service reload failed"):
            acme.deploy(config, self._env(), fail_both_reloads)
        self.assertEqual(os.readlink(self.target / "current"), old_current)

    def test_idempotent_deploy_reuses_verified_release_without_reload(self) -> None:
        config = acme.load_registry(self.registry)
        acme.deploy(config, self._env(), self._ok)

        def unexpected_runner(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
            raise AssertionError("idempotent deployment should not rerun service commands")

        result = acme.deploy(config, self._env(), unexpected_runner)
        self.assertIn("already uses certificate", result)

    def test_dangling_current_pointer_is_replaced_on_deploy(self) -> None:
        config = acme.load_registry(self.registry)
        acme.deploy(config, self._env(), self._ok)
        (self.target / "current").unlink()
        (self.target / "current").symlink_to("releases/missing-release")
        result = acme.deploy(config, self._env(), self._ok)
        self.assertIn("activated certificate", result)
        self.assertTrue((self.target / "current").resolve().is_dir())

    def test_planning_uses_a_dedicated_certbot_store_and_does_not_create_it(self) -> None:
        self.certbot_root = self.base / "not-created"
        self.config["acme"]["certbot_root"] = str(self.certbot_root)
        self._write_registry()
        config = acme.load_registry(self.registry)
        command, environment = acme.enrollment_command(config, "test_web", "/usr/bin/certbot", self.registry)
        self.assertIn("--preferred-challenges", command)
        self.assertIn("http-01", command)
        self.assertIn("--config-dir", command)
        self.assertEqual(environment["REQUESTS_CA_BUNDLE"], str(self.root))
        self.assertFalse(self.certbot_root.exists())

    def test_rejects_public_certbot_store(self) -> None:
        self.config["acme"]["certbot_root"] = "/etc/letsencrypt"
        self._write_registry()
        with self.assertRaisesRegex(acme.AcmeError, "separate Certbot store"):
            acme.load_registry(self.registry)

    def test_rejects_target_outside_configured_target_root(self) -> None:
        self.config["services"][0]["target"] = str(self.base / "outside-target")
        self._write_registry()
        with self.assertRaisesRegex(acme.AcmeError, "direct child of target_root"):
            acme.load_registry(self.registry)

    def test_renew_command_is_scoped_and_can_force_pilot_renewal(self) -> None:
        command, environment = acme.renewal_command(
            acme.load_registry(self.registry), "/usr/bin/certbot", self.registry, force=True
        )
        self.assertEqual(command[command.index("renew")], "renew")
        self.assertIn("--no-random-sleep-on-renew", command)
        self.assertIn("--no-directory-hooks", command)
        self.assertEqual(command[command.index("--pre-hook") + 1], "")
        self.assertEqual(command[command.index("--post-hook") + 1], "")
        self.assertNotIn("--config", command)
        self.assertIn(str(self.certbot_root), command)
        self.assertIn("--force-renewal", command)
        self.assertEqual(environment["REQUESTS_CA_BUNDLE"], str(self.root))

    def test_certbot_version_gate_rejects_pre_four_release(self) -> None:
        with patch.object(acme, "_run", return_value=b"certbot 3.3.0"):
            with self.assertRaisesRegex(acme.AcmeError, "Certbot 4.0.0 or later"):
                acme._require_certbot4("/usr/bin/certbot")

    def test_renew_health_check_fails_when_hook_did_not_deploy(self) -> None:
        config = acme.load_registry(self.registry)
        with self.assertRaisesRegex(acme.AcmeError, "did not activate"):
            acme._run_and_verify_certbot(
                ["/usr/bin/certbot", "renew"], {}, self._ok, config, config["services"]
            )

    def test_private_renewal_store_must_match_registered_lineages(self) -> None:
        renewal = self.certbot_root / "renewal"
        renewal.mkdir()
        (renewal / "test.internal.conf").write_text("renewalparams = {}\n", encoding="utf-8")
        acme._validate_private_renewal_store(acme.load_registry(self.registry))
        (renewal / "extra.conf").write_text("renewalparams = {}\n", encoding="utf-8")
        with self.assertRaisesRegex(acme.AcmeError, "do not exactly match"):
            acme._validate_private_renewal_store(acme.load_registry(self.registry))

    def test_enrollment_command_is_inert_until_issue_mode(self) -> None:
        command, environment = acme.enrollment_command(
            acme.load_registry(self.registry), "test_web", "/usr/bin/certbot", self.registry
        )
        self.assertEqual(command[command.index("certonly")], "certonly")
        self.assertIn("--no-directory-hooks", command)
        self.assertEqual(command[command.index("--pre-hook") + 1], "")
        self.assertEqual(command[command.index("--post-hook") + 1], "")
        self.assertNotIn("--config", command)
        self.assertIn("--deploy-hook", command)
        self.assertEqual(environment, {"REQUESTS_CA_BUNDLE": str(self.root)})


if __name__ == "__main__":
    unittest.main()
