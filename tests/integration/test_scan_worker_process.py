#!/usr/bin/env python3
"""Exercise scan worker signal, lease-loss, standby, and recovery paths.

All jobs and lease rows live in an explicitly named disposable MariaDB schema.
The worker's nmap subprocess is replaced by a controlled local fixture so the
test needs no network targets or third-party scanner.
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

OWNER_A = 810001
OWNER_B = 810002
TARGET_IP = "127.0.0.1"


def database_config() -> tuple[str, str, str, str]:
    if os.environ.get("SOLARI_TEST_DB") != "1":
        raise RuntimeError("set SOLARI_TEST_DB=1 to enable the disposable DB test")
    name = os.environ.get("SOLARI_DB_NAME", "")
    if not re.fullmatch(r"solarinet_scan_worker_[A-Za-z0-9_]+", name):
        raise RuntimeError("SOLARI_DB_NAME must use the dedicated solarinet_scan_worker_* test prefix")
    return (os.environ.get("SOLARI_DB_HOST", "127.0.0.1"),
            os.environ.get("SOLARI_DB_PORT", "3306"), name,
            os.environ.get("SOLARI_DB_USER", "solari"))


def mariadb_args(db: tuple[str, str, str, str]) -> list[str]:
    host, port, name, user = db
    return ["mariadb", "--batch", "--skip-column-names", "--host", host,
            "--port", port, "--user", user, "--database", name]


def query(db: tuple[str, str, str, str], sql: str) -> list[str]:
    result = subprocess.run(mariadb_args(db) + ["--execute", sql], check=True,
                            capture_output=True, text=True, timeout=5)
    return result.stdout.rstrip("\n").splitlines()


def execute(db: tuple[str, str, str, str], sql: str) -> None:
    subprocess.run(mariadb_args(db) + ["--execute", sql], check=True,
                   capture_output=True, text=True, timeout=5)


def job_state(db: tuple[str, str, str, str], scan_id: int) -> tuple[str, str]:
    rows = query(db, f"SELECT status,COALESCE(errorText,'') FROM discoveryScanJob WHERE scanId={scan_id}")
    if len(rows) != 1:
        return ("missing", "")
    status, _, error = rows[0].partition("\t")
    return (status, error)


def insert_job(db: tuple[str, str, str, str], owner: int, epoch: int, port: int) -> int:
    rows = query(db,
        "INSERT INTO discoveryScanJob "
        "(requestedBy,ownerNodeId,leaseEpoch,cidr,ports,status,hostsTotal,createdAt,updatedAt) "
        f"VALUES ('scan-worker-process-test',{owner},{epoch},'{TARGET_IP}/32','{port}',"
        "'queued',1,UTC_TIMESTAMP(3),UTC_TIMESTAMP(3)); SELECT LAST_INSERT_ID()")
    if not rows or not rows[-1].isdigit():
        raise RuntimeError("could not create disposable scan job")
    return int(rows[-1])


def set_lease(db: tuple[str, str, str, str], owner: int, epoch: int) -> None:
    execute(db,
        "INSERT INTO serverLease(id,leaseHolder,leaseEpoch,expiresAt) "
        f"VALUES (1,{owner},{epoch},UTC_TIMESTAMP(3)+INTERVAL 30 SECOND) "
        "ON DUPLICATE KEY UPDATE leaseHolder=VALUES(leaseHolder), "
        "leaseEpoch=VALUES(leaseEpoch),expiresAt=VALUES(expiresAt)")


def write_config(path: pathlib.Path, node_id: int,
                 db: tuple[str, str, str, str]) -> None:
    host, port, name, user = db
    path.write_text(f"""[identity]
hostFqdn = scan-worker-{node_id}.lease-test.invalid
nodeId = {node_id}

[db]
host = {host}
port = {port}
name = {name}
user = {user}
poolSize = 1

