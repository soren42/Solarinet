#!/usr/bin/env python3
"""Exercise enrollment installer validation against a local mock endpoint."""
import http.server
import json
import os
from pathlib import Path
import socketserver
import subprocess
import tempfile
import threading
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "deploy/enrollment/solari-enroll.sh"


def run(*args, input_text=None):
    return subprocess.run(args, input=input_text, text=True, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class EnrollmentInstallTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="solari-enroll-install-")
        cls.work = Path(cls.tmp.name)
        cls.ca_key = cls.work / "ca.key"
        cls.ca_pem = cls.work / "ca.pem"
        cls.wrong_key = cls.work / "wrong.key"
        cls.wrong_csr = cls.work / "wrong.csr"
        cls.wrong_cert = cls.work / "wrong.pem"
        cls.foreign_key = cls.work / "foreign-ca.key"
        cls.foreign_ca = cls.work / "foreign-ca.pem"
        run("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
            "ec_paramgen_curve:P-256", "-nodes", "-keyout", str(cls.ca_key),
            "-out", str(cls.ca_pem), "-days", "2", "-subj", "/CN=Test CA")
        run("openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
            "ec_paramgen_curve:P-256", "-out", str(cls.wrong_key))
        run("openssl", "req", "-new", "-key", str(cls.wrong_key), "-out",
            str(cls.wrong_csr), "-subj", "/CN=client.other.test")
        run("openssl", "x509", "-req", "-in", str(cls.wrong_csr), "-CA",
            str(cls.ca_pem), "-CAkey", str(cls.ca_key), "-CAcreateserial",
            "-out", str(cls.wrong_cert), "-days", "1")
        run("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
            "ec_paramgen_curve:P-256", "-nodes", "-keyout", str(cls.foreign_key),
            "-out", str(cls.foreign_ca), "-days", "2", "-subj", "/CN=Foreign CA")
        cls.ca_text = cls.ca_pem.read_text()
        cls.foreign_ca_text = cls.foreign_ca.read_text()
        cls.wrong_cert_text = cls.wrong_cert.read_text()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, mode):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if self.path.endswith("/ack"):
                    self.server.ack_calls += 1
                    if mode in ("ack-first-fails", "resume-different") and self.server.ack_calls == 1:
                        self.send_response(503)
                        self.end_headers()
                        return
                    self.server.acknowledged = payload.get("token") == "test-token"
                    body = json.dumps({"status": "acknowledged"}).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if mode.startswith("mismatch"):
                    cert = self.server.wrong_cert_text
                else:
                    self.server.submit_calls += 1
                    cert_path = self.server.work / f"issued-{mode}.pem"
                    if not cert_path.exists() or (mode == "resume-different" and self.server.submit_calls > 1):
                        csr_path = self.server.work / f"submitted-{mode}.csr"
                        csr_path.write_text(payload["csrPem"])
                        run("openssl", "x509", "-req", "-in", str(csr_path), "-CA",
                            str(self.server.ca_pem), "-CAkey", str(self.server.ca_key),
                            "-CAcreateserial", "-out", str(cert_path), "-days", "1")
                    cert = cert_path.read_text()
                ca = self.server.foreign_ca_text if mode == "untrusted" else self.server.ca_text
                body = json.dumps({"nodeId": "1234", "cert": cert, "ca": ca}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_args):
                pass

        with socketserver.TCPServer(("127.0.0.1", 0), Handler) as server:
            server.work = self.work
            server.ca_pem = self.ca_pem
            server.ca_key = self.ca_key
            server.ca_text = self.ca_text
            server.foreign_ca_text = self.foreign_ca_text
            server.wrong_cert_text = self.wrong_cert_text
            server.acknowledged = False
            server.ack_calls = 0
            server.submit_calls = 0
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            conf = self.work / f"conf-{mode}"
            conf.mkdir()
            config = conf / "client.conf"
            config.write_text("[identity]\nnodeId = 0\n")
            if mode in ("mismatch-existing", "force-valid", "force-install-fails"):
                (conf / "node.key").write_text(self.wrong_key.read_text())
                (conf / "node.pem").write_text(self.wrong_cert_text)
                (conf / "ca.pem").write_text(self.ca_text)
                config.write_text("[identity]\nnodeId = 77\n")
            args = ["bash", str(SCRIPT), "--role", "client", "--token", "test-token",
                    "--server", f"http://127.0.0.1:{server.server_address[1]}",
                    "--fqdn", f"{mode}.test", "--conf-dir", str(conf),
                    "--config", str(config)]
            if mode in ("mismatch-existing", "force-valid"):
                args.append("--force")
            run_env = None
            if mode == "force-install-fails":
                args.append("--force")
                bin_dir = self.work / "fail-mv-bin"
                bin_dir.mkdir(exist_ok=True)
                mv = bin_dir / "mv"
                mv.write_text("#!/bin/sh\nif [ \"$1\" = \"-f\" ] && [ \"$3\" = \"" + str(conf / "node.pem") + "\" ]; then exit 1; fi\nexec /bin/mv \"$@\"\n")
                mv.chmod(0o755)
                run_env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}")
            proc = subprocess.run(
                args, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=run_env)
            if mode in ("ack-first-fails", "resume-different"):
                installed_cert = (conf / "node.pem").read_text()
                installed_ca = (conf / "ca.pem").read_text()
                node_config = config.read_text()
                resumed = subprocess.run(
                    args + ["--resume"], text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertEqual((conf / "node.pem").read_text(), installed_cert)
                self.assertEqual((conf / "ca.pem").read_text(), installed_ca)
                self.assertEqual(config.read_text(), node_config)
                if mode == "ack-first-fails":
                    self.assertEqual(resumed.returncode, 0,
                                     resumed.stdout + resumed.stderr)
                    self.assertEqual(server.ack_calls, 2)
                    self.assertTrue(server.acknowledged)
                else:
                    self.assertNotEqual(resumed.returncode, 0)
                    self.assertIn("different certificate", resumed.stderr)
                    self.assertEqual(server.ack_calls, 1)
                    self.assertFalse(server.acknowledged)
            server.shutdown()
            thread.join(timeout=2)

        cert_path = conf / "node.pem"
        ca_path = conf / "ca.pem"
        if mode in ("valid", "force-valid", "ack-first-fails", "resume-different"):
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertTrue(cert_path.is_file())
            self.assertTrue(ca_path.is_file())
            self.assertIn("nodeId = 1234", config.read_text())
            if mode == "force-valid":
                self.assertNotEqual((conf / "node.key").read_text(), self.wrong_key.read_text())
                run("openssl", "pkey", "-in", str(conf / "node.key"), "-check", "-noout")
            if mode in ("valid", "force-valid"):
                self.assertTrue(server.acknowledged, "successful install must acknowledge token use")
            elif mode == "ack-first-fails":
                self.assertIn("server acknowledgement failed", proc.stderr)
                self.assertTrue(server.acknowledged, "resume must retry the token acknowledgement")
            else:
                self.assertIn("server acknowledgement failed", proc.stderr)
                self.assertFalse(server.acknowledged)
            run("openssl", "verify", "-CAfile", str(ca_path), str(cert_path))
        else:
            self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            if mode in ("mismatch-existing", "force-install-fails"):
                self.assertEqual((conf / "node.key").read_text(), self.wrong_key.read_text())
                self.assertEqual(cert_path.read_text(), self.wrong_cert_text)
                self.assertEqual(ca_path.read_text(), self.ca_text)
                self.assertIn("nodeId = 77", config.read_text())
                self.assertFalse(server.acknowledged)
                if mode == "force-install-fails":
                    self.assertIn("cannot install the issued certificate", proc.stderr)
            else:
                self.assertFalse(cert_path.exists(), "mismatched cert must not be installed")
                self.assertFalse(ca_path.exists(), "CA must not be installed on failed enrollment")
                self.assertIn("nodeId = 0", config.read_text())
            self.assertFalse(server.acknowledged, "rejected material must not acknowledge the token")

    def test_valid_certificate_and_key_are_installed(self):
        self.run_case("valid")

    def test_certificate_for_another_key_is_rejected_before_install(self):
        self.run_case("mismatch")

    def test_failed_forced_reenrollment_preserves_installed_identity(self):
        self.run_case("mismatch-existing")

    def test_successful_forced_renewal_commits_matching_new_key_and_certificate(self):
        self.run_case("force-valid")

    def test_forced_renewal_rolls_back_key_and_ca_when_leaf_install_fails(self):
        self.run_case("force-install-fails")

    def test_certificate_from_untrusted_issuer_is_rejected_before_install(self):
        self.run_case("untrusted")

    def test_resume_retries_ack_without_replacing_installed_identity(self):
        self.run_case("ack-first-fails")

    def test_resume_rejects_different_certificate_and_preserves_identity(self):
        self.run_case("resume-different")

    def test_force_and_resume_conflict_is_rejected_before_key_change(self):
        conf = self.work / "conf-force-resume-conflict"
        conf.mkdir()
        key = conf / "node.key"
        cert = conf / "node.pem"
        ca = conf / "ca.pem"
        key.write_text(self.wrong_key.read_text())
        cert.write_text(self.wrong_cert_text)
        ca.write_text(self.ca_text)
        before = (key.read_text(), cert.read_text(), ca.read_text())
        proc = subprocess.run(
            ["bash", str(SCRIPT), "--role", "client", "--token", "test-token",
             "--force", "--resume", "--conf-dir", str(conf)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("cannot be combined", proc.stderr)
        self.assertEqual((key.read_text(), cert.read_text(), ca.read_text()), before)


if __name__ == "__main__":
    unittest.main()
