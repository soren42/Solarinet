#!/usr/bin/env python3
"""Run a bounded, serial OS update check or apply through Ansible.

Only hosts in the existing fleet Ansible inventory are contacted. The playbook
reports unsupported inventory entries as skipped and never requests a reboot.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "ansible"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("check", "apply"), required=True)
    parser.add_argument("--op", required=True)
    args = parser.parse_args()
    if not args.op or len(args.op) > 128:
        parser.error("invalid operator")
    if not (ROOT / "ansible.cfg").is_file() or not (ROOT / "playbooks/updates.yml").is_file():
        print("ERROR: fleet Ansible update playbook is not installed", flush=True)
        return 2
    print(f"FLEET_UPDATES mode={args.mode} operator={args.op}", flush=True)
    print("Hosts run serially. This operation never reboots a host.", flush=True)
    command = ["ansible-playbook", "--inventory", "inventory/hosts.yml",
               "playbooks/updates.yml", "--forks", "1", "--extra-vars",
               f"solari_update_mode={args.mode}"]
    env = os.environ.copy()
    try:
        result = subprocess.run(command, cwd=ROOT, env=env, timeout=7200, check=False)
    except subprocess.TimeoutExpired:
        print("ERROR: update run exceeded its two-hour limit", flush=True)
        return 124
    except OSError as exc:
        print(f"ERROR: could not start Ansible ({exc.strerror or 'launch failed'})", flush=True)
        return 127
    print(f"FLEET_UPDATES exit={result.returncode}", flush=True)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
