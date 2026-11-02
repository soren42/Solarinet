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
# The tree is streamed as a tar into ${PROV_ROOT}/ansible.new, then swapped in
# with two renames; the previous tree is kept as ansible.prev. A playbook that is
# already running keeps its cwd in the old tree. benzene's own inventory/ is
# preserved (the release only carries an example).
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
  -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
tarball() { tar -C "${SRC}" --exclude=inventory --exclude='*.retry' -cf - .; }

if [ "${CHECK}" -eq 1 ]; then
  # shellcheck disable=SC2016
  body='set -e; t="$(mktemp -d)"; trap "rm -rf \"$t\"" EXIT; tar -C "$t" -xf -
diff -rq --exclude=inventory --exclude=.release "$t" "$1" && echo "in sync: $1"'
  flog "comparing ${SRC} with ${BENZENE}:${DST}"
  tarball | bssh "sh -c $(fleet_sq "$body") sync-ansible $(fleet_sq "$DST")" || true
  exit 0
fi

# shellcheck disable=SC2016
body='set -e; dst="$1"; new="$dst.new"; rev="$2"
rm -rf "$new"; mkdir -p "$new"; tar -C "$new" -xf -
[ -f "$new/ansible.cfg" ] || { echo "sync-ansible: empty tree" >&2; rm -rf "$new"; exit 1; }
if [ -d "$dst/inventory" ]; then cp -a "$dst/inventory" "$new/"; fi
printf "%s\n" "$rev" > "$new/.release"
rm -rf "$dst.prev"
if [ -d "$dst" ]; then mv "$dst" "$dst.prev"; fi
mv "$new" "$dst"'
flog "publishing ${SRC} (${REV}) to ${BENZENE}:${DST}"
tarball | bssh "sh -c $(fleet_sq "$body") sync-ansible $(fleet_sq "$DST") $(fleet_sq "$REV")" \
  || fdie "Ansible tree sync to ${BENZENE} failed; ${DST} unchanged unless the final rename ran"
flog "done; previous tree kept at ${DST}.prev"
