#!/usr/bin/env bash
# Install the release-backed Xenon units and restart each with a health check.
set -uo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT=/
SYSTEMCTL=systemctl
DRY_RUN=0
usage() { echo "usage: $0 [--dry-run --root TEMP_ROOT] [--systemctl MOCK]" >&2; }
while (($#)); do
  case "$1" in
    --root) (($# >= 2)) || { usage; exit 2; }; ROOT="$2"; shift 2 ;;
    --systemctl) (($# >= 2)) || { usage; exit 2; }; SYSTEMCTL="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done
if [[ "$ROOT" != / && "$DRY_RUN" -ne 1 ]]; then
  echo "--root is restricted to --dry-run" >&2; exit 2
fi
if [[ "$DRY_RUN" -ne 1 && "$EUID" -ne 0 ]]; then
  echo "run this cutover as root" >&2; exit 2
fi
if [[ "$DRY_RUN" -eq 1 && "$ROOT" == / ]]; then
  echo "--dry-run requires a temporary --root" >&2; exit 2
fi

UNITS=(sor-emit sor-apply-dns sor-apply-pihole sor-reconcile notify alertbridge unifipolld solarinet-client solarinet-monitor solarinet-snmp solari-mdns-inspect solarinet-server)
UNIT_DIR="${ROOT%/}/etc/systemd/system"
BACKUP="${ROOT%/}/var/backups/solarinet-immutable-$(date -u +%Y%m%dT%H%M%SZ)-$BASHPID"
ALERTBRIDGE_CONF="${ROOT%/}/home/jason/Code/Solarinet/deploy/alertbridge/alertbridge.conf"
mkdir -p "$UNIT_DIR" "$BACKUP/units"
: > "$BACKUP/existed"
for unit in "${UNITS[@]}"; do
  name="$unit.service"
  [[ -f "$SCRIPT_DIR/$name" ]] || { echo "missing candidate unit: $name" >&2; exit 1; }
  if [[ -e "$UNIT_DIR/$name" ]]; then
    cp -a "$UNIT_DIR/$name" "$BACKUP/units/$name"
    echo "$name" >> "$BACKUP/existed"
  fi
done
printf '%s\n' "${UNITS[@]/%/.service}" > "$BACKUP/units.list"
if [[ -f "$ALERTBRIDGE_CONF" ]]; then
  mkdir -p "$BACKUP/config"
  cp -p "$ALERTBRIDGE_CONF" "$BACKUP/config/alertbridge.conf"
  : > "$BACKUP/config-existed"
elif [[ "$DRY_RUN" -eq 0 ]]; then
  echo "required alertbridge config is missing: $ALERTBRIDGE_CONF" >&2
  exit 1
fi
echo "unit backup: $BACKUP"

restore_units() {
  local unit name
  for unit in "${UNITS[@]}"; do
    name="$unit.service"
    if grep -Fxq "$name" "$BACKUP/existed"; then
      cp -a "$BACKUP/units/$name" "$UNIT_DIR/$name" || return 1
    else
      rm -f "$UNIT_DIR/$name" || return 1
    fi
  done
  if [[ -f "$BACKUP/config-existed" ]]; then
    cp -p "$BACKUP/config/alertbridge.conf" "$ALERTBRIDGE_CONF" || return 1
  fi
}
preserve_alertbridge_state_path() {
  [[ -f "$ALERTBRIDGE_CONF" ]] || return 0
  local temporary
  temporary="$(mktemp "${ALERTBRIDGE_CONF}.immutable.XXXXXX")" || return 1
  if ! python3 - "$ALERTBRIDGE_CONF" "$temporary" <<'PY'
from pathlib import Path
import re
import sys

source, target = map(Path, sys.argv[1:])
lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
in_state = False
has_state = False
has_file = False
insert_at = None
for index, line in enumerate(lines):
    stripped = line.strip()
    if stripped.startswith("[") and stripped.endswith("]"):
        in_state = stripped[1:-1].strip().lower() == "state"
        if in_state:
            has_state = True
            insert_at = index + 1
    elif in_state and re.match(r"^\s*file\s*=", line, re.IGNORECASE):
        has_file = True
if not has_file:
    setting = "file = /home/jason/Code/Solarinet/deploy/alertbridge/alertbridge.state.json\n"
    if has_state and insert_at is not None:
        lines.insert(insert_at, setting)
    else:
        if lines and not lines[-1].endswith("\n"):
            lines[-1] += "\n"
        lines.extend(["\n[state]\n", setting])
target.write_text("".join(lines), encoding="utf-8")
PY
  then
    rm -f "$temporary"
    return 1
  fi
  chmod --reference="$ALERTBRIDGE_CONF" "$temporary"
  chown --reference="$ALERTBRIDGE_CONF" "$temporary"
  mv -f "$temporary" "$ALERTBRIDGE_CONF"
}
health_check() {
  local unit="$1" type
  if grep -Eq '^Type=oneshot([[:space:]]|$)' "$SCRIPT_DIR/$unit.service"; then
    [[ "$("$SYSTEMCTL" show --property=Result --value "$unit.service")" == success ]] || return 1
    [[ "$("$SYSTEMCTL" show --property=ExecMainStatus --value "$unit.service")" == 0 ]] || return 1
  else
    "$SYSTEMCTL" is-active --quiet "$unit.service" || return 1
  fi
}
was_active=()
attempted=()
: > "$BACKUP/active"
for unit in "${UNITS[@]}"; do
  if "$SYSTEMCTL" is-active --quiet "$unit.service"; then
    was_active+=(1)
    echo "$unit=1" >> "$BACKUP/active"
  else
    was_active+=(0)
    echo "$unit=0" >> "$BACKUP/active"
  fi
done
rollback_after_failure() {
  local index unit result=0
  echo "cutover failed; restoring unit backup" >&2
  restore_units || result=1
  "$SYSTEMCTL" daemon-reload || result=1
  for index in "${!attempted[@]}"; do
    unit="${attempted[$index]}"
    if [[ "${was_active[$index]}" == 1 ]]; then
      "$SYSTEMCTL" restart "$unit.service" || result=1
    else
      "$SYSTEMCTL" stop "$unit.service" || result=1
    fi
  done
  if [[ "$result" -ne 0 ]]; then echo "unit files restored, but service recovery needs operator attention" >&2; fi
  return "$result"
}

for unit in "${UNITS[@]}"; do
  cp "$SCRIPT_DIR/$unit.service" "$UNIT_DIR/$unit.service" || { rollback_after_failure; exit 1; }
done
if ! preserve_alertbridge_state_path; then
  echo "could not preserve alertbridge state path" >&2
  rollback_after_failure
  exit 1
fi
if ! "$SYSTEMCTL" daemon-reload; then rollback_after_failure; exit 1; fi
for index in "${!UNITS[@]}"; do
  unit="${UNITS[$index]}"
  attempted+=("$unit")
  echo "restarting $unit.service"
  if grep -Eq '^Type=oneshot([[:space:]]|$)' "$SCRIPT_DIR/$unit.service"; then
    if ! "$SYSTEMCTL" start "$unit.service" || ! health_check "$unit"; then
      echo "health check failed: $unit.service" >&2
      rollback_after_failure
      exit 1
    fi
  elif ! "$SYSTEMCTL" restart "$unit.service" || ! health_check "$unit"; then
    echo "health check failed: $unit.service" >&2
    rollback_after_failure
    exit 1
  fi
done
echo "immutable unit cutover complete"
