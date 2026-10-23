#!/usr/bin/env python3
"""Exercise scan, progress, discovery, and adoption through the real PHP API.

The discovered candidate is created by the native scan worker process test.
The AF_UNIX bridge is the real C serverCtl handler attached to a disposable
monitoring database. The HTTP API, sessions, CSRF, C asset adoption, monitoring
reads, and SoR mirror all use their real code paths; no monitor transport is
attached.
"""
from __future__ import annotations

import http.client
import ipaddress
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
API_DIR = ROOT / "dashboard" / "api"
AUTH_LIB = API_DIR / "lib" / "Auth.php"
API_ENTRY = API_DIR / "index.php"
EXPECTED_ADOPTED_NAME = "scan-e2e-node"


def scratch_config() -> tuple[str, str, str, str, str, str]:
    if os.environ.get("SOLARI_TEST_DB") != "1" or os.environ.get("SOLARI_TEST_SOR_DB") != "1":
        raise RuntimeError("both disposable monitoring and SoR test flags are required")
    mh = os.environ.get("SOLARI_DB_HOST", "")
    mn = os.environ.get("SOLARI_DB_NAME", "")
    sh = os.environ.get("SOR_DB_HOST", "")
    sn = os.environ.get("SOR_DB_NAME", "")
    if mh not in {"127.0.0.1", "localhost"} or re.fullmatch(r"solarinet_scan_worker_[a-z0-9_]+", mn) is None:
        raise RuntimeError("refusing a non-local or non-scratch monitoring database")
    if sh not in {"127.0.0.1", "localhost"} or re.fullmatch(r"solarinet_scan_worker_sor_[a-z0-9_]+", sn) is None:
        raise RuntimeError("refusing a non-local or non-scratch SoR database")
    return mh, os.environ.get("SOLARI_DB_PORT", "3306"), mn, sh, os.environ.get("SOR_DB_PORT", "3306"), sn


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def php_router() -> str:
    return f"""<?php
require_once {json.dumps(str(AUTH_LIB))};
$path = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);
if ($path === '/__health') {{ echo 'ok'; exit; }}
if ($path === '/__fixture_session') {{
    $role = (string)($_GET['role'] ?? 'operator');
    if (!in_array($role, ['viewer', 'operator'], true)) {{ http_response_code(400); exit; }}
    $user = $role === 'operator' ? 'scan-worker-process-test' : 'scan-http-viewer';
    Auth::establishSession(['username'=>$user,'role'=>$role,'source'=>'oidc']);
    header('Content-Type: application/json'); echo '{{"ready":true}}'; exit;
}}
if (is_string($path) && str_starts_with($path, '/api/')) {{ require {json.dumps(str(API_ENTRY))}; exit; }}
return false;
?>"""


def http_request(port: int, method: str, path: str, *, cookie: str = "",
                 csrf: str = "", body: dict | None = None) -> tuple[int, dict | None, str]:
    headers = {"Accept": "application/json"}
    if cookie:
        headers["Cookie"] = cookie
    if csrf:
        headers["X-Solari-CSRF"] = csrf
    if method in {"POST", "PUT", "PATCH", "DELETE"}:
        headers["Origin"] = f"http://127.0.0.1:{port}"
        headers["Content-Type"] = "application/json"
    payload = json.dumps(body).encode() if body is not None else None
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=6)
    try:
        conn.request(method, path, body=payload, headers=headers)
        response = conn.getresponse()
        raw = response.read().decode("utf-8", "replace")
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            data = None
        return response.status, data, response.getheader("Set-Cookie", "")
    finally:
        conn.close()


def mariadb(sql: str, db: tuple[str, str, str, str, str, str]) -> list[str]:
    _mh, _mp, _mn, sh, sp, sn = db
    cmd = ["mariadb", "--batch", "--skip-column-names", "--host", sh,
           "--port", sp, "--user", os.environ.get("SOR_DB_USER", "root"),
           "--database", sn, "--execute", sql]
    result = subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=8)
    return result.stdout.rstrip("\n").splitlines()


