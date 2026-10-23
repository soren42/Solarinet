#!/usr/bin/env python3
"""Render Forgejo CI from GitHub CI with the verified host-runner contract."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GITHUB = ROOT / ".github/workflows/ci.yml"
FORGEJO = ROOT / ".forgejo/workflows/ci.yml"
MACOS_BOUNDARY = "\n  # ---- macOS universal (native runner; §12 macos-universal)"

START_STEPS = {
    "sorsync": """      - name: Start loopback-only disposable broker and database
        run: |
          docker rm -f solari-ci-sorsync-rabbitmq solari-ci-sorsync-db >/dev/null 2>&1 || true
          docker run --detach --name solari-ci-sorsync-rabbitmq \\
            --publish 127.0.0.1:5672:5672 \\
            --env RABBITMQ_DEFAULT_USER=solarinet_fixture \\
            --env RABBITMQ_DEFAULT_PASS=delivery_fixture \\
            --env RABBITMQ_DEFAULT_VHOST=solari \\
            --health-cmd='rabbitmq-diagnostics -q check_running' \\
            --health-interval=5s --health-timeout=5s --health-retries=20 \\
            "${{ matrix.rabbitmq_image }}"
          docker run --detach --name solari-ci-sorsync-db \\
            --publish 127.0.0.1:3306:3306 \\
            --env MARIADB_ROOT_PASSWORD=sorsync_fixture \\
            --env MARIADB_DATABASE=solarinet_sorsync_ci \\
            --health-cmd='mariadb-admin ping -uroot -psorsync_fixture' \\
            --health-interval=5s --health-timeout=5s --health-retries=20 \\
            mariadb:11
          wait_healthy() {
            name="$1"
            for attempt in $(seq 1 120); do
              state="$(docker inspect --format '{{.State.Health.Status}}' "$name" 2>/dev/null || true)"
              [ "$state" = healthy ] && return 0
              sleep 1
            done
            docker logs "$name"
            return 1
          }
          wait_healthy solari-ci-sorsync-rabbitmq
          wait_healthy solari-ci-sorsync-db
""",
    "server": """      - name: Start loopback-only disposable server database
        run: |
          docker rm -f solari-ci-server-db >/dev/null 2>&1 || true
          docker run --detach --name solari-ci-server-db \\
            --publish 127.0.0.1:3306:3306 \\
            --env MARIADB_ROOT_PASSWORD=serverci_scratch \\
            --env MARIADB_DATABASE=solarinet_receipts_ci \\
            --health-cmd='mariadb-admin ping -uroot -pserverci_scratch' \\
            --health-interval=5s --health-timeout=5s --health-retries=20 \\
            mariadb:11
          for attempt in $(seq 1 120); do
            state="$(docker inspect --format '{{.State.Health.Status}}' solari-ci-server-db 2>/dev/null || true)"
            [ "$state" = healthy ] && exit 0
            sleep 1
          done
          docker logs solari-ci-server-db
          exit 1
""",
    "dashboard": """      - name: Start loopback-only disposable dashboard database
        run: |
          docker rm -f solari-ci-dashboard-db >/dev/null 2>&1 || true
          docker run --detach --name solari-ci-dashboard-db \\
            --publish 127.0.0.1:3306:3306 \\
            --env MARIADB_ROOT_PASSWORD=rwci_scratch \\
            --env MARIADB_DATABASE=solarinet_stage \\
            --health-cmd='mariadb-admin ping -uroot -prwci_scratch' \\
            --health-interval=5s --health-timeout=5s --health-retries=20 \\
            mariadb:11
          for attempt in $(seq 1 120); do
            state="$(docker inspect --format '{{.State.Health.Status}}' solari-ci-dashboard-db 2>/dev/null || true)"
            [ "$state" = healthy ] && exit 0
            sleep 1
          done
          docker logs solari-ci-dashboard-db
          exit 1
""",
}

CLEANUP_STEPS = {
    "sorsync": """      - name: Remove disposable broker and database
        if: always()
        run: docker rm -f solari-ci-sorsync-rabbitmq solari-ci-sorsync-db || true
""",
    "server": """      - name: Remove disposable server database
        if: always()
        run: docker rm -f solari-ci-server-db || true
""",
    "dashboard": """      - name: Remove disposable dashboard database
        if: always()
        run: docker rm -f solari-ci-dashboard-db || true
