#!/usr/bin/env python3
"""Verify the HTTP decommission route never persists its confirmation token.

Runs only against the named loopback scratch monitoring database. The test
starts the real C control socket with its privileged queue consumer and a local
PHP HTTP server; the decommission target is a synthetic row in that database.
"""
from __future__ import annotations

import atexit
import http.client
import json
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
NODE_ID = 879300001
OPERATOR = "decommission-http-test"


def db_args() -> list[str]:
    if os.environ.get("SOLARI_TEST_DB") != "1":
        raise RuntimeError("set SOLARI_TEST_DB=1 for a disposable DB test")
    host = os.environ.get("SOLARI_DB_HOST", "")
    name = os.environ.get("SOLARI_DB_NAME", "")
    if host not in {"127.0.0.1", "localhost"} or not re.fullmatch(
            r"solarinet_scan_worker_[a-z0-9_]+", name):
        raise RuntimeError("refusing a non-loopback or non-scratch monitoring database")
    return ["--host", host, "--port", os.environ.get("SOLARI_DB_PORT", "3306"),
            "--user", os.environ.get("SOLARI_DB_USER", "root"), "--database", name]


def sql(query: str) -> str:
    run = subprocess.run(["mariadb", "--batch", "--skip-column-names", *db_args(),
                          "--execute", query], check=True, capture_output=True,
                         text=True, timeout=10)
    return run.stdout.strip()


def port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(port_num: int, method: str, path: str, *, cookie: str = "",
            csrf: str = "", body: dict | None = None) -> tuple[int, dict | None, str]:
    headers = {"Accept": "application/json"}
    if cookie:
        headers["Cookie"] = cookie
    if csrf:
        headers["X-Solari-CSRF"] = csrf
    if method in {"POST", "PUT", "PATCH", "DELETE"}:
        headers.update({"Origin": f"http://127.0.0.1:{port_num}",
                        "Content-Type": "application/json"})
    payload = json.dumps(body).encode() if body is not None else None
    conn = http.client.HTTPConnection("127.0.0.1", port_num, timeout=8)
    try:
        conn.request(method, path, body=payload, headers=headers)
        res = conn.getresponse()
        raw = res.read().decode("utf-8", "replace")
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            data = None
        return res.status, data, res.getheader("Set-Cookie", "")
    finally:
        conn.close()


