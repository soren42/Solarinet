#!/usr/bin/env bash
# Restore a backup created by cutover-immutable.sh and restart the prior units.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT=/
SYSTEMCTL=systemctl
DRY_RUN=0
BACKUP=
usage() { echo "usage: $0 BACKUP_DIR [--dry-run --root TEMP_ROOT] [--systemctl MOCK]" >&2; }
while (($#)); do
  case "$1" in
    --root) (($# >= 2)) || { usage; exit 2; }; ROOT="$2"; shift 2 ;;
    --systemctl) (($# >= 2)) || { usage; exit 2; }; SYSTEMCTL="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) if [[ -z "$BACKUP" ]]; then BACKUP="$1"; shift; else usage; exit 2; fi ;;
  esac
done
[[ -n "$BACKUP" && -f "$BACKUP/existed" && -f "$BACKUP/units.list" && -f "$BACKUP/active" ]] || { usage; exit 2; }
if [[ "$ROOT" != / && "$DRY_RUN" -ne 1 ]]; then echo "--root is restricted to --dry-run" >&2; exit 2; fi
if [[ "$DRY_RUN" -ne 1 && "$EUID" -ne 0 ]]; then echo "run rollback as root" >&2; exit 2; fi
UNIT_DIR="${ROOT%/}/etc/systemd/system"
mkdir -p "$UNIT_DIR"
while IFS= read -r name; do
  [[ -n "$name" ]] || continue
  if grep -Fxq "$name" "$BACKUP/existed"; then
    cp -a "$BACKUP/units/$name" "$UNIT_DIR/$name" || exit 1
  else
    rm -f "$UNIT_DIR/$name" || exit 1
  fi
done < "$BACKUP/units.list"
if [[ -f "$BACKUP/config-existed" ]]; then
  config="${ROOT%/}/home/jason/Code/Solarinet/deploy/alertbridge/alertbridge.conf"
  mkdir -p "$(dirname "$config")"
  cp -p "$BACKUP/config/alertbridge.conf" "$config" || exit 1
fi
"$SYSTEMCTL" daemon-reload || exit 1
while IFS= read -r name; do
  [[ -n "$name" ]] || continue
  unit="${name%.service}"
  prior="$BACKUP/units/$name"
  was_active="$(awk -F= -v service="$unit" '$1 == service {print $2}' "$BACKUP/active")"
  if [[ "$was_active" == 1 ]] && [[ -f "$prior" ]] && grep -Eq '^Type=oneshot([[:space:]]|$)' "$prior"; then
    "$SYSTEMCTL" start "$name" || { echo "start failed: $name" >&2; exit 1; }
    [[ "$("$SYSTEMCTL" show --property=Result --value "$name")" == success ]] || { echo "health check failed: $name" >&2; exit 1; }
    [[ "$("$SYSTEMCTL" show --property=ExecMainStatus --value "$name")" == 0 ]] || { echo "health check failed: $name" >&2; exit 1; }
  elif [[ "$was_active" == 1 ]] && grep -Fxq "$name" "$BACKUP/existed"; then
    "$SYSTEMCTL" restart "$name" || { echo "restart failed: $name" >&2; exit 1; }
    "$SYSTEMCTL" is-active --quiet "$name" || { echo "health check failed: $name" >&2; exit 1; }
  else
    "$SYSTEMCTL" stop "$name" || exit 1
  fi
done < "$BACKUP/units.list"
echo "immutable unit rollback complete"
