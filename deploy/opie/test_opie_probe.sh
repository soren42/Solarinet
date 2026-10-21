#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/ssh"
touch "$tmp/ssh/id_ed25519" "$tmp/ssh/known_hosts"
printf '%s\n' 'node-a.akoria.net' > "$tmp/hosts"
cat > "$tmp/bin/ssh" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$*" > "$OPIE_TEST_SSH_ARGS"
printf 'alive\n'
SH
chmod +x "$tmp/bin/ssh"

env PATH="$tmp/bin:$PATH" \
  OPIE_TEST_SSH_ARGS="$tmp/ssh-args" \
  OPIE_PROBE_HOSTS_FILE="$tmp/hosts" \
  OPIE_PROBE_KNOWN_HOSTS="$tmp/ssh/known_hosts" \
  OPIE_PROBE_SSH_IDENTITY="$tmp/ssh/id_ed25519" \
  OPIE_PROBE_SSH_USER=solari-probe \
  "$ROOT/opie-probe.sh" node-a ping > "$tmp/allowed-output"
grep -Fxq 'alive' "$tmp/allowed-output"
grep -Fq -- '-o StrictHostKeyChecking=yes' "$tmp/ssh-args"
grep -Fq -- '-l solari-probe node-a.akoria.net' "$tmp/ssh-args"
grep -Fq -- "-o UserKnownHostsFile=$tmp/ssh/known_hosts" "$tmp/ssh-args"
grep -Fq -- '/usr/local/libexec/solari-opie-remote ping ' "$tmp/ssh-args"
! grep -Fq 'accept-new' "$tmp/ssh-args"

if env PATH="$tmp/bin:$PATH" \
  OPIE_TEST_SSH_ARGS="$tmp/should-not-run" \
  OPIE_PROBE_HOSTS_FILE="$tmp/hosts" \
  OPIE_PROBE_KNOWN_HOSTS="$tmp/ssh/known_hosts" \
  OPIE_PROBE_SSH_IDENTITY="$tmp/ssh/id_ed25519" \
  "$ROOT/opie-probe.sh" 10.7.0.10 ping > /dev/null 2>&1; then
  echo 'unexpected unlisted host accepted' >&2
  exit 1
fi
[ ! -e "$tmp/should-not-run" ]
echo 'Opie probe host and SSH boundary tests passed.'
