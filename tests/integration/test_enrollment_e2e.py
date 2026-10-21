#!/usr/bin/env python3
"""Full HTTPS -> PHP -> solariCtl -> MariaDB -> CA enrollment integration.

Requires a disposable MariaDB service and the IO-enabled server build. The test
creates and drops its own database, starts a local control server and PHP API,
and uses a temporary CA. It never connects to a production socket or SoR.
"""
from __future__ import annotations

import http.client
import http.server
import hashlib
import json
import os
import re
import socket
import ssl
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import quote


ROOT = Path(__file__).resolve().parents[2]
API_DIR = ROOT / "dashboard" / "api"
SERVER = Path(os.environ.get("SOLARI_TEST_SERVER_BIN", ROOT / "build-server/src/server/solariServer"))
CTL = Path(os.environ.get("SOLARI_TEST_CTL_BIN", ROOT / "build-server/src/server/solariCtlClient"))


def run_command(args: list[str], *, env: dict[str, str], input_text: str | None = None) -> str:
    result = subprocess.run(args, cwd=ROOT, env=env, input=input_text, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return result.stdout


def reserve_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def ctl_call(sock_path: Path, line: str) -> str:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(5)
        sock.connect(str(sock_path))
        sock.sendall(line.encode("ascii") + b"\n")
        response = bytearray()
        while b"\n" not in response and len(response) < 32768:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
    return response.split(b"\n", 1)[0].decode("ascii")


class ProxyHandler(http.server.BaseHTTPRequestHandler):
    backend_port = 0

    def log_message(self, *_args) -> None:
        pass

    def do_POST(self) -> None:
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        upstream = http.client.HTTPConnection("127.0.0.1", self.backend_port, timeout=8)
        try:
            upstream.request("POST", self.path, body=body,
                             headers={"Content-Type": self.headers.get("Content-Type", "application/json")})
            response = upstream.getresponse()
            payload = response.read()
            self.send_response(response.status)
            self.send_header("Content-Type", response.getheader("Content-Type", "application/json"))
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)
        finally:
            upstream.close()


def https_post(conn: http.client.HTTPSConnection, path: str, value: dict) -> tuple[int, dict]:
    conn.request("POST", path, body=json.dumps(value), headers={"Content-Type": "application/json"})
    response = conn.getresponse()
    payload = response.read()
    return response.status, json.loads(payload)


