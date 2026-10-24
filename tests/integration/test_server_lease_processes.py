#!/usr/bin/env python3
"""Exercise two real solariServer processes against a disposable MariaDB.

The servers share one lease row and the same loopback listener URLs. The first
must become active, the second must remain standby without changing the epoch,
and the second must claim the next epoch and bind all listeners after the first
shuts down. This test never targets the production database name.
"""
from __future__ import annotations

import os
import pathlib
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
import datetime


def require_test_database() -> tuple[str, str, str, str, str]:
    if os.environ.get("SOLARI_TEST_DB") != "1":
        raise RuntimeError("set SOLARI_TEST_DB=1 to enable the disposable DB test")
    name = os.environ.get("SOLARI_DB_NAME", "")
    if not re.fullmatch(r"solarinet_lease_process_[A-Za-z0-9_]+", name):
        raise RuntimeError("SOLARI_DB_NAME must use the dedicated solarinet_lease_process_* test prefix")
    return (os.environ.get("SOLARI_DB_HOST", "127.0.0.1"),
            os.environ.get("SOLARI_DB_PORT", "3306"), name,
            os.environ.get("SOLARI_DB_USER", "solari"),
            os.environ.get("SOLARI_DB_SOCKET", ""))


def mariadb_args(host: str, port: str, name: str, user: str,
                 db_socket: str) -> list[str]:
    args = ["mariadb", "--batch", "--skip-column-names", "--user", user,
            "--database", name]
    if db_socket:
        args.extend(["--protocol=SOCKET", "--socket", db_socket])
    else:
        args.extend(["--host", host, "--port", port])
    return args


def lease_state(db: tuple[str, str, str, str, str]) -> tuple[int, int]:
    result = subprocess.run(mariadb_args(*db) + ["--execute",
        "SELECT IF(expiresAt>UTC_TIMESTAMP(3),COALESCE(leaseHolder,0),0),leaseEpoch "
        "FROM serverLease WHERE id=1"],
        check=True, capture_output=True, text=True, timeout=5)
    fields = result.stdout.strip().split("\t")
    if len(fields) != 2:
        return (0, 0)
    return (int(fields[0]), int(fields[1]))


def lease_expiry(db: tuple[str, str, str, str, str]) -> tuple[int, int, datetime.datetime]:
    result = subprocess.run(mariadb_args(*db) + ["--execute",
        "SELECT COALESCE(leaseHolder,0),leaseEpoch,"
        "DATE_FORMAT(expiresAt,'%Y-%m-%d %H:%i:%s.%f') FROM serverLease WHERE id=1"],
        check=True, capture_output=True, text=True, timeout=5)
    fields = result.stdout.strip().split("\t")
    if len(fields) != 3 or not fields[2]:
        raise RuntimeError("active lease has no expiry timestamp")
    return (int(fields[0]), int(fields[1]), datetime.datetime.fromisoformat(fields[2]))


def execute_sql(db: tuple[str, str, str, str, str], sql: str) -> None:
    subprocess.run(mariadb_args(*db) + ["--execute", sql], check=True,
                   capture_output=True, text=True, timeout=5)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def listeners_ready(ports: tuple[int, int, int]) -> bool:
    for port in ports:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                pass
        except OSError:
            return False
    return True


