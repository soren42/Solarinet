#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
SSH_ORIGINAL_COMMAND='/usr/local/libexec/solari-opie-remote ping' \
  bash "$ROOT/opie-probe-remote.sh" > "$tmp/output"
grep -Fxq 'alive' "$tmp/output"

grep -Fq 'systemctl show --no-pager -p LoadState -p ActiveState -p SubState -p Result' "$ROOT/opie-probe-remote.sh"
! grep -Fq 'systemctl cat' "$ROOT/opie-probe-remote.sh"
! grep -Fq 'ps aux' "$ROOT/opie-probe-remote.sh"

if \
  SSH_ORIGINAL_COMMAND='/usr/local/libexec/solari-opie-remote ping; id' \
  bash "$ROOT/opie-probe-remote.sh" > /dev/null 2>&1; then
  echo 'unexpected command accepted' >&2
  exit 1
fi
echo 'Opie forced-command protocol tests passed.'
