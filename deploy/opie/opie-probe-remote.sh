#!/usr/bin/env bash
# Restricted remote command target for the solari-probe SSH account.
# Install as /usr/local/libexec/solari-opie-remote and force it from
# authorized_keys. Never eval or shell-interpret SSH_ORIGINAL_COMMAND.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

die() { echo "solari-opie-remote: $*" >&2; exit 2; }

if [[ -v SSH_ORIGINAL_COMMAND ]]; then
  read -r -a request <<< "$SSH_ORIGINAL_COMMAND"
  [[ "${request[0]:-}" == "/usr/local/libexec/solari-opie-remote" ]] || die "command rejected"
  set -- "${request[@]:1}"
fi

(( $# >= 1 && $# <= 2 )) || die "invalid request"
CHECK="$1"
ARG="${2:-}"
if [[ -n "$ARG" ]]; then
  case "$ARG" in *[!a-zA-Z0-9._@:/-]*) die "invalid argument" ;; esac
fi

case "$CHECK" in
  failed-units) systemctl --failed --no-legend --plain ;;
  unit)
    [[ -n "$ARG" ]] || die "unit needs an argument"
    systemctl status --no-pager -n 40 -- "$ARG" ;;
  logs)
    [[ -n "$ARG" ]] || die "logs needs an argument"
    journalctl -u "$ARG" -n 150 --no-pager --output=short-iso ;;
  kernel)
    journalctl -k -n 200 --no-pager --output=short-iso 2>/dev/null || dmesg 2>/dev/null | tail -200 ;;
  disk)
    df -hP
    echo ---BLKDEV---
    lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT,LABEL 2>/dev/null ;;
  mounts) findmnt -rno TARGET,SOURCE,FSTYPE,OPTIONS 2>/dev/null | head -60 ;;
  smart)
    [[ -n "$ARG" ]] || die "smart needs a device"
    [[ "$ARG" =~ ^[a-zA-Z0-9]+$ ]] || die "invalid device"
    sudo -n smartctl -H -A "/dev/$ARG" 2>/dev/null || echo 'smartctl unavailable/needs privilege' ;;
  net)
    ip -br addr
    echo ---LISTEN---
    ss -tlnp 2>/dev/null | head -50 ;;
  proc)
    ps -eo pid,comm,%cpu,%mem --sort=-%cpu 2>/dev/null | head -20
    echo ---MEM---
    ps -eo pid,comm,%cpu,%mem --sort=-%mem 2>/dev/null | head -8 ;;
  load)
    uptime
    echo ---MEM---
    free -h
    echo ---
    cat /proc/loadavg ;;
  dmesg-errors)
    journalctl -k -p err -n 120 --no-pager --output=short-iso 2>/dev/null || dmesg --level=err,crit,alert,emerg 2>/dev/null | tail -120 ;;
  service)
    [[ -n "$ARG" ]] || die "service needs a unit"
    systemctl show --no-pager -p LoadState -p ActiveState -p SubState -p Result \
      -p ExecMainStatus -p FragmentPath -p UnitFileState -p User -p Group \
      -p Restart -- "$ARG" ;;
  ping)
    echo alive
    hostname
    uptime -p ;;
  *) die "unknown check" ;;
esac
