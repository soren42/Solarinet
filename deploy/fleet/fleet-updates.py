#!/usr/bin/env python3
"""Check for, or apply, operating system package updates across the fleet.

The roster is the System of Record: active servers, workstations and appliances
with a distribution recorded as their OS. Active servers and workstations
without one are named as skipped. Ansible runs on the provisioning host, one host
at a time, each in its own run so that a host's failure or login cannot affect the next.
Hosts this run depends on go last. Nothing here reboots a host.

The log is the interface to the dashboard. Lines it parses:
  PENDING <fqdn> count=<n> new=<n> remove=<n>
  RESULT <fqdn> status=<complete|failed> reboot_required=<yes|no|unknown>
  SKIPPED <fqdn>: <reason>
  FLEET_UPDATES exit=<n>     (last line; the run has finished)
"""
from __future__ import annotations

import argparse
import fcntl
import importlib.util
import os
import re
import socket
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import provlib  # noqa: E402
from provlib import ProvError  # noqa: E402

_spec = importlib.util.spec_from_file_location("stack_apply", os.path.join(HERE, "stack-apply.py"))
stack_apply = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stack_apply)

DEFAULT_DOMAIN = stack_apply.DEFAULT_DOMAIN
HOST_LABEL_RE = stack_apply.HOST_LABEL_RE
# SoR, Ansible control host, and the host running this helper: in that order, last.
CORE_LAST = ("cesium", "benzene", "xenon")
TIMEOUT = {"check": 600, "apply": 3600}
LOCK_FILE = "fleet-updates.lock"
MARKER_RE = re.compile(r'SOLARI_UPDATE (PENDING|RESULT|SKIPPED) ?([^"]*)')
FATAL_RE = re.compile(r'^(?:fatal|failed): .*?"msg": "([^"]{1,200})')
RC_UNREACHABLE = 4


def load_roster(cur) -> tuple[list[dict], list[str]]:
    """(managed rows, names of active hosts with no distribution recorded)."""
    # Rows named "Linux <kernel>" in operating_systems are monitoring's kernel
    # strings, not distributions.
    cur.execute("SELECT e.id, e.name, e.entity_type, o.name AS os FROM entities e"
                " LEFT JOIN operating_systems o ON o.id=e.os_id"
                " WHERE e.deleted_at IS NULL AND e.lifecycle='active'"
                " AND e.entity_type IN ('server','workstation','appliance')")
    managed, unknown = [], []
    for row in cur.fetchall():
        if row["os"] and not row["os"].startswith("Linux ") and HOST_LABEL_RE.match(row["name"] or ""):
            managed.append(row)
        elif row["entity_type"] != "appliance":
            unknown.append(row["name"])
    managed.sort(key=lambda r: (CORE_LAST.index(r["name"]) + 1 if r["name"] in CORE_LAST else 0, r["name"]))
    return managed, sorted(unknown)


def parse_host_output(text: str) -> dict[str, str]:
    """The marker lines the playbook printed for one host, by kind."""
    found = {}
    for line in text.splitlines():
        m = MARKER_RE.search(line)
        if m:
            found[m.group(1)] = m.group(2).strip()
    return found


def failure_reason(text: str, rc: int) -> str:
    for line in reversed(text.splitlines()):
        m = FATAL_RE.search(line)
        if m:
            return m.group(1)
    return f"ansible-playbook exit {rc}"


def run_host(ansible_dir: str, fqdn: str, user: str | None, mode: str, hold_restarts: bool) -> int:
    """Run one host and print its log lines. Returns 1 when the host failed."""
    argv = ["env", "-C", ansible_dir, "ANSIBLE_LOAD_CALLBACK_PLUGINS=0",
            "ANSIBLE_STDOUT_CALLBACK=ansible.builtin.default", "ANSIBLE_CALLBACK_RESULT_FORMAT=json",
            "ansible-playbook", "-i", f"{fqdn},", *(["-u", user] if user else []),
            "playbooks/updates.yml", "-e", f"solari_update_mode={mode}",
            "-e", f"solari_update_hold_restarts={'true' if hold_restarts else 'false'}"]
    print(f"HOST {fqdn}", flush=True)
    try:
        proc = provlib.remote_run(argv, check=False, timeout=TIMEOUT[mode])
    except ProvError as exc:
        print(f"RESULT {fqdn} status=failed reboot_required=unknown", flush=True)
        print(f"{fqdn}: {exc}", flush=True)
        return 1
    text = (proc.stdout or b"").decode("utf-8", "replace")
    found = parse_host_output(text)
    if "SKIPPED" in found:
        print(f"SKIPPED {fqdn}: {found['SKIPPED']}", flush=True)
        return 0
    if proc.returncode == RC_UNREACHABLE:
        reason = "the control host's login was refused" if "Permission denied" in text else "unreachable"
        print(f"SKIPPED {fqdn}: {reason}", flush=True)
        return 0
    if "PENDING" in found:
        print(f"PENDING {fqdn} {found['PENDING']}", flush=True)
    if proc.returncode != 0:
        print(f"RESULT {fqdn} status=failed reboot_required=unknown", flush=True)
        print(f"{fqdn}: {failure_reason(text, proc.returncode)}", flush=True)
        return 1
    if mode == "apply":
        print(f"RESULT {fqdn} status=complete {found.get('RESULT', 'reboot_required=unknown')}", flush=True)
    return 0


def run(mode: str, op: str) -> int:
    domain = os.environ.get("SOLARI_PROV_DOMAIN", DEFAULT_DOMAIN)
    prov_root = provlib.remote_env()["PROV_ROOT"].rstrip("/")
    conn = provlib.connect_sor()
    try:
        logins = {}
        with conn.cursor() as cur:
            managed, unknown = load_roster(cur)
            for row in managed:
                logins[row["id"]] = stack_apply.last_reimage_run(cur, row["id"])
        conn.commit()
    finally:
        conn.close()
    print(f"FLEET_UPDATES mode={mode} operator={op} hosts={len(managed)}", flush=True)
    print("Hosts run one at a time. This operation never reboots a host.", flush=True)
    for name in unknown:
        print(f"SKIPPED {name}: no operating system recorded", flush=True)
    me = socket.gethostname().split(".")[0]
    failed = 0
    for row in managed:
        fqdn = f"{row['name']}.{domain}"
        try:
            user = stack_apply.admin_login(prov_root, row["id"], fqdn, logins[row["id"]])
        except ProvError as exc:
            print(f"RESULT {fqdn} status=failed reboot_required=unknown\n{fqdn}: {exc}", flush=True)
            failed += 1
            continue
        failed += run_host(f"{prov_root}/ansible", fqdn, user, mode, row["name"] == me)
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--mode", choices=("check", "apply"), required=True)
    parser.add_argument("--op", required=True)
    args = parser.parse_args(argv)
    try:
        op = provlib.parse_principal(args.op)
    except ProvError as exc:
        parser.error(str(exc))
    try:
        # A second run must not start a second package manager on any host. The
        # lock dies with this process, and each run has its own log.
        with open(os.path.join(provlib.status_dir(), LOCK_FILE), "w", encoding="utf-8") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                print("ERROR: another update run is in progress", flush=True)
                status = 3
            else:
                status = run(args.mode, op)
    except (ProvError, OSError) as exc:
        print(f"ERROR: {exc}", flush=True)
        status = 2
    print(f"FLEET_UPDATES exit={status}", flush=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