def wait_for(description: str, predicate, processes, timeout: float = 12.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for label, process, log_path in processes:
            if process.poll() is not None:
                raise RuntimeError(f"{label} exited early ({process.returncode}):\n"
                                   f"{log_path.read_text(errors='replace')[-5000:]}")
        if predicate():
            return
        time.sleep(0.1)
    details = []
    for label, _process, log_path in processes:
        if log_path.exists():
            details.append(f"--- {label} log ---\n"
                           f"{log_path.read_text(errors='replace')[-5000:]}")
    suffix = "\n" + "\n".join(details) if details else ""
    raise RuntimeError(f"timed out waiting for {description}{suffix}")


def run_bind_failure_case(binary: pathlib.Path,
                          db: tuple[str, str, str, str, str], root: pathlib.Path) -> None:
    """A listener-bind failure must release the lease for a healthy standby."""
    failed_ports = tuple(free_port() for _ in range(3))
    standby_ports = tuple(free_port() for _ in range(3))
    while len(set(failed_ports + standby_ports)) != 6:
        failed_ports = tuple(free_port() for _ in range(3))
        standby_ports = tuple(free_port() for _ in range(3))
    failed_config, standby_config = root / "bind-failed.conf", root / "bind-standby.conf"
    failed_log, standby_log = root / "bind-failed.log", root / "bind-standby.log"
    write_config(failed_config, "bind-failed", 710011, db, failed_ports)
    write_config(standby_config, "bind-standby", 710012, db, standby_ports)
    blocked = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    blocked.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    blocked.bind(("127.0.0.1", failed_ports[2]))
    blocked.listen(1)
    active: list[tuple[str, subprocess.Popen, pathlib.Path]] = []
    handles = []
    try:
        execute_sql(db, "DELETE FROM serverLease WHERE id=1")
        for label, config, log_path in (
            ("bind-failed", failed_config, failed_log),
            ("bind-standby", standby_config, standby_log),
        ):
            handle = log_path.open("wb")
            handles.append(handle)
            process = subprocess.Popen([str(binary), "--config", str(config)],
                                       stdout=handle, stderr=subprocess.STDOUT)
            active.append((label, process, log_path))
            if label == "bind-failed":
                wait_for("failed primary to report listener bind failure",
                         lambda: b"bind pub (" in log_path.read_bytes()
                         and b"failed:" in log_path.read_bytes(), active)
                wait_for("failed primary to relinquish epoch 1",
                         lambda: lease_state(db) == (0, 1), active)
            else:
                wait_for("healthy standby to take epoch 2 and bind listeners",
                         lambda: lease_state(db) == (710012, 2)
                         and listeners_ready(standby_ports), active)
                time.sleep(6.5)  # cross the failed server's local TTL cooldown
                if lease_state(db) != (710012, 2):
                    raise RuntimeError("bind-failed server reclaimed the healthy primary lease")
                if not listeners_ready(standby_ports):
                    raise RuntimeError("healthy standby stopped serving after takeover")
    finally:
        for _label, process, _path in active:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        blocked.close()
        for handle in handles:
            handle.close()
        execute_sql(db, "DELETE FROM serverLease WHERE id=1")


def write_config(path: pathlib.Path, label: str, node_id: int,
                 db: tuple[str, str, str, str, str], ports: tuple[int, int, int]) -> None:
    host, db_port, db_name, db_user, db_socket = db
    if db_socket:
        host, db_port = "", "0"
    ingest, control, publish = ports
    path.write_text(f"""[identity]
hostFqdn = {label}.lease-test.invalid
nodeId = {node_id}

[bind]
ingestUrl = tcp://127.0.0.1:{ingest}
controlUrl = tcp://127.0.0.1:{control}
pubUrl = tcp://127.0.0.1:{publish}
ctlSocket = {path.parent / (label + '.sock')}
useTls = false

[db]
host = {host}
port = {db_port}
name = {db_name}
user = {db_user}
socket = {db_socket}
poolSize = 1

[lease]
leaseRenewSec = 1
leaseTtlSec = 6
""", encoding="utf-8")


def write_worker_config(path: pathlib.Path, node_id: int,
                        db: tuple[str, str, str, str, str]) -> None:
    host, db_port, db_name, db_user, db_socket = db
    if db_socket:
        host, db_port = "", "0"
    path.write_text(f"""[identity]
hostFqdn = scan-worker-{node_id}.lease-test.invalid
nodeId = {node_id}

[db]
host = {host}
port = {db_port}
name = {db_name}
user = {db_user}
socket = {db_socket}
poolSize = 1

[lease]
leaseRenewSec = 1
leaseTtlSec = 6
""", encoding="utf-8")


def make_slow_nmap(path: pathlib.Path, marker: pathlib.Path) -> None:
    path.write_text("""#!/bin/sh
if [ "$SOLARI_TEST_NMAP_MODE" = slow ]; then
    /bin/sleep 30 &
    child=$!
    printf '%s %s\\n' "$$" "$child" > "$SOLARI_TEST_NMAP_MARKER"
    wait "$child"
fi
printf '%s\\n' '<nmaprun><host><status state="up"/></host></nmaprun>'
""", encoding="utf-8")
    path.chmod(0o755)


def main() -> int:
    if len(sys.argv) != 3:
        raise RuntimeError("usage: test_server_lease_processes.py /path/to/solariServer /path/to/solariScanWorker")
    binary = pathlib.Path(sys.argv[1]).resolve(strict=True)
    worker_binary = pathlib.Path(sys.argv[2]).resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError("server binary is not an executable file")
    if not worker_binary.is_file() or not os.access(worker_binary, os.X_OK):
        raise RuntimeError("scan worker binary is not an executable file")
    db = require_test_database()
    ports = (free_port(), free_port(), free_port())
    while len(set(ports)) != len(ports):
        ports = (free_port(), free_port(), free_port())

    with tempfile.TemporaryDirectory(prefix="solarinet-lease-process-") as folder:
        root = pathlib.Path(folder)
        config_a, config_b = root / "server-a.conf", root / "server-b.conf"
        log_a, log_b = root / "server-a.log", root / "server-b.log"
        worker_config_a, worker_config_b = root / "worker-a.conf", root / "worker-b.conf"
        write_worker_config(worker_config_a, 710001, db)
        write_worker_config(worker_config_b, 710002, db)
        write_config(config_a, "server-a", 710001, db, ports)
        write_config(config_b, "server-b", 710002, db, ports)
        processes: list[tuple[str, subprocess.Popen, pathlib.Path]] = []
        log_handles = []
        try:
            run_bind_failure_case(binary, db, root)
            execute_sql(db, "DELETE FROM serverLease WHERE id=1")
            execute_sql(db, "DELETE FROM discovered WHERE ip='127.0.0.1' AND kind='host'")
            execute_sql(db, "DELETE FROM discoveryScanJob")
            for label, config, log_path in (("server-a", config_a, log_a),
                                            ("server-b", config_b, log_b)):
                log_file = log_path.open("wb")
                log_handles.append(log_file)
                if label == "server-a":
                    process = subprocess.Popen([str(binary), "--config", str(config)],
                                               stdout=log_file, stderr=subprocess.STDOUT)
                    processes.append((label, process, log_path))
                    wait_for("server-a to acquire epoch 1",
                             lambda: lease_state(db) == (710001, 1), processes)
                    wait_for("server-a to bind all listeners",
                             lambda: listeners_ready(ports), processes)
                else:
                    process = subprocess.Popen([str(binary), "--config", str(config)],
                                               stdout=log_file, stderr=subprocess.STDOUT)
                    processes.append((label, process, log_path))
                    wait_for("server-b to enter its run loop",
                             lambda: b"entering main loop" in log_path.read_bytes(), processes)
                    time.sleep(1.5)
                    if lease_state(db) != (710001, 1):
                        raise RuntimeError("standby claim changed the active holder or epoch")
                    if not listeners_ready(ports):
                        raise RuntimeError("active server stopped listening while standby ran")

            # A slow scan must not block the primary server's renewal loop.
            # The worker discovers the loopback listener, then blocks in a
            # controlled enrichment process while the real server keeps its
            # six-second lease alive through multiple one-second renewals.
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as scan_listener:
                scan_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                scan_listener.bind(("127.0.0.1", 0))
                scan_listener.listen(4)
                scan_port = scan_listener.getsockname()[1]
                fixture_dir = root / "tools"
                fixture_dir.mkdir()
                marker = root / "slow-nmap.marker"
                make_slow_nmap(fixture_dir / "nmap", marker)
                env = dict(os.environ)
                env["PATH"] = str(fixture_dir) + os.pathsep + env.get("PATH", "/usr/bin:/bin")
                env["SOLARI_TEST_NMAP_MARKER"] = str(marker)
                env["SOLARI_TEST_NMAP_MODE"] = "slow"
                execute_sql(db, "DELETE FROM discoveryScanJob")
                execute_sql(db,
                    "INSERT INTO discoveryScanJob "
                    "(requestedBy,ownerNodeId,leaseEpoch,cidr,ports,status,hostsTotal,createdAt,updatedAt) "
                    f"VALUES ('lease-renew-scan-test',710001,1,'127.0.0.1/32','{scan_port}',"
                    "'queued',1,UTC_TIMESTAMP(3),UTC_TIMESTAMP(3))")
                scan_id_result = subprocess.run(mariadb_args(*db) + ["--execute",
                    "SELECT scanId FROM discoveryScanJob WHERE requestedBy='lease-renew-scan-test'"],
                    check=True, capture_output=True, text=True, timeout=5)
                scan_id = int(scan_id_result.stdout.strip())
                expiry_before = lease_expiry(db)
                worker_log = root / "scan-worker.log"
                with worker_log.open("wb") as worker_log_file:
                    worker = subprocess.Popen([str(worker_binary), "--config",
                        str(worker_config_a), "--lock-file", str(root / "scan-worker-a.lock")],
                        stdout=worker_log_file, stderr=subprocess.STDOUT, env=env)
                    processes.append(("scan-worker", worker, worker_log))
                    wait_for("slow scan enrichment to start",
                        lambda: marker.exists(), processes)
                    time.sleep(2.2)
                    expiry_after = lease_expiry(db)
                    if expiry_after[:2] != (710001, 1):
                        raise RuntimeError("active server lost lease during the slow scan")
                    if expiry_after[2] <= expiry_before[2] + datetime.timedelta(seconds=1):
                        raise RuntimeError("active server did not renew its lease during the slow scan")
                    process_a = processes[0][1]
                    process_a.send_signal(signal.SIGTERM)
                    process_a.wait(timeout=5)
                    if process_a.returncode != 0:
                        raise RuntimeError(f"server-a did not shut down cleanly: {process_a.returncode}")
                    processes.remove(("server-a", process_a, log_a))
                    wait_for("server-b takeover during the active scan",
                        lambda: lease_state(db) == (710002, 2) and listeners_ready(ports),
                        processes, timeout=12.0)
                    wait_for("lease-losing scan worker to stop enrichment",
                        lambda: b"stopped after lease loss" in worker_log.read_bytes(), processes)
                    scan_state = subprocess.run(mariadb_args(*db) + ["--execute",
                        f"SELECT status FROM discoveryScanJob WHERE scanId={scan_id}"],
                        check=True, capture_output=True, text=True, timeout=5).stdout.strip()
                    if scan_state != "running":
                        raise RuntimeError("lease-losing worker changed the orphan scan state")
                    stale_results = subprocess.run(mariadb_args(*db) + ["--execute",
                        "SELECT COUNT(*) FROM discovered WHERE ip='127.0.0.1' AND kind='host'"],
                        check=True, capture_output=True, text=True, timeout=5).stdout.strip()
                    if stale_results != "0":
                        raise RuntimeError("lease-losing worker published a stale discovery result")

                    execute_sql(db,
                        "INSERT INTO discoveryScanJob "
                        "(requestedBy,ownerNodeId,leaseEpoch,cidr,ports,status,hostsTotal,createdAt,updatedAt) "
                        f"VALUES ('lease-successor-scan-test',710002,2,'127.0.0.1/32','{scan_port}',"
                        "'queued',1,UTC_TIMESTAMP(3),UTC_TIMESTAMP(3))")
                    successor_id = int(subprocess.run(mariadb_args(*db) + ["--execute",
                        "SELECT scanId FROM discoveryScanJob WHERE requestedBy='lease-successor-scan-test'"],
                        check=True, capture_output=True, text=True, timeout=5).stdout.strip())
                    env["SOLARI_TEST_NMAP_MODE"] = "fast"
                    successor_log = root / "scan-worker-successor.log"
                    with successor_log.open("wb") as successor_log_file:
                        successor = subprocess.Popen([str(worker_binary), "--config",
                            str(worker_config_b), "--lock-file", str(root / "scan-worker-b.lock")],
                            stdout=successor_log_file, stderr=subprocess.STDOUT, env=env)
                        processes.append(("scan-worker-successor", successor, successor_log))
                        wait_for("successor worker to recover orphan and finish its scan",
                            lambda: subprocess.run(mariadb_args(*db) + ["--execute",
                                "SELECT status FROM discoveryScanJob "
                                f"WHERE scanId={scan_id}"], check=True, capture_output=True,
                                text=True, timeout=5).stdout.strip() == "failed"
                            and subprocess.run(mariadb_args(*db) + ["--execute",
                                "SELECT status FROM discoveryScanJob "
                                f"WHERE scanId={successor_id}"], check=True, capture_output=True,
                                text=True, timeout=5).stdout.strip() == "completed",
                            processes, timeout=12.0)
                        final_results = subprocess.run(mariadb_args(*db) + ["--execute",
                            "SELECT COUNT(*) FROM discovered WHERE ip='127.0.0.1' "
                            "AND kind='host' AND via='portscan'"], check=True,
                            capture_output=True, text=True, timeout=5).stdout.strip()
                        if final_results != "1":
                            raise RuntimeError("successor scan did not publish exactly one candidate")
                        successor.send_signal(signal.SIGTERM)
                        successor.wait(timeout=5)
                        if successor.returncode != 0:
                            raise RuntimeError("successor scan worker did not stop cleanly")
                    worker.send_signal(signal.SIGTERM)
                    worker.wait(timeout=5)
                    if worker.returncode != 0:
                        raise RuntimeError("standby scan worker did not stop cleanly")
                    processes.remove(("scan-worker", worker, worker_log))
                    processes.remove(("scan-worker-successor", successor, successor_log))
                    execute_sql(db, f"DELETE FROM discoveryScanJob WHERE scanId IN ({scan_id},{successor_id})")

            if lease_state(db) != (710002, 2):
                raise RuntimeError("server-b lost its successor lease after scan recovery")
            if not listeners_ready(ports):
                raise RuntimeError("server-b stopped listening after scan recovery")
            print("PASS: bind failure relinquished the lease for a healthy standby; "
                  "server-a renewed epoch 1 during slow scan enrichment; server-b "
                  "took epoch 2, worker-a stopped on lease loss, worker-b recovered the "
                  "orphan and completed a scan")
        finally:
            for _label, process, _log_path in processes:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
            try:
                execute_sql(db, "DELETE FROM discovered WHERE ip='127.0.0.1' AND kind='host'")
                execute_sql(db, "DELETE FROM discoveryScanJob")
                execute_sql(db, "DELETE FROM serverLease WHERE id=1")
            except (subprocess.SubprocessError, OSError):
                pass
            for log_file in log_handles:
                log_file.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