def monitoring_mariadb(sql: str, db: tuple[str, str, str, str, str, str]) -> list[str]:
    mh, mp, mn, _sh, _sp, _sn = db
    cmd = ["mariadb", "--batch", "--skip-column-names", "--host", mh,
           "--port", mp, "--user", os.environ.get("SOLARI_DB_USER", "root"),
           "--database", mn, "--execute", sql]
    result = subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=8)
    return result.stdout.rstrip("\n").splitlines()


def clean_monitoring_fixture(db: tuple[str, str, str, str, str, str]) -> None:
    monitoring_mariadb(
        "START TRANSACTION; "
        "DELETE FROM probeCurrent WHERE targetId IN "
        "(SELECT targetId FROM probeTarget WHERE assetId IN "
        "(SELECT assetId FROM asset WHERE ip='127.0.0.1')); "
        "DELETE FROM probeHistory WHERE targetId IN "
        "(SELECT targetId FROM probeTarget WHERE assetId IN "
        "(SELECT assetId FROM asset WHERE ip='127.0.0.1')); "
        "DELETE FROM probeTarget WHERE assetId IN "
        "(SELECT assetId FROM asset WHERE ip='127.0.0.1'); "
        "DELETE FROM asset WHERE ip='127.0.0.1'; "
        "DELETE FROM ctlRequest WHERE requestedBy='scan-worker-process-test' AND verb='DISCOVER'; "
        "COMMIT;", db)


def queued_discovery_count(db: tuple[str, str, str, str, str, str]) -> int:
    rows = monitoring_mariadb(
        "SELECT COUNT(*) FROM ctlRequest WHERE requestedBy='scan-worker-process-test' AND verb='DISCOVER'",
        db)
    return int(rows[0])