""",
}


def job_ranges(text: str) -> dict[str, tuple[int, int]]:
    jobs_at = text.index("jobs:\n") + len("jobs:\n")
    matches = list(re.finditer(r"(?m)^  ([A-Za-z0-9_-]+):\n", text[jobs_at:]))
    ranges: dict[str, tuple[int, int]] = {}
    for index, match in enumerate(matches):
        start = jobs_at + match.start()
        end = jobs_at + (matches[index + 1].start() if index + 1 < len(matches) else len(text) - jobs_at)
        ranges[match.group(1)] = (start, end)
    return ranges


def replace_once(text: str, old: str, new: str, description: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"expected exactly one {description}, found {count}")
    return text.replace(old, new, 1)


def adapt_job(block: str, job: str) -> str:
    block = block.replace("runs-on: ubuntu-latest", "runs-on: solarinet-ci-host")
    service_start = block.find("    services:\n")
    steps_start = block.find("    steps:\n")
    if service_start >= 0:
        if steps_start < service_start:
            raise ValueError(f"unexpected services/steps order in {job}")
        block = block[:service_start] + block[steps_start:]
    if job in START_STEPS:
        checkout = "      - uses: actions/checkout@v4\n"
        at = block.find(checkout)
        if at < 0:
            raise ValueError(f"checkout step missing in {job}")
        at += len(checkout)
        if block.startswith("        with:\n", at):
            options_end = block.find("      - ", at)
            if options_end < 0:
                raise ValueError(f"checkout options block is unterminated in {job}")
            at = options_end
        block = block[:at] + START_STEPS[job] + block[at:]
        block = block.rstrip() + "\n" + CLEANUP_STEPS[job]
    return block


def render() -> str:
    source = GITHUB.read_text(encoding="utf-8")
    if MACOS_BOUNDARY not in source:
        raise ValueError("GitHub workflow no longer has the expected native macOS boundary")
    source = source.split(MACOS_BOUNDARY, 1)[0].rstrip() + "\n"
    source = replace_once(source, "name: ci\n", "name: ci-forgejo\n", "workflow name")
    source = replace_once(
        source,
        "  push:\n    branches: [ main ]",
        "  push:\n    branches: [ main, 'integrate/**' ]",
        "push branch filter",
    )
    source = replace_once(
        source,
        "group: ci-${{ github.ref }}",
        "group: ci-forgejo-${{ forgejo.ref }}",
        "concurrency group",
    )
    if source.count("uses: actions/upload-artifact@v4") != 1:
        raise ValueError("expected exactly one v4 artifact upload action")
    source = source.replace("uses: actions/upload-artifact@v4", "uses: actions/upload-artifact@v3")
    if source.count("uses: actions/download-artifact@v4") != 1:
        raise ValueError("expected exactly one v4 artifact download action")
    source = source.replace("uses: actions/download-artifact@v4", "uses: actions/download-artifact@v3")
    source = source.replace("runs-on: ubuntu-latest", "runs-on: solarinet-ci-host")

    spans = job_ranges(source)
    for job in ("sorsync", "server", "dashboard"):
        if job not in spans:
            raise ValueError(f"required service job missing: {job}")
    dashboard_start, dashboard_end = spans["dashboard"]
    dashboard = source[dashboard_start:dashboard_end]
    dashboard_gate = "    if: ${{ !cancelled() }}\n"
    if dashboard.count(dashboard_gate) != 1:
        raise ValueError(
            "dashboard must run unless cancelled; its needs edge enforces server success"
        )
    for job, (start, end) in sorted(spans.items(), key=lambda item: item[1][0], reverse=True):
        block = source[start:end]
        source = source[:start] + adapt_job(block, job) + source[end:]

    source = source.replace(
        "#\n# macos-universal is exercised on the native macOS runner (no qemu needed).\n",
        "#\n# Native macOS coverage remains in the GitHub-specific workflow.\n",
        1,
    )
    source = source.replace(
        "# SolariNet CI (Architecture & Plan §15 / Phase 7).\n",
        "# SolariNet CI for a dedicated disposable Forgejo host runner.\n",
        1,
    )
    source += (
        "\n# Requires runner label solarinet-ci-host in host mode with capacity=1.\n"
        "# Jobs execute on that disposable VM, keep DB/broker fixtures on loopback,\n"
        "# and use Docker for short-lived service containers. Never use a shared host.\n"
    )
    return source


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if the checked-in Forgejo workflow is stale")
    parser.add_argument("--write", action="store_true", help="write the rendered workflow")
    args = parser.parse_args()
    if args.check == args.write:
        parser.error("choose exactly one of --check or --write")
    try:
        expected = render()
    except (ValueError, OSError) as exc:
        print(f"cannot render Forgejo workflow: {exc}", file=sys.stderr)
        return 1
    if args.write:
        FORGEJO.parent.mkdir(parents=True, exist_ok=True)
        FORGEJO.write_text(expected, encoding="utf-8")
        return 0
    try:
        actual = FORGEJO.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"Forgejo workflow missing: {exc}", file=sys.stderr)
        return 1
    if actual != expected:
        print("Forgejo workflow is out of date; run tools/ci/render_forgejo_workflow.py --write", file=sys.stderr)
        return 1
    print("Forgejo CI parity and host-runner adaptation: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
