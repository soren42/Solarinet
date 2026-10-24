#!/usr/bin/env python3
"""Prove the dashboard CA client requires trusted, hostname-valid HTTPS."""
from __future__ import annotations

import http.server
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import tempfile
import threading
import unittest


ROOT = Path(__file__).resolve().parents[2]
PHP = shutil.which("php")
OPENSSL = shutil.which("openssl")


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self) -> None:  # noqa: N802 - stdlib handler API
        body = b'{"status":"ok"}' if self.path == "/health" else b'{"crts":[]}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format: str, *_args: object) -> None:
        pass


@unittest.skipUnless(PHP and OPENSSL, "PHP and OpenSSL are required")
class CaTlsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solari-ca-tls-")
        self.addCleanup(self.temp.cleanup)
        cert = Path(self.temp.name) / "server.pem"
        key = Path(self.temp.name) / "server.key"
        subprocess.run(
            [OPENSSL, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-keyout", str(key), "-out", str(cert), "-days", "1",
             "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        self.cert = cert
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(cert), str(key))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.thread.join, timeout=2)
        self.addCleanup(self.server.shutdown)
        self.url = f"https://127.0.0.1:{self.server.server_port}"

    def query(self, *, url: str | None = None, ca_file: Path | None = None) -> dict:
        env = os.environ.copy()
        env["STEPCA_URL"] = url or self.url
        env.pop("STEPCA_CA_FILE", None)
        if ca_file is not None:
            env["STEPCA_CA_FILE"] = str(ca_file)
        php = r'''class Db { public static function rows(string $q): array { return []; } }
require "dashboard/api/lib/Ca.php";
echo json_encode(Ca::summary()["stepCa"], JSON_THROW_ON_ERROR);'''
        result = subprocess.run(
            [PHP, "-r", php], cwd=ROOT, env=env, check=True,
            text=True, capture_output=True, timeout=10,
        )
        return json.loads(result.stdout)

    def test_untrusted_certificate_is_rejected(self) -> None:
        self.assertFalse(self.query()["available"])

    def test_explicit_ca_bundle_allows_trusted_hostname(self) -> None:
        result = self.query(ca_file=self.cert)
        self.assertTrue(result["available"])
        self.assertEqual(result["status"], "ok")

    def test_non_https_endpoint_is_rejected(self) -> None:
        self.assertFalse(self.query(url="http://127.0.0.1:1")["available"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