def main() -> int:
    if len(sys.argv) != 2:
        raise RuntimeError("usage: test_decommission_queue_http.py /path/to/test_ctl_adopt_bridge")
    bridge = pathlib.Path(sys.argv[1]).resolve(strict=True)
    if not bridge.is_file() or not os.access(bridge, os.X_OK):
        raise RuntimeError("real C control bridge fixture is not executable")

    if sql(f"SELECT COUNT(*) FROM node WHERE nodeId={NODE_ID}") != "0":
        raise RuntimeError("scratch decommission node already exists")
    sql(f"INSERT INTO node (nodeId,role,hostFqdn,certCn,enrolledAt,state) VALUES "
        f"({NODE_ID},'client','decommission-test.invalid','decommission-test.invalid',"
        "UTC_TIMESTAMP(),'up')")

    def clean_fixture() -> None:
        sql(f"DELETE FROM ctlRequest WHERE requestedBy='{OPERATOR}'")
        sql(f"DELETE FROM alertEvent WHERE nodeId={NODE_ID}")
        sql(f"DELETE FROM node WHERE nodeId={NODE_ID}")

    atexit.register(clean_fixture)

    with tempfile.TemporaryDirectory(prefix="solari-decommission-http-") as td:
        temp = pathlib.Path(td)
        sessions = temp / "sessions"
        sessions.mkdir(mode=0o700)
        ctl_sock = temp / "solariCtl.sock"
        router = temp / "router.php"
        router.write_text(f"""<?php
require_once {json.dumps(str(ROOT / 'dashboard/api/lib/Auth.php'))};
$path = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);
if ($path === '/__health') {{ echo 'ok'; exit; }}
if ($path === '/__fixture_session') {{
  Auth::establishSession(['username'=>'{OPERATOR}','role'=>'operator','source'=>'test']);
  header('Content-Type: application/json'); echo '{{\"ready\":true}}'; exit;
}}
if (is_string($path) && str_starts_with($path, '/api/')) {{ require {json.dumps(str(ROOT / 'dashboard/api/index.php'))}; exit; }}
return false;
?>""", encoding="utf-8")

        env = os.environ.copy()
        env["SOLARI_CTL_SOCK"] = str(ctl_sock)
        env["SOLARI_TEST_DRAIN_CTL_QUEUE"] = "1"
        bridge_log = (temp / "bridge.log").open("wb")
        http_log = (temp / "http.log").open("wb")
        ctl = subprocess.Popen([str(bridge), str(ctl_sock)], cwd=ROOT, env=env,
                               stdout=bridge_log, stderr=subprocess.STDOUT)
        web_port = port()
        php = None
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline and not ctl_sock.exists():
                if ctl.poll() is not None:
                    raise RuntimeError("C bridge exited before opening its socket")
                time.sleep(0.05)
            if not ctl_sock.exists():
                raise RuntimeError("C bridge socket did not appear")
            php = subprocess.Popen(["php", "-d", f"session.save_path={sessions}",
                                    "-S", f"127.0.0.1:{web_port}", str(router)],
                                   cwd=ROOT, env=env, stdout=http_log,
                                   stderr=subprocess.STDOUT)
            for _ in range(100):
                if php.poll() is not None:
                    raise RuntimeError("PHP test server exited before health check")
                try:
                    if request(web_port, "GET", "/__health")[0] == 200:
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                raise RuntimeError("PHP test server did not become ready")

            _status, _data, set_cookie = request(web_port, "GET", "/__fixture_session")
            cookie = set_cookie.split(";", 1)[0]
            status, whoami, _ = request(web_port, "GET", "/api/auth/whoami", cookie=cookie)
            csrf = whoami["data"]["csrfToken"] if status == 200 and whoami and whoami.get("ok") else ""
            if not csrf:
                raise RuntimeError("operator fixture did not receive a CSRF token")

            status, result, _ = request(web_port, "POST", "/api/control/decommission",
                cookie=cookie, csrf=csrf,
                body={"nodeId": NODE_ID, "confirm": True, "wipeScope": ["config", "certs"]})
            if status != 200 or not result or not result.get("ok") or \
                    result.get("data", {}).get("status") != "awaiting_node":
                raise RuntimeError("decommission HTTP request did not reach awaiting-node state")

            stored = sql("SELECT CONCAT(state,'|',argsWire,'|',COALESCE(result,''),'|',"
                "COALESCE(error,'')) FROM ctlRequest "
                f"WHERE requestedBy='{OPERATOR}' AND verb='DECOMMISSION' ORDER BY id DESC LIMIT 1")
            fields = stored.split("|", 3)
            if len(fields) != 4 or fields[0] != "done" or \
                    "inlineConfirm=1" not in fields[1] or "confirm=" in fields[1] or \
                    fields[2] != "OK accepted=1" or "confirm=" in fields[2] or "confirm=" in fields[3]:
                raise RuntimeError("queued decommission request/result was not token-free")
            challenge = sql(f"SELECT CONCAT(state,'|',CHAR_LENGTH(tokenHash)) "
                f"FROM decommissionChallenge WHERE nodeId={NODE_ID}")
            if challenge != "consumed|64":
                raise RuntimeError("synthetic decommission challenge did not complete")
            print("PASS: HTTP route checked bridge capability, queued one token-free intent, "
                  "and C consumed the challenge without storing the token")
        finally:
            if php is not None:
                php.terminate()
                try: php.wait(timeout=5)
                except subprocess.TimeoutExpired: php.kill(); php.wait()
            ctl.terminate()
            try: ctl.wait(timeout=5)
            except subprocess.TimeoutExpired: ctl.kill(); ctl.wait()
            bridge_log.close()
            http_log.close()
    # Remove only this uniquely marked fixture data, even if the harness DB is reused.
    clean_fixture()
    atexit.unregister(clean_fixture)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
