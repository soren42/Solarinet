#!/usr/bin/env bash
# =============================================================================
# sync-ansible.sh - publish a release's Ansible tree to the provisioning host.
# =============================================================================
# stack-apply.py and prov-finish.py run playbooks from ${PROV_ROOT}/ansible on
# benzene, not from the release on xenon, so every release that changes
# deploy/fleet/ansible must be pushed there. Run it after install-release.py:
#
#   /opt/solarinet/current/deploy/fleet/sync-ansible.sh            # publish
#   /opt/solarinet/current/deploy/fleet/sync-ansible.sh --check    # diff only
#
# Each publish extracts into its own directory under ${PROV_ROOT}/ansible.releases
# and ${PROV_ROOT}/ansible becomes a symlink to it, switched with one atomic
# rename. Runners `env -C` into that path, so a playbook already running keeps
# resolving files in the tree it started with; the newest KEEP trees are kept
# and ansible.prev points at the previous one (rollback: repoint the symlink).
# benzene's own inventory/ is carried forward (the release only has an example).
# --check exits 0 when in sync, 1 on drift, 2 when the comparison itself failed.
# =============================================================================
set -euo pipefail

FLEET_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=deploy/fleet/fleet-lib.sh
. "${FLEET_DIR}/fleet-lib.sh"

SRC="${FLEET_DIR}/ansible"
CHECK=0
case "${1:-}" in
  "") ;;
  --check) CHECK=1 ;;
  -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) fdie "unknown option: $1 (try --help)" ;;
esac
[ -f "${SRC}/ansible.cfg" ] && [ -f "${SRC}/playbooks/site.yml" ] \
  || fdie "no Ansible tree at ${SRC}"
[[ "$PROV_ROOT" =~ ^/[A-Za-z0-9._/-]+$ ]] && [[ "/$PROV_ROOT/" != */../* ]] \
  || fdie "unsafe PROV_ROOT: $PROV_ROOT"

# Release identity (when run from an installed release) for the marker file.
REV="unknown"
if [ -f "${FLEET_DIR}/../../RELEASE.json" ]; then
  REV="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d.get("version") or "unknown")' \
        "${FLEET_DIR}/../../RELEASE.json" 2>/dev/null || echo unknown)"
fi
[[ "$REV" =~ ^[A-Za-z0-9._-]+$ ]] || REV="unknown"

DST="${PROV_ROOT}/ansible"
KEEP=3
tarball() { tar -C "${SRC}" --exclude=inventory --exclude='*.retry' -cf - .; }

if [ "${CHECK}" -eq 1 ]; then
  # Remote exit 10 = drift; anything else nonzero = the check itself failed.
  # shellcheck disable=SC2016
  body='t="$(mktemp -d)" || exit 2; trap "rm -rf \"$t\"" EXIT
tar -C "$t" -xf - || exit 2
if [ ! -e "$1" ]; then echo "not published yet: $1"; exit 10; fi
diff -rq --exclude=inventory --exclude=.release "$t" "$1/"; rc=$?
[ "$rc" -eq 0 ] && echo "in sync: $1"
[ "$rc" -eq 1 ] && exit 10
exit "$rc"'
  flog "comparing ${SRC} with ${BENZENE}:${DST}"
  rc=0
  tarball | bssh "sh -c $(fleet_sq "$body") sync-ansible $(fleet_sq "$DST")" || rc=$?
  case "$rc" in
    0) exit 0 ;;
    10) flog "drift: ${BENZENE}:${DST} differs from this release"; exit 1 ;;
    *) fwarn "comparison with ${BENZENE}:${DST} failed (exit ${rc})"; exit 2 ;;
  esac
fi

# Remote side. A pre-symlink install (a real directory) is converted once with
# mv --exchange, so ${dst} never disappears; without it, refuse rather than
# open a gap.
# shellcheck disable=SC2016
body='set -eu; dst="$1"; rev="$2"; keep="$3"; rels="$dst.releases"
mkdir -p "$rels"
new="$rels/$(date -u +%Y%m%dT%H%M%SZ)-$rev"
[ ! -e "$new" ] || new="$new-$$"
mkdir "$new"
if ! tar -C "$new" -xf - || [ ! -f "$new/ansible.cfg" ]; then
  echo "sync-ansible: tree did not extract" >&2; rm -rf "$new"; exit 1
fi
if [ -d "$dst/inventory" ]; then cp -a "$dst/inventory" "$new/"; fi
printf "%s\n" "$rev" > "$new/.release"
# An ansible.prev left as a real directory by the old two-rename scheme would
# block the symlink below; file it with the other trees (pruned normally).
if [ -d "$dst.prev" ] && [ ! -L "$dst.prev" ]; then
  mv -T "$dst.prev" "$rels/$(date -u +%Y%m%dT%H%M%SZ)-oldprev"
fi
rm -f "$dst.swap"; ln -s "$new" "$dst.swap"
if [ -d "$dst" ] && [ ! -L "$dst" ]; then
  if pgrep -f '[a]nsible-playbook' >/dev/null 2>&1; then
    rm -f "$dst.swap"; rm -rf "$new"
    echo "sync-ansible: a playbook is running; retry the one-time conversion when idle" >&2; exit 1
  fi
  if ! mv --exchange -T "$dst.swap" "$dst" 2>/dev/null; then
    rm -f "$dst.swap"; rm -rf "$new"
    echo "sync-ansible: $dst is a directory and mv --exchange is unavailable; convert it by hand" >&2
    exit 1
  fi
  prev="$rels/$(date -u +%Y%m%dT%H%M%SZ)-legacy"
  mv -T "$dst.swap" "$prev"
else
  prev="$(readlink "$dst" 2>/dev/null || true)"
  mv -T "$dst.swap" "$dst"
fi
if [ -n "$prev" ]; then ln -sfn "$prev" "$dst.swap" && mv -T "$dst.swap" "$dst.prev"; fi
# Prune all but the newest $keep trees, never the live or previous one.
ls -1d "$rels"/*/ 2>/dev/null | sed "s:/\$::" | sort -r | tail -n +"$((keep + 1))" |
while IFS= read -r old; do
  [ "$old" = "$new" ] || [ "$old" = "$prev" ] || rm -rf "$old"
done'
flog "publishing ${SRC} (${REV}) to ${BENZENE}:${DST}"
tarball | bssh "sh -c $(fleet_sq "$body") sync-ansible $(fleet_sq "$DST") $(fleet_sq "$REV") $(fleet_sq "$KEEP")" \
  || fdie "Ansible tree sync to ${BENZENE} failed (see above); ${DST} is switched only by the final rename"
flog "done; ${DST} -> ${DST}.releases (previous at ${DST}.prev)"