[lease]
leaseRenewSec = 1
leaseTtlSec = 6
""", encoding="utf-8")


def make_nmap_fixture(path: pathlib.Path) -> None:
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


def process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    stat_path = pathlib.Path(f"/proc/{pid}/stat")
    if stat_path.exists():
        fields = stat_path.read_text().split()
        if len(fields) > 2 and fields[2] == "Z":
            return False
    return True


def wait_tool_stopped(pids: tuple[int, ...], timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not any(process_alive(pid) for pid in pids):
            return
        time.sleep(0.05)
    raise RuntimeError("enrichment process group remained alive after worker cancellation")


def wait_for(description: str, predicate, workers, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for label, process, log_path in workers:
            if process.poll() is not None and process.returncode != 0:
                raise RuntimeError(f"{label} exited early ({process.returncode}):\n"
                                   f"{log_path.read_text(errors='replace')[-5000:]}")
        if predicate():
            return
        time.sleep(0.05)
    details = []
    for label, process, log_path in workers:
        tail = log_path.read_text(errors='replace')[-3000:]
        details.append(f"{label} (exit={process.poll()}):\n{tail}")
    raise RuntimeError(f"timed out waiting for {description}; worker logs:\n" + "\n".join(details))


def start_worker(binary: pathlib.Path, config: pathlib.Path, lock_file: pathlib.Path,
                 log_path: pathlib.Path, fixture_dir: pathlib.Path,
                 marker: pathlib.Path, mode: str):
    env = dict(os.environ)
    env["PATH"] = str(fixture_dir) + os.pathsep + env.get("PATH", "/usr/bin:/bin")
    env["SOLARI_TEST_NMAP_MODE"] = mode
    env["SOLARI_TEST_NMAP_MARKER"] = str(marker)
    log_file = log_path.open("wb")
    process = subprocess.Popen([str(binary), "--config", str(config),
                                "--lock-file", str(lock_file)],
                               stdout=log_file, stderr=subprocess.STDOUT, env=env)
    return process, log_file


def stop_worker(process, timeout: float = 5.0) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=timeout)


def main() -> int:
    if len(sys.argv) != 3:
        raise RuntimeError("usage: test_scan_worker_process.py /path/to/solariScanWorker /path/to/test_ctl_adopt_bridge")
    binary = pathlib.Path(sys.argv[1]).resolve(strict=True)
    ctl_binary = pathlib.Path(sys.argv[2]).resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError("scan worker binary is not an executable file")
    if not ctl_binary.is_file() or not os.access(ctl_binary, os.X_OK):
        raise RuntimeError("C control bridge fixture is not an executable file")
    db = database_config()
    execute(db, "DELETE FROM discoveryScanJob")
    execute(db, f"DELETE FROM discovered WHERE ip='{TARGET_IP}' AND kind='host'")
    execute(db, "DELETE FROM serverLease WHERE id=1")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((TARGET_IP, 0))
        listener.listen(4)
        port = listener.getsockname()[1]

        with tempfile.TemporaryDirectory(prefix="solarinet-scan-worker-") as folder:
            root = pathlib.Path(folder)
            fixture_dir = root / "tools"
            fixture_dir.mkdir()
            make_nmap_fixture(fixture_dir / "nmap")
            config_a, config_b = root / "worker-a.conf", root / "worker-b.conf"
            write_config(config_a, OWNER_A, db)
            write_config(config_b, OWNER_B, db)
            lock_file = root / "scan-worker.lock"
            workers = []
            log_files = []
            scan_ids: list[int] = []
            try:
                set_lease(db, OWNER_A, 1)

                # SIGTERM while fake nmap is running must cancel the job and
                # terminate the entire enrichment process group.
                cancelled_id = insert_job(db, OWNER_A, 1, port)
                scan_ids.append(cancelled_id)
                marker = root / "nmap-cancel.marker"
                process, log_file = start_worker(binary, config_a, lock_file,
                    root / "worker-cancel.log", fixture_dir, marker, "slow")
                workers.append(("worker-cancel", process,
                                root / "worker-cancel.log"))
                log_files.append(log_file)
                wait_for("slow enrichment to start", lambda: marker.exists() and
                         job_state(db, cancelled_id)[0] == "running", workers)
                tool_pids = tuple(int(pid) for pid in marker.read_text().split())
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=7)
                if process.returncode != 0:
                    raise RuntimeError("worker did not stop cleanly after SIGTERM")
                if job_state(db, cancelled_id)[0] != "cancelled":
                    raise RuntimeError("SIGTERM did not persist a cancelled scan state")
                wait_tool_stopped(tool_pids)

                # Lease loss during enrichment must stop the tool and leave the
                # running job for the successor to settle, with no result write.
                lease_id = insert_job(db, OWNER_A, 1, port)
                scan_ids.append(lease_id)
                marker = root / "nmap-lease-loss.marker"
                process, log_file = start_worker(binary, config_a, lock_file,
                    root / "worker-lease-loss.log", fixture_dir, marker, "slow")
                workers.append(("worker-lease-loss", process,
                                root / "worker-lease-loss.log"))
                log_files.append(log_file)
                wait_for("lease-loss enrichment to start", lambda: marker.exists() and
                         job_state(db, lease_id)[0] == "running", workers)
                tool_pids = tuple(int(pid) for pid in marker.read_text().split())
                set_lease(db, OWNER_B, 2)
                wait_for("worker to observe lease loss", lambda:
                         b"stopped after lease loss" in (root / "worker-lease-loss.log").read_bytes(),
                         workers)
                wait_tool_stopped(tool_pids)
                if job_state(db, lease_id)[0] != "running":
                    raise RuntimeError("lease loser wrote a terminal scan state")
                if int(query(db, f"SELECT COUNT(*) FROM discovered WHERE ip='{TARGET_IP}' AND kind='host'")[0]) != 0:
                    raise RuntimeError("lease loser wrote a discovered host")
                stop_worker(process)

                # A standby worker cannot claim the successor's job. The active
                # successor then settles the orphan and completes a fresh scan.
                pending_id = insert_job(db, OWNER_B, 2, port)
                scan_ids.append(pending_id)
                marker = root / "nmap-standby.marker"
                process, log_file = start_worker(binary, config_a, lock_file,
                    root / "worker-standby.log", fixture_dir, marker, "slow")
                workers.append(("worker-standby", process, root / "worker-standby.log"))
                log_files.append(log_file)
                time.sleep(1.2)
                if job_state(db, pending_id)[0] != "queued" or marker.exists():
                    raise RuntimeError("standby worker claimed or started enrichment for a job")
                stop_worker(process)

                marker = root / "nmap-success.marker"
                process, log_file = start_worker(binary, config_b, lock_file,
                    root / "worker-success.log", fixture_dir, marker, "fast")
                workers.append(("worker-success", process, root / "worker-success.log"))
                log_files.append(log_file)
                wait_for("successor to settle old job and complete fresh job",
                         lambda: job_state(db, lease_id)[0] == "failed" and
                         job_state(db, pending_id)[0] == "completed", workers, timeout=12.0)
                if "lease owner changed" not in job_state(db, lease_id)[1]:
                    raise RuntimeError("successor did not record the lease-lost recovery reason")
                if int(query(db, f"SELECT COUNT(*) FROM discovered WHERE ip='{TARGET_IP}' AND kind='host'")[0]) != 1:
                    raise RuntimeError("active successor did not persist exactly one scan result")

                # Continue the real scan result through the PHP read/adopt
                # routes and the transactional SoR mirror when the test job
                # provides its second, separately migrated scratch schema.
                if os.environ.get("SOLARI_TEST_SOR_DB") == "1":
                    rows = query(db,
                        f"SELECT discId FROM discovered WHERE ip='{TARGET_IP}' "
                        "AND kind='host' AND via='portscan' AND status='new'")
                    if len(rows) != 1 or not rows[0].isdigit():
                        raise RuntimeError("successful TCP scan did not produce one adoptable portscan candidate")
                    if not pending_id:
                        raise RuntimeError("successful scan job id is missing")
                    api_test = pathlib.Path(__file__).parents[1] / "dashboard" / "test_discovery_http.py"
                    api = subprocess.run([sys.executable, str(api_test), rows[0],
                                          str(pending_id), str(ctl_binary)], check=False,
                                         capture_output=True, text=True, timeout=30)
                    if api.returncode != 0:
                        raise RuntimeError("discovery API/SoR acceptance failed: "
                            + (api.stderr or api.stdout)[-4000:])
                    print(api.stdout.strip())

                print("PASS: SIGTERM cancelled a scan; lease loss stopped enrichment and "
                      "fenced result writes; standby did not claim; successor recovered "
                      "the orphan and completed a scan")
            finally:
                for _label, process, _log_path in workers:
                    stop_worker(process)
                for log_file in log_files:
                    log_file.close()
                if scan_ids:
                    id_list = ",".join(str(scan_id) for scan_id in scan_ids)
                    execute(db, f"DELETE FROM discoveryScanJob WHERE scanId IN ({id_list})")
                execute(db, f"DELETE FROM discovered WHERE ip='{TARGET_IP}' AND kind='host'")
                execute(db, "DELETE FROM serverLease WHERE id=1")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