def main() -> int:
    if len(sys.argv) != 4 or not sys.argv[1].isdigit() or not sys.argv[2].isdigit():
        raise RuntimeError("usage: test_discovery_http.py <discId> <scanId> <test_ctl_adopt_bridge>")
    db = scratch_config()
    disc_id = int(sys.argv[1])
    scan_id = int(sys.argv[2])
    ctl_binary = pathlib.Path(sys.argv[3]).resolve(strict=True)
    if not ctl_binary.is_file() or not os.access(ctl_binary, os.X_OK):
        raise RuntimeError("real C control bridge fixture is not executable")
    target_ip = ipaddress.ip_address("127.0.0.1")
    mapped = f"::ffff:{target_ip}"
    ip_sql = f"'::{target_ip.compressed}'" if target_ip.version == 6 else f"'{mapped}'"
    # The test marker is unique and its address must be unused before the API writes.
    previous = mariadb(
        "SELECT COUNT(*) FROM entities e JOIN ip_addresses a ON a.entity_id=e.id "
        f"WHERE a.address=INET6_ATON({ip_sql}) AND a.deleted_at IS NULL AND e.deleted_at IS NULL",
        db)
    if previous != ["0"]:
        raise RuntimeError("scratch SoR already contains the scan candidate IP")

    stale_monitor_asset = monitoring_mariadb(
        "SELECT CONCAT(assetId,'|',displayName) FROM asset WHERE ip='127.0.0.1'", db)
    if stale_monitor_asset:
        raise RuntimeError("scratch monitoring DB already contains the scan candidate asset")

    temp_context = tempfile.TemporaryDirectory(prefix="solari-discovery-http-")
    temp = pathlib.Path(temp_context.name)
    sessions = temp / "sessions"
    sessions.mkdir(mode=0o700)
    router_file = temp / "router.php"
    ctl_path = temp / "ctl.sock"
    port = free_port()
    router_file.write_text(php_router(), encoding="utf-8")
    log_path = temp / "php.log"
    log = log_path.open("wb")
    ctl_log_path = temp / "ctl.log"
    ctl_log = ctl_log_path.open("wb")
    env = os.environ.copy()
    env["SOLARI_CTL_SOCK"] = str(ctl_path)
    env["SOLARI_DB_HOST"], env["SOLARI_DB_PORT"], env["SOLARI_DB_NAME"] = db[0], db[1], db[2]
    env["SOR_DB_HOST"], env["SOR_DB_PORT"], env["SOR_DB_NAME"] = db[3], db[4], db[5]
    ctl_process = subprocess.Popen([str(ctl_binary), str(ctl_path)],
                                   cwd=ROOT, env=env, stdout=ctl_log, stderr=subprocess.STDOUT)
    php = None
    adopted = False
    checks: list[tuple[str, bool]] = []
    try:
        socket_deadline = time.monotonic() + 8
        while time.monotonic() < socket_deadline:
            if ctl_process.poll() is not None:
                raise RuntimeError("C control bridge exited: " + ctl_log_path.read_text(errors="replace")[-3000:])
            if ctl_path.exists():
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("C control bridge socket did not appear")

        php = subprocess.Popen(["php", "-d", f"session.save_path={sessions}",
                            "-S", f"127.0.0.1:{port}", str(router_file)],
                           cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if php.poll() is not None:
                raise RuntimeError("PHP discovery fixture exited: " + log_path.read_text(errors="replace")[-3000:])
            try:
                status, _data, _cookie = http_request(port, "GET", "/__health")
                if status == 200:
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("PHP discovery fixture did not start")

        status, data, _ = http_request(port, "GET", "/api/discovery?status=new")
        checks.append(("anonymous discovery read requires session", status == 401))

        _status, _data, session_cookie = http_request(port, "GET", "/__fixture_session?role=viewer")
        viewer_cookie = session_cookie.split(";", 1)[0]
        status, whoami, _ = http_request(port, "GET", "/api/auth/whoami", cookie=viewer_cookie)
        viewer_token = whoami["data"]["csrfToken"] if whoami and whoami.get("ok") else ""
        before_submit = queued_discovery_count(db)
        status, _data, _ = http_request(port, "POST", "/api/discovery/scan",
            cookie=viewer_cookie, csrf=viewer_token, body={"cidr": "10.0.0.0/24", "ports": "443"})
        checks.append(("viewer cannot start a scan", status == 403 and queued_discovery_count(db) == before_submit))

        _status, _data, session_cookie = http_request(port, "GET", "/__fixture_session?role=operator")
        operator_cookie = session_cookie.split(";", 1)[0]
        status, whoami, _ = http_request(port, "GET", "/api/auth/whoami", cookie=operator_cookie)
        token = whoami["data"]["csrfToken"] if whoami and whoami.get("ok") else ""
        before_submit = queued_discovery_count(db)
        status, _data, _ = http_request(port, "POST", "/api/discovery/scan",
            cookie=operator_cookie, body={"cidr": "10.0.0.0/24", "ports": "443"})
        checks.append(("operator scan requires CSRF", status == 403 and queued_discovery_count(db) == before_submit))

        status, scan, _ = http_request(port, "POST", "/api/discovery/scan",
            cookie=operator_cookie, csrf=token,
            body={"cidr": "10.0.0.0/24", "ports": "443"})
        checks.append(("operator scan queues and returns durable scan identity",
            status == 202 and scan and scan.get("data", {}).get("requestId", 0) > 0
            and scan["data"].get("status") == "admission_pending"
            and queued_discovery_count(db) == before_submit + 1))

        request_id = int(scan["data"]["requestId"])
        status, admission, _ = http_request(port, "GET",
            f"/api/discovery/scan-requests/{request_id}", cookie=operator_cookie)
        checks.append(("queued scan admission exposes persisted pending state",
            status == 200 and admission and admission.get("data", {}).get("state") == "pending"))

        status, progress, _ = http_request(port, "GET", f"/api/discovery/scans/{scan_id}", cookie=operator_cookie)
        checks.append(("scan progress reports terminal worker counts",
            status == 200 and progress and progress.get("data", {}).get("status") == "completed"
            and progress["data"].get("hostsFound") == 1))

        status, candidates, _ = http_request(port, "GET", "/api/discovery?status=new", cookie=operator_cookie)
        candidate = next((row for row in candidates.get("data", [])
                          if int(row.get("discId", 0)) == disc_id), None) if candidates else None
        checks.append(("native scan result is visible in authenticated discovery list",
            status == 200 and candidate is not None and candidate.get("ip") == str(target_ip)
            and candidate.get("via") == "portscan"))

        status, _data, _ = http_request(port, "POST", f"/api/discovery/{disc_id}/adopt",
            cookie=operator_cookie, body={"displayName": EXPECTED_ADOPTED_NAME})
        checks.append(("adoption requires CSRF", status == 403))

        status, adoption, _ = http_request(port, "POST", f"/api/discovery/{disc_id}/adopt",
            cookie=operator_cookie, csrf=token,
            body={"displayName": EXPECTED_ADOPTED_NAME, "class": "host",
                  "heartbeat": False, "services": [443]})
        asset_id = int(adoption.get("data", {}).get("assetId", 0)) if adoption else 0
        checks.append(("adoption executes real C asset mutation",
            status == 200 and asset_id > 0 and adoption.get("data", {}).get("discId") == str(disc_id)))
        adopted = status == 200 and bool(adoption and adoption.get("ok"))

        assets = monitoring_mariadb(
            "SELECT CONCAT(assetId,'|',displayName,'|',class,'|',monitorHost) FROM asset "
            "WHERE ip='127.0.0.1'", db)
        checks.append(("C control mutation writes selected asset metadata",
            assets == [f"{asset_id}|{EXPECTED_ADOPTED_NAME}|host|0"]))
        targets = monitoring_mariadb(
            "SELECT CONCAT(targetId,'|',assetId) FROM probeTarget "
            "WHERE targetId='tcp:127.0.0.1:443'", db)
        checks.append(("C control mutation creates service target owned by asset",
            len(targets) == 1 and targets[0] == f"tcp:127.0.0.1:443|{asset_id}"))
        adopted_rows = monitoring_mariadb(
            f"SELECT status FROM discovered WHERE discId={disc_id}", db)
        checks.append(("C control mutation marks candidate adopted",
            adopted_rows == ["adopted"]))

        if adopted:
            rows = mariadb(
                "SELECT CONCAT(e.name,'|',s.slug,'|',COUNT(a.id),'|',SUM(a.is_primary)) "
                "FROM entities e JOIN ip_addresses a ON a.entity_id=e.id "
                "JOIN sources s ON s.id=e.source_id "
                f"WHERE a.address=INET6_ATON({ip_sql}) AND a.deleted_at IS NULL AND e.deleted_at IS NULL "
                "GROUP BY e.id,e.name,s.slug", db)
            checks.append(("SoR contains one dashboard-provenance primary address",
                rows == [f"{EXPECTED_ADOPTED_NAME}|dashboard|1|1"]))
    finally:
        if php is not None and php.poll() is None:
            php.terminate()
            try:
                php.wait(timeout=3)
            except subprocess.TimeoutExpired:
                php.kill()
                php.wait(timeout=3)
        log.close()
        if ctl_process.poll() is None:
            ctl_process.terminate()
            try:
                ctl_process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                ctl_process.kill()
                ctl_process.wait(timeout=3)
        ctl_log.close()
        # Recover only the exact marked fixture this test may have written.
        if adopted:
            mariadb(
                "START TRANSACTION; "
                "SET @e2e=(SELECT e.id FROM entities e JOIN ip_addresses a ON a.entity_id=e.id "
                "JOIN sources s ON s.id=e.source_id "
                f"WHERE a.address=INET6_ATON({ip_sql}) AND e.name='{EXPECTED_ADOPTED_NAME}' "
                "AND s.slug='dashboard' AND a.deleted_at IS NULL AND e.deleted_at IS NULL LIMIT 1); "
                "DELETE FROM ip_addresses WHERE entity_id=@e2e; "
                "DELETE FROM entities WHERE id=@e2e; COMMIT;", db)
        clean_monitoring_fixture(db)
        temp_context.cleanup()

    failures = 0
    for label, passed in checks:
        print(f"{'ok' if passed else 'FAIL'} - {label}")
        failures += not passed
    if failures:
        print("PHP fixture log:\n" + log_path.read_text(errors="replace")[-3000:])
    print(f"{len(checks) - failures} passed, {failures} failed")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
