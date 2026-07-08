#!/bin/bash
# opie-probe — the ONLY way Opie (the AI on-call SA) touches a host. Every check
# is a vetted, READ-ONLY command run over SSH. There is no code path here that
# writes, deletes, restarts, or configures anything. Opie's Claude instance is
# granted only `Bash(opie-probe:*)` + Read/Grep, so this allowlist is the whole
# blast radius of an investigation.
#
# USAGE: opie-probe <host> <check> [arg]
#   host  : an akoria fleet host (name or 10.x IP) — validated, not arbitrary
#   check : one of the read-only checks below
#   arg   : optional (unit name / device) — charset-validated
set -euo pipefail

HOST="${1:-}"; CHECK="${2:-}"; ARG="${3:-}"
die() { echo "opie-probe: $*" >&2; exit 2; }
[ -n "$HOST" ] && [ -n "$CHECK" ] || die "usage: opie-probe <host> <check> [arg]"

# --- host allowlist: fleet names/IPs only, safe charset, must resolve/reach ---
case "$HOST" in
  *[!a-zA-Z0-9.-]*) die "illegal host '$HOST'" ;;
esac
case "$HOST" in
  *.akoria.net|10.*|localhost|127.0.0.1) : ;;
  *) if [[ "$HOST" =~ ^[a-z][a-z0-9-]*$ ]]; then HOST="$HOST.akoria.net"; else die "host not in fleet scope: $HOST"; fi ;;
esac

# --- arg charset: unit names, device nodes, iface names only ---
if [ -n "$ARG" ]; then
  case "$ARG" in
    *[!a-zA-Z0-9._@:/-]*) die "illegal arg '$ARG'" ;;
  esac
fi

SSHOPTS=(-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new)

# Local host -> run directly (no self-SSH). Match name/fqdn/IPs of this machine.
_localset=" localhost 127.0.0.1 $(hostname 2>/dev/null) $(hostname -f 2>/dev/null) $(hostname -I 2>/dev/null) "
is_local() { case "$_localset" in *" $1 "*) return 0 ;; esac; [ "$1" = "$(hostname -s 2>/dev/null).akoria.net" ] && return 0; return 1; }
run() {
  if is_local "$HOST"; then bash -c "$1" 2>&1 | head -300
  else ssh "${SSHOPTS[@]}" "jason@$HOST" "$1" 2>&1 | head -300; fi
}

case "$CHECK" in
  failed-units)  run 'systemctl --failed --no-legend --plain' ;;
  unit)          [ -n "$ARG" ] || die "unit needs a unit name"; run "systemctl status --no-pager -n 40 -- '$ARG'" ;;
  logs)          [ -n "$ARG" ] || die "logs needs a unit name"; run "journalctl -u '$ARG' -n 150 --no-pager --output=short-iso" ;;
  kernel)        run 'journalctl -k -n 200 --no-pager --output=short-iso 2>/dev/null || dmesg 2>/dev/null | tail -200' ;;
  disk)          run 'df -hP; echo ---BLKDEV---; lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT,LABEL 2>/dev/null' ;;
  mounts)        run 'findmnt -rno TARGET,SOURCE,FSTYPE,OPTIONS 2>/dev/null | head -60' ;;
  smart)         [ -n "$ARG" ] || die "smart needs a device"; run "sudo -n smartctl -H -A /dev/'$ARG' 2>/dev/null || echo 'smartctl unavailable/needs privilege'" ;;
  net)           run 'ip -br addr; echo ---LISTEN---; ss -tlnp 2>/dev/null | head -50' ;;
  proc)          run 'ps aux --sort=-%cpu 2>/dev/null | head -20; echo ---MEM---; ps aux --sort=-%mem 2>/dev/null | head -8' ;;
  load)          run 'uptime; echo ---MEM---; free -h; echo ---; cat /proc/loadavg' ;;
  dmesg-errors)  run "journalctl -k -p err -n 120 --no-pager --output=short-iso 2>/dev/null || dmesg --level=err,crit,alert,emerg 2>/dev/null | tail -120" ;;
  service-conf)  [ -n "$ARG" ] || die "service-conf needs a unit"; run "systemctl cat '$ARG' 2>/dev/null | head -80" ;;
  ping)          run 'echo alive; hostname; uptime -p' ;;
  *) die "unknown check '$CHECK' (allowed: failed-units unit logs kernel disk mounts smart net proc load dmesg-errors service-conf ping)" ;;
esac
