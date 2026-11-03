#!/usr/bin/env bash
# Exercise migration 034 and its compensating rollback on a disposable SoR.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOR="$ROOT/netdb/sor"
DB="sor_test_prov_os034"
M034="$SOR/migrations/034_prov_images_os.sql"
D034="$SOR/rollback/034-down.sql"
if [[ "${SOLARI_TEST_SOR_DB:-}" == "1" ]]; then
  [[ "${MYSQL_HOST:-}" == "127.0.0.1" ]] || { echo "refusing: CI mode needs MYSQL_HOST=127.0.0.1" >&2; exit 2; }
  ADMIN=(mariadb --protocol=TCP --host=127.0.0.1 --port="${MYSQL_PORT:-3306}" --user="${MYSQL_USER:-root}")  # password: MYSQL_PWD env
else
  ADMIN=(sudo -n mariadb)
fi
ERR="$(mktemp)"
created=0

case "$DB" in sor|solarinet|solarinet_stage|sor_probe) echo "refusing live DB name $DB" >&2; exit 2 ;; esac
if "${ADMIN[@]}" -N -B -e "SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='$DB'" | grep -q .; then
  echo "refusing to run: scratch database '$DB' already exists" >&2
  exit 2
fi
cleanup() {
  if [[ "$created" == 1 ]]; then "${ADMIN[@]}" -e "DROP DATABASE IF EXISTS \`$DB\`" >/dev/null; fi
  rm -f "$ERR"
}
trap cleanup EXIT

pass=0
ok() { pass=$((pass + 1)); echo "ok - $*"; }
die() { echo "FAIL: $*" >&2; exit 1; }
q() { "${ADMIN[@]}" -N -B --database="$DB" -e "$1"; }
apply() { "${ADMIN[@]}" --database="$DB" < "$1" || die "apply $(basename "$1")"; }
eq() {
  local label="$1" sql="$2" want="$3" got
  got="$(q "$sql")"
  [[ "$got" == "$want" ]] || die "$label: expected '$want', got '$got'"
  ok "$label"
}

[[ -f "$M034" && -f "$D034" ]] || die "034 migration or rollback missing"
"${ADMIN[@]}" -e "CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
created=1
apply "$SOR/schema.sql"
for migration in "$SOR"/migrations/*.sql; do
  base="$(basename "$migration")"
  number="${base:0:3}"
  if (( 10#$number < 34 )); then apply "$migration"; fi
done
ok "schema.sql and all available migrations before 034 applied to scratch"

# Existing catalogue entries cover direct release-name matching. Tumbleweed
# has an operator-facing label distinct from its lower-case pipeline handle.
SRC="(SELECT id FROM sources WHERE slug='manual')"
q "INSERT INTO operating_systems(family,name,version,source_id,asserted_kind) VALUES
  ('linux','Debian','12',$SRC,'human'),('linux','Ubuntu','24.04',$SRC,'human')"
apply "$M034"
ok "034 clean apply"

eq "every provisioner image has a catalogue link" \
  "SELECT COUNT(*) FROM prov_images WHERE os_id IS NULL" 0
eq "Debian release_name maps to existing Debian 12 catalogue row" \
  "SELECT COUNT(*) FROM prov_images i JOIN operating_systems os ON os.id=i.os_id WHERE i.distro='debian' AND i.release_name='12' AND os.name='Debian' AND os.version='12'" 1
eq "openSUSE Tumbleweed label maps to openSUSE/Tumbleweed" \
  "SELECT COUNT(*) FROM prov_images i JOIN operating_systems os ON os.id=i.os_id WHERE i.distro='opensuse' AND i.release_name='tumbleweed' AND i.release_label='Tumbleweed' AND os.name='openSUSE' AND os.version='Tumbleweed'" 1
eq "missing Raspberry Pi OS row inserted as human/manual Linux" \
  "SELECT COUNT(*) FROM operating_systems os JOIN sources s ON s.id=os.source_id WHERE os.name='Raspberry Pi OS' AND os.version='12' AND os.family='linux' AND os.asserted_kind='human' AND s.slug='manual'" 1
eq "Leap maps to openSUSE Leap/15.6, not bare openSUSE" \
  "SELECT COUNT(*) FROM prov_images i JOIN operating_systems os ON os.id=i.os_id WHERE i.release_name='15.6' AND os.name='openSUSE Leap' AND os.version='15.6'" 1
eq "image OS link is NOT NULL after backfill" \
  "SELECT IS_NULLABLE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='prov_images' AND COLUMN_NAME='os_id'" NO
eq "image OS FK uses RESTRICT" \
  "SELECT DELETE_RULE FROM information_schema.REFERENTIAL_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE() AND CONSTRAINT_NAME='fk_prov_images_os'" RESTRICT
ok "backfill count and schema assertions"

apply "$D034"
eq "rollback removes only the image OS link" \
  "SELECT (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='prov_images' AND COLUMN_NAME='os_id')+(SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE() AND CONSTRAINT_NAME='fk_prov_images_os')" 0
eq "rollback retains canonical OS catalogue rows" \
  "SELECT COUNT(*) FROM operating_systems WHERE name='Raspberry Pi OS' AND version='12'" 1
ok "rollback succeeds on migration fixture"
echo "PASS: $pass assertions"
