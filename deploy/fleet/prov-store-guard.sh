# shellcheck shell=sh
# =============================================================================
# prov-store-guard.sh - the provisioning image store check, shared by every
# helper that reads or writes big netboot payloads (installers/, images/).
# A function library (POSIX sh, also valid bash): callers ship its TEXT to the
# provisioning host and append one call, e.g.
#
#   sh -c "$(cat prov-store-guard.sh)
#   prov_store \"\$@\"" prov-store check /srv/solari-provision/store cifs \
#       /srv/solari-provision/netboot/http installers images
#
# so the check runs where the store is mounted (benzene), or locally under the
# SOLARI_PROV_LOCAL=1 test seam. Used by fetch-images.sh (inside its fixed
# remote script), fleet-lib.sh (fleet_require_store) and provlib.py
# (require_store / store_status).
#
#   prov_store check  <store> <fstype> <http_root> <sub>...
#   prov_store status <store> <fstype> <http_root> <sub>...
#
# The store is a NAS share (CIFS, systemd automount) at <store>; the HTTP root
# serves it through whole-directory symlinks <http_root>/<sub> -> <store>/<sub>
# (see deploy/host-configs/benzene/README.md). `check` FAILS CLOSED (exit 1,
# one-line reason on stderr) unless:
#   1. <store> is a mounted filesystem of type <fstype> (findmnt), after
#      touching it to trigger the automount — an unmounted mountpoint is just
#      a directory on the local disk, and writing there would silently fill
#      benzene's root filesystem;
#   2. every <sub> under <http_root> resolves (readlink -f) to <store>/<sub>,
#      which is a writable directory;
#   3. (check only) the store maintenance lock .prov-store-migrate.lock in the
#      directory holding <http_root> (local disk) can be taken SHARED within PROV_STORE_LOCK_WAIT seconds (default 10):
#      prov-store-migrate.py holds it exclusively from before planning through
#      the swap, so no refresh, stage or image build runs during a migration.
# <fstype> "none" skips (1) and is for the test seam only; the callers refuse
# it outside SOLARI_PROV_LOCAL=1.
#
# Every filesystem touch on the store is bounded by timeout(1) (PROV_STORE_TIMEOUT
# seconds, default 20): the share is mounted `hard`, so a hung NAS would
# otherwise block the caller forever. Without timeout(1) the check FAILS CLOSED,
# and a touch that times out is "store not responding", never a pass.
#
# On success `check` leaves, in the CALLING shell:
#   PROV_STORE_REAL  the resolved store path (readlink -f). Writers build their
#                    destination from it (PROV_STORE_REAL/<sub>/...), never
#                    through <http_root>/<sub>, so a link swapped for a real
#                    directory after the check cannot redirect a write onto the
#                    local disk;
#   fd 7             open on the maintenance lock, held shared until the calling
#                    shell exits. Run `prov_store check` in the shell that does
#                    the work (not in a $(...) subshell), or the lock is dropped.
#
# `status` never fails; it prints key=value lines for the dashboard:
#   store= fstype= mounted=yes|no source= size_kb= avail_kb= linked=yes|no
#   ok=yes|no reason=
# =============================================================================

_ps_bounded() {
  timeout -k 5 "${PROV_STORE_TIMEOUT:-20}" "$@"
}