def main() -> int:
    required = ("SOLARI_DB_HOST", "SOLARI_DB_PORT", "SOLARI_DB_USER", "SOLARI_DB_NAME")
    missing = [key for key in required if not os.environ.get(key)]
    if missing:
        raise SystemExit("missing test DB settings: " + ", ".join(missing))
    if not SERVER.is_file() or not CTL.is_file():
        raise SystemExit(f"missing server/ctl binaries: {SERVER} / {CTL}")

    env = os.environ.copy()
    password = env.get("SOLARI_DB_PASS", "")
    if password:
        env["MYSQL_PWD"] = password
    db_name = f"solarinet_enroll_e2e_{os.getpid()}"
    mysql = ["mariadb", "--protocol=tcp", "-h", env["SOLARI_DB_HOST"],
             "-P", env["SOLARI_DB_PORT"], "-u", env["SOLARI_DB_USER"]]
    if env.get("SOLARI_DB_SOCKET"):
        mysql = ["mariadb", "--socket=" + env["SOLARI_DB_SOCKET"], "-u", env["SOLARI_DB_USER"]]
    subprocess.run(mysql + ["-e", f"DROP DATABASE IF EXISTS `{db_name}`; CREATE DATABASE `{db_name}` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    test_env = env.copy()
    test_env["SOLARI_DB_NAME"] = db_name

    temp_root: tempfile.TemporaryDirectory[str] | None = tempfile.TemporaryDirectory(prefix="solari-enroll-e2e-")
    tmp = Path(temp_root.name)
    server_proc: subprocess.Popen | None = None
    php_proc: subprocess.Popen | None = None
    tls_server: http.server.ThreadingHTTPServer | None = None
    tls_thread: threading.Thread | None = None
    try:
        run_command(["python3", "tools/migrate.py", "--database", "monitoring", "--db-name", db_name,
                     "--host", env["SOLARI_DB_HOST"], "--port", env["SOLARI_DB_PORT"], "--user", env["SOLARI_DB_USER"]], env=test_env)

        ca_cert, ca_key = tmp / "ca.pem", tmp / "ca-key.pem"
        web_cert, web_key = tmp / "web.pem", tmp / "web-key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=SolariNet E2E CA", "-keyout", str(ca_key), "-out", str(ca_cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
                        "-keyout", str(web_key), "-out", str(web_cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ca_key.chmod(0o600)

        ctl_sock = tmp / "solariCtl.sock"
        config = tmp / "server.conf"
        config.write_text(
            "[bind]\nuseTls=false\n"
            "ingestUrl=tcp://127.0.0.1:0\nsurveyUrl=tcp://127.0.0.1:0\n"
            "controlUrl=tcp://127.0.0.1:0\npubUrl=tcp://127.0.0.1:0\n"
            f"ctlSocket={ctl_sock}\n"
            "[db]\n"
            f"host={env['SOLARI_DB_HOST']}\nport={env['SOLARI_DB_PORT']}\n"
            f"name={db_name}\nuser={env['SOLARI_DB_USER']}\n"
            "[ca]\nmode=local\n"
            f"certFile={ca_cert}\nkeyFile={ca_key}\n"
        )
        server_proc = subprocess.Popen([str(SERVER), "--config", str(config)], cwd=ROOT,
                                       env=test_env, stdout=subprocess.DEVNULL)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if server_proc.poll() is not None:
                raise RuntimeError("solariServer exited before opening ctl socket")
            try:
                if ctl_call(ctl_sock, "PING").startswith("OK"):
                    break
            except (OSError, TimeoutError):
                time.sleep(0.1)
        else:
            raise RuntimeError("solariServer ctl socket did not become ready")

        token_reply = run_command([str(CTL), "--sock", str(ctl_sock), "enroll-token", "--op", "ci-e2e",
                                   "--role", "client", "--host", "e2e.akoria.net", "--ttl", "600"],
                                  env=test_env)
        token_match = re.search(r"\btoken=([0-9a-f]{64})\b", token_reply)
        enr_match = re.search(r"\benr=(\d+)\b", token_reply)
        if not token_match or not enr_match:
            raise RuntimeError("solariCtl did not issue a valid enrollment token")
        token, enr_id = token_match.group(1), int(enr_match.group(1))

        req_conf = tmp / "csr.cnf"
        req_conf.write_text("[req]\ndistinguished_name=dn\nreq_extensions=v3_req\nprompt=no\n"
                            "[dn]\nCN=client.e2e.akoria.net\n"
                            "[v3_req]\nsubjectAltName=DNS:e2e.akoria.net\n")
        csr_path, client_key = tmp / "client.csr", tmp / "client-key.pem"
        subprocess.run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", str(client_key),
                        "-out", str(csr_path), "-config", str(req_conf)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        payload = {"token": token, "role": "client", "host": "e2e.akoria.net",
                   "certFp": ":".join(["ab"] * 32), "csrPem": csr_path.read_text()}

        backend_port, tls_port = reserve_port(), reserve_port()
        php_env = env.copy()
        php_env["SOLARI_CTL_SOCK"] = str(ctl_sock)
        php_proc = subprocess.Popen(["php", "-S", f"127.0.0.1:{backend_port}", "-t", str(API_DIR),
                                     str(API_DIR / "index.php")], cwd=ROOT, env=php_env,
                                    stdout=subprocess.DEVNULL)
        for _ in range(100):
            if php_proc.poll() is not None:
                raise RuntimeError("PHP API exited during startup")
            try:
                with socket.create_connection(("127.0.0.1", backend_port), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("PHP API did not start")

        ProxyHandler.backend_port = backend_port
        tls_server = http.server.ThreadingHTTPServer(("127.0.0.1", tls_port), ProxyHandler)
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.load_cert_chain(str(web_cert), str(web_key))
        tls_server.socket = tls_context.wrap_socket(tls_server.socket, server_side=True)
        tls_thread = threading.Thread(target=tls_server.serve_forever, daemon=True)
        tls_thread.start()
        conn = http.client.HTTPSConnection("127.0.0.1", tls_port,
                                           context=ssl.create_default_context(cafile=str(web_cert)), timeout=8)

        status, reply = https_post(conn, "/api/enrollments", payload)
        assert status == 200 and reply.get("data", {}).get("status") == "pending", (status, reply)
        approval_reply = ctl_call(ctl_sock, f"APPROVE enr={enr_id} op=ci-e2e")
        assert approval_reply.startswith("OK"), f"approval failed: {approval_reply}"
        status, reply = https_post(conn, "/api/enrollments", payload)
        data = reply.get("data", {})
        assert status == 200 and data.get("status") == "approved", (status, reply)
        assert data.get("nodeId") and data.get("cert") and data.get("ca"), "approved pickup was incomplete"
        leaf, returned_ca = tmp / "leaf.pem", tmp / "returned-ca.pem"
        leaf.write_text(data["cert"])
        returned_ca.write_text(data["ca"])
        assert returned_ca.read_bytes() == ca_cert.read_bytes(), "pickup CA did not match configured CA"
        subprocess.run(["openssl", "verify", "-CAfile", str(ca_cert), str(leaf)], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subject = subprocess.run(["openssl", "x509", "-in", str(leaf), "-noout", "-subject"], check=True,
                                 capture_output=True, text=True).stdout
        assert "client.e2e.akoria.net" in subject, subject
        expected_fp = hashlib.sha256(ssl.PEM_cert_to_DER_cert(data["cert"])).hexdigest()
        expected_serial = subprocess.run(
            ["openssl", "x509", "-in", str(leaf), "-noout", "-serial"],
            check=True, capture_output=True, text=True,
        ).stdout.strip().split("=", 1)[1].upper()
        # Preserve the first byte: the persisted serial is byte-encoded, so 0F
        # must not be reduced to F by stripping all leading zero characters.
        assert re.fullmatch(r"[0-9A-F]{1,40}", expected_serial), expected_serial

        status, reply = https_post(conn, "/api/enrollments/ack", {"token": token})
        assert status == 200 and reply.get("data", {}).get("status") == "acknowledged", (status, reply)
        status, reply = https_post(conn, "/api/enrollments", payload)
        assert status == 400, (status, reply)
        conn.close()

        query = ("SELECT e.status, e.certAckAt IS NOT NULL, e.csrPem IS NULL, "
                 "e.enrollTokenHash IS NULL, n.leafCertFp, n.leafCertSerial "
                 f"FROM enrollment e JOIN node n ON n.nodeId=e.nodeId WHERE e.enrId={enr_id}")
        row = run_command(mysql + [db_name, "-N", "-B", "-e", query], env=env).strip().split("\t")
        assert row[:4] == ["approved", "1", "1", "1"], row
        assert row[4] == expected_fp and row[5].upper() == expected_serial, row
        print("ok - real HTTPS/PHP/solariCtl/MariaDB/CA enrollment, approval, pickup, and ack")
        print("ok - issued leaf SHA-256 fingerprint and serial persisted on node")
        print("ok - CSR identity, CA chain, and post-ack token/CSR cleanup")
        return 0
    finally:
        if tls_server is not None:
            if tls_thread is not None:
                tls_server.shutdown()
            tls_server.server_close()
        if tls_thread is not None:
            tls_thread.join(timeout=3)
        for proc in (php_proc, server_proc):
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
        temp_root.cleanup()
        subprocess.run(mysql + ["-e", f"DROP DATABASE IF EXISTS `{db_name}`"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
