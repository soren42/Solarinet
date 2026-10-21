#!/bin/bash
# opie-probe — the ONLY way Opie (the AI on-call SA) touches a host. Every check
# is a vetted, READ-ONLY command run over SSH. There is no code path here that
# writes, deletes, restarts, or configures anything. Claude can call it only
# through the structured `mcp__opie__probe` server; shell access is denied.
#
# USAGE: opie-probe <host> <check> [arg]
#   host  : a host listed exactly in OPIE_PROBE_HOSTS_FILE
#   check : one of the read-only checks below
#   arg   : optional (unit name / device) — charset-validated
set -euo pipefail

HOST="${1:-}"; CHECK="${2:-}"; ARG="${3:-}"
die() { echo "opie-probe: $*" >&2; exit 2; }
[ -n "$HOST" ] && [ -n "$CHECK" ] || die "usage: opie-probe <host> <check> [arg]"

# --- normalize a short name, then require an exact inventory entry ---
case "$HOST" in
  *[!a-zA-Z0-9.-]*) die "illegal host '$HOST'" ;;
esac
case "$HOST" in
  *.*) : ;;
  *) if [[ "$HOST" =~ ^[a-z][a-z0-9-]*$ ]]; then HOST="$HOST.akoria.net"; else die "host not in fleet scope: $HOST"; fi ;;
esac
HOSTS_FILE="${OPIE_PROBE_HOSTS_FILE:-/etc/solarinet/opie-hosts.allow}"
[ -r "$HOSTS_FILE" ] || die "host allowlist is unavailable"
grep -Fxq -- "$HOST" "$HOSTS_FILE" || die "host is not in the configured inventory allowlist: $HOST"

# --- arg charset: unit names, device nodes, iface names only ---
if [ -n "$ARG" ]; then
  case "$ARG" in
    *[!a-zA-Z0-9._@:/-]*) die "illegal arg '$ARG'" ;;
  esac
fi

SSH_USER="${OPIE_PROBE_SSH_USER:-solari-probe}"
SSH_IDENTITY="${OPIE_PROBE_SSH_IDENTITY:-${HOME:-/var/lib/solari-opie}/.ssh/id_ed25519}"
KNOWN_HOSTS="${OPIE_PROBE_KNOWN_HOSTS:-${HOME:-/var/lib/solari-opie}/.ssh/known_hosts}"
[[ "$SSH_USER" =~ ^[a-z_][a-z0-9_-]*[$]?$ ]] || die "invalid configured SSH user"
SSHOPTS=(-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=yes
         -o "UserKnownHostsFile=$KNOWN_HOSTS" -o IdentitiesOnly=yes -i "$SSH_IDENTITY")
REMOTE_COMMAND="${OPIE_PROBE_REMOTE_COMMAND:-/usr/local/libexec/solari-opie-remote}"
REMOTE_SCRIPT="${OPIE_PROBE_REMOTE_SCRIPT:-$(dirname -- "$0")/opie-probe-remote.sh}"

# Local host -> run directly (no self-SSH). Match name/fqdn/IPs of this machine.
_localset=" localhost 127.0.0.1 $(hostname 2>/dev/null) $(hostname -f 2>/dev/null) $(hostname -I 2>/dev/null) "
is_local() { case "$_localset" in *" $1 "*) return 0 ;; esac; [ "$1" = "$(hostname -s 2>/dev/null).akoria.net" ] && return 0; return 1; }
run() {
  local check="$1" arg="${2:-}"
  if is_local "$HOST"; then "$REMOTE_SCRIPT" "$check" "$arg" 2>&1 | head -300
  else
    [ -r "$KNOWN_HOSTS" ] || die "pinned SSH known_hosts file is unavailable"
    [ -r "$SSH_IDENTITY" ] || die "dedicated SSH identity is unavailable"
    ssh "${SSHOPTS[@]}" -l "$SSH_USER" "$HOST" "$REMOTE_COMMAND $check $arg" 2>&1 | head -300
  fi
}

case "$CHECK" in
  failed-units)  run failed-units ;;
  unit)          [ -n "$ARG" ] || die "unit needs a unit name"; run unit "$ARG" ;;
  logs)          [ -n "$ARG" ] || die "logs needs a unit name"; run logs "$ARG" ;;
  kernel)        run kernel ;;
  disk)          run disk ;;
  mounts)        run mounts ;;
  smart)         [ -n "$ARG" ] || die "smart needs a device"; run smart "$ARG" ;;
  net)           run net ;;
  proc)          run proc ;;
  load)          run load ;;
  dmesg-errors)  run dmesg-errors ;;
  service)       [ -n "$ARG" ] || die "service needs a unit"; run service "$ARG" ;;
  ping)          run ping ;;
  *) die "unknown check '$CHECK' (allowed: failed-units unit logs kernel disk mounts smart net proc load dmesg-errors service ping)" ;;
esac