prov_store() {
  _ps_mode=$1 _ps_store=$2 _ps_fs=$3 _ps_root=$4
  shift 4
  _ps_mounted=no _ps_src="" _ps_linked=yes _ps_reason="" _ps_real=""
  PROV_STORE_REAL=""
  if ! command -v timeout >/dev/null 2>&1; then
    _ps_reason="timeout(1) is not installed: the image store check cannot be bounded (a hung NAS mount would block forever); refusing"
  elif [ "$_ps_mode" = check ]; then
    _ps_lock_shared "${_ps_root%/*}/.prov-store-migrate.lock" || true
  fi
  if [ -z "$_ps_reason" ]; then
    _ps_rc=0
    _ps_bounded ls -- "$_ps_store/" >/dev/null 2>&1 || _ps_rc=$?   # trigger the automount
    case "$_ps_rc" in
      124|137) _ps_reason="provisioning image store not responding: touching $_ps_store timed out after ${PROV_STORE_TIMEOUT:-20}s (NAS hung or unreachable); refusing" ;;
    esac
  fi
  if [ -n "$_ps_reason" ]; then
    :
  elif [ "$_ps_fs" = none ]; then
    _ps_bounded test -d "$_ps_store" && _ps_mounted=yes
  else
    _ps_src=$(_ps_bounded findmnt -n -o SOURCE -t "$_ps_fs" -M "$_ps_store" 2>/dev/null) || _ps_src=""
    _ps_src=$(printf '%s\n' "$_ps_src" | sed -n 1p)
    [ -n "$_ps_src" ] && _ps_mounted=yes
  fi
  if [ "$_ps_mounted" = yes ]; then
    _ps_real=$(_ps_bounded readlink -f -- "$_ps_store" 2>/dev/null) || _ps_real=""
    [ -n "$_ps_real" ] || { _ps_mounted=no; _ps_reason="cannot resolve $_ps_store"; }
  elif [ -z "$_ps_reason" ]; then
    _ps_reason="provisioning image store not mounted: $_ps_store is not a $_ps_fs mount (NAS down, share or credentials missing, or solari-prov-store-mount not up); refusing to read or write images on the local disk underneath"
  fi
  if [ "$_ps_mounted" = yes ]; then
    for _ps_sub in "$@"; do
      _ps_got=$(_ps_bounded readlink -f -- "$_ps_root/$_ps_sub" 2>/dev/null) || _ps_got=""
      if [ "$_ps_got" != "$_ps_real/$_ps_sub" ] || ! _ps_bounded test -d "$_ps_got"; then
        _ps_linked=no
        _ps_reason="$_ps_root/$_ps_sub does not resolve into the image store ($_ps_store/$_ps_sub); run deploy/fleet/prov-store-migrate.py"
        break
      fi
      if [ "$_ps_mode" = check ] && ! _ps_bounded test -w "$_ps_got"; then
        _ps_linked=no
        _ps_reason="image store directory $_ps_store/$_ps_sub is not writable"
        break
      fi
    done
  else
    _ps_linked=no
  fi
  if [ "$_ps_mode" = status ]; then
    _ps_size="" _ps_avail=""
    if [ "$_ps_mounted" = yes ]; then
      # counted from the right: the share name (column 1) may contain spaces
      _ps_df=$(_ps_bounded df -P -k -- "$_ps_store" 2>/dev/null | awk 'NR == 2 { print $(NF-4), $(NF-2) }') || _ps_df=""
      _ps_size=${_ps_df% *} _ps_avail=${_ps_df#* }
      case "$_ps_size$_ps_avail" in *[!0-9]*) _ps_size="" _ps_avail="" ;; esac
    fi
    _ps_ok=no; [ -z "$_ps_reason" ] && _ps_ok=yes
    printf 'store=%s\nfstype=%s\nmounted=%s\nsource=%s\nsize_kb=%s\navail_kb=%s\nlinked=%s\nok=%s\nreason=%s\n' \
      "$_ps_store" "$_ps_fs" "$_ps_mounted" "$_ps_src" "$_ps_size" "$_ps_avail" "$_ps_linked" "$_ps_ok" "$_ps_reason"
    return 0
  fi
  [ "$_ps_mode" = check ] || { printf '[store][error] bad mode %s\n' "$_ps_mode" >&2; return 2; }
  if [ -n "$_ps_reason" ]; then
    printf '[store][error] %s\n' "$_ps_reason" >&2
    return 1
  fi
  # shellcheck disable=SC2034  # output for the caller (see the header)
  PROV_STORE_REAL=$_ps_real
  return 0
}

# _ps_lock_shared <lockfile>: open fd 7 on the store maintenance lock and take
# it shared (bounded wait). Sets _ps_reason on failure. The lock file lives on
# the local disk beside the web root, never on the store, so this cannot hang.
_ps_lock_shared() {
  [ "${_ps_locked:-}" = "$1" ] && return 0           # already held by this shell
  if ! command -v flock >/dev/null 2>&1; then
    _ps_reason="flock(1) is not installed: cannot honour the image store maintenance lock $1; refusing"
    return 1
  fi
  [ -e "$1" ] || : >>"$1" 2>/dev/null || true
  if [ ! -f "$1" ] || [ ! -r "$1" ]; then
    _ps_reason="cannot open the image store maintenance lock $1; refusing"
    return 1
  fi
  exec 7<"$1"
  if ! flock -s -w "${PROV_STORE_LOCK_WAIT:-10}" 7; then
    exec 7<&-
    _ps_reason="image store maintenance in progress: prov-store-migrate.py holds $1; retry when it has finished"
    return 1
  fi
  _ps_locked=$1
  return 0
}
