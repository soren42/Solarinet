#!/usr/bin/env python3
"""Exercise the public enrollment HTTP routes over TLS through PHP to a ctl socket.

The operator bridge is a protocol fixture and no SoR, CA, or production socket
is used. This verifies the real API front controller, request validation,
SolariCtl framing/reply parsing, and HTTPS transport as one local path.
"""
from __future__ import annotations

import http.client
import http.server
import json
import os
import socket
import ssl
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import quote, unquote


ROOT = Path(__file__).resolve().parents[2]
API_DIR = ROOT / "dashboard" / "api"
TOKEN = "ab" * 32
FP = ":".join(["12"] * 32)
CSR = "-----BEGIN CERTIFICATE REQUEST-----\nfixture csr body\n-----END CERTIFICATE REQUEST-----\n"


def reserve_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class CtlFixture:
    def __init__(self, path: Path):
        self.path = str(path)
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.path)
        self.listener.listen(8)
        self.listener.settimeout(0.2)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self) -> None:
        while not self.stop.is_set():
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with conn:
                conn.settimeout(2)
                request = b""
                while b"\n" not in request and len(request) < 16384:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    request += chunk
                line = request.split(b"\n", 1)[0].decode("ascii", "strict")
                with self.lock:
                    self.lines.append(line)
                verb, _, raw_args = line.partition(" ")
                args = {}
                for token in raw_args.split():
                    key, sep, value = token.partition("=")
                    if sep:
                        args[key] = unquote(value)
                if verb == "ENROLL_ACK":
                    reply = "OK status=acknowledged\n"
                elif verb == "ENROLL_SUBMIT" and args.get("host") == "approved.akoria.net":
                    reply = "OK status=approved nodeId=42 cert=" + quote("LEAF PEM\n", safe="") + " ca=" + quote("CA PEM\n", safe="") + "\n"
                elif verb == "ENROLL_SUBMIT" and args.get("host") == "incomplete.akoria.net":
                    reply = "OK status=approved nodeId=42 cert=" + quote("LEAF PEM\n", safe="") + "\n"
                else:
                    reply = "OK status=pending\n"
                conn.sendall(reply.encode("ascii"))

    def close(self) -> None:
        self.stop.set()
        self.listener.close()
        self.thread.join(timeout=2)
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass


def start_php(port: int, ctl_path: str) -> subprocess.Popen:
    env = os.environ.copy()
    env["SOLARI_CTL_SOCK"] = ctl_path
    env.pop("SOLARI_DB_USER", None)
    env.pop("SOLARI_DB_PASS", None)
    return subprocess.Popen(
        ["php", "-S", f"127.0.0.1:{port}", "-t", str(API_DIR), str(API_DIR / "index.php")],
        cwd=ROOT,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


class ProxyHandler(http.server.BaseHTTPRequestHandler):
    backend_port = 0

    def log_message(self, *_args) -> None:
        return

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        headers = {"Content-Type": self.headers.get("Content-Type", "application/json")}
        connection = http.client.HTTPConnection("127.0.0.1", self.backend_port, timeout=5)
        try:
            connection.request("POST", self.path, body=body, headers=headers)
            response = connection.getresponse()
            payload = response.read()
            self.send_response(response.status)
            self.send_header("Content-Type", response.getheader("Content-Type", "application/json"))
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)
        finally:
            connection.close()


def request(conn: http.client.HTTPSConnection, path: str, body: bytes, content_type: str = "application/json"):
    conn.request("POST", path, body=body, headers={"Content-Type": content_type})
    response = conn.getresponse()
    payload = response.read()
    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        data = None
    return response.status, data


def enrollment(host: str = "node.akoria.net") -> bytes:
    return json.dumps({"token": TOKEN, "role": "client", "host": host,
                       "certFp": FP, "csrPem": CSR}).encode()


def run() -> int:
    checks: list[tuple[str, bool]] = []
    with tempfile.TemporaryDirectory(prefix="solari-enroll-http-") as tmp:
        tmp_path = Path(tmp)
        ctl_path = tmp_path / "ctl.sock"
        ca = tmp_path / "ca.pem"
        cert = tmp_path / "server.pem"
        key = tmp_path / "server.key"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
            "-keyout", str(key), "-out", str(cert),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ca.write_bytes(cert.read_bytes())

        fixture = CtlFixture(ctl_path)
        backend_port, tls_port = reserve_port(), reserve_port()
        php = start_php(backend_port, str(ctl_path))
        proxy = None
        server = None
        thread = None
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if php.poll() is not None:
                    raise RuntimeError("PHP API fixture exited during startup")
                try:
                    with socket.create_connection(("127.0.0.1", backend_port), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                raise RuntimeError("PHP API fixture did not start")

            ProxyHandler.backend_port = backend_port
            server = http.server.ThreadingHTTPServer(("127.0.0.1", tls_port), ProxyHandler)
            tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls.load_cert_chain(str(cert), str(key))
            server.socket = tls.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            client_tls = ssl.create_default_context(cafile=str(ca))
            conn = http.client.HTTPSConnection("127.0.0.1", tls_port, context=client_tls, timeout=5)

            status, data = request(conn, "/api/enrollments", b'{"role":"client"}')
            checks.append(("invalid enrollment is rejected before ctl", status == 400 and len(fixture.lines) == 0))

            status, data = request(conn, "/api/enrollments", b" " * 16385)
            checks.append(("oversized enrollment is rejected before JSON/ctl", status == 413 and len(fixture.lines) == 0))

            status, data = request(conn, "/api/enrollments", enrollment())
            checks.append(("valid enrollment reaches ctl and returns pending", status == 200 and data and data.get("ok") is True and data["data"].get("status") == "pending"))
            with fixture.lock:
                submit = fixture.lines[-1] if fixture.lines else ""
            checks.append(("ctl frame carries exact token, identity, and CSR", all(x in submit for x in ("ENROLL_SUBMIT", "token=" + TOKEN, "role=client", "host=node.akoria.net", "certFp=" + FP, "csrPem="))))

            status, data = request(conn, "/api/enrollments", enrollment("approved.akoria.net"))
            checks.append(("approved response returns complete certificate material", status == 200 and data and data["data"].get("nodeId") == "42" and data["data"].get("cert") == "LEAF PEM\n" and data["data"].get("ca") == "CA PEM\n"))

            status, data = request(conn, "/api/enrollments", enrollment("incomplete.akoria.net"))
            checks.append(("incomplete approved response fails closed", status == 502 and data and data.get("error", {}).get("code") == "control_error"))

            status, data = request(conn, "/api/enrollments/ack", json.dumps({"token": TOKEN}).encode())
            checks.append(("valid receipt acknowledgement reaches ctl", status == 200 and data and data["data"].get("status") == "acknowledged"))

            status, data = request(conn, "/api/enrollments/ack", b'{"token":"short"}')
            checks.append(("invalid receipt acknowledgement is rejected", status == 400 and len(fixture.lines) == 4))
            conn.close()
        finally:
            if server is not None:
                server.shutdown()
                server.server_close()
            if thread is not None:
                thread.join(timeout=2)
            php.terminate()
            try:
                php.wait(timeout=3)
            except subprocess.TimeoutExpired:
                php.kill()
                php.wait(timeout=3)
            fixture.close()

    failures = 0
    for label, passed in checks:
        print(f"{'ok' if passed else 'FAIL'} - {label}")
        failures += not passed
    print(f"{len(checks) - failures} passed, {failures} failed")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(run())
