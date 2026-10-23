#!/usr/bin/env python3
"""Render one-job cloud-init data without placing runner credentials in argv."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

SECRET_PREFIX = "534f4c4152494349"


def render(token_path: Path, uuid_path: Path, bootstrap_path: Path) -> str:
    token = token_path.read_text().strip()
    runner_uuid = uuid_path.read_text().strip()
    if not re.fullmatch(rf"{SECRET_PREFIX}[0-9a-f]{{24}}", token):
        raise ValueError("invalid one-job runner token")
    if not re.fullmatch(r"[0-9a-fA-F-]{36}", runner_uuid):
        raise ValueError("invalid runner UUID")
    bootstrap = bootstrap_path.read_text()
    indented_bootstrap = "\n".join(f"      {line}" for line in bootstrap.rstrip().splitlines())
    return f"""#cloud-config
hostname: solarinet-ci
manage_etc_hosts: true
users:
  - name: runner
    gecos: Solarinet CI runner
    groups: [sudo, docker]
    shell: /bin/bash
    sudo: ["ALL=(ALL) NOPASSWD:ALL"]
    lock_passwd: true
package_update: true
package_upgrade: false
bootcmd:
  - [systemd-run, --unit=solarinet-ci-bootstrap-deadline, --on-active=45m, /usr/bin/systemctl, poweroff, --force]
packages:
  - ca-certificates
  - curl
  - docker.io
  - git
  - iproute2
  - jq
  - nftables
  - openssh-client
  - python3
  - python3-pip
  - python3-venv
  - sudo
  - xz-utils
write_files:
  - path: /etc/solarinet-ci/runner-token
    owner: root:root
    permissions: '0600'
    content: {json.dumps(token)}
  - path: /etc/solarinet-ci/runner-uuid
    owner: root:root
    permissions: '0600'
    content: {json.dumps(runner_uuid)}
  - path: /usr/local/sbin/solarinet-ci-guest-bootstrap
    owner: root:root
    permissions: '0755'
    content: |
{indented_bootstrap}
runcmd:
  - [bash, /usr/local/sbin/solarinet-ci-guest-bootstrap]
"""


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: render-user-data.py TOKEN_FILE UUID_FILE BOOTSTRAP", file=sys.stderr)
        return 2
    try:
        sys.stdout.write(render(*(Path(value) for value in sys.argv[1:])))
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
