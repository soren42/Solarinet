#!/usr/bin/env bash
# Exercise migration 035 (catalog display names, edit-preserving legacy sync)
# and its compensating rollback on a disposable SoR.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOR="$ROOT/netdb/sor"
DB="sor_test_catalog_names035"
M035="$SOR/migrations/035_catalog_display_names.sql"
D035="$SOR/rollback/035-down.sql"
if [[ "${SOLARI_TEST_SOR_DB:-}" == "1" ]]; then
  [[ "${MYSQL_HOST:-}" == "127.0.0.1" ]] || { echo "refusing: CI mode needs MYSQL_HOST=127.0.0.1" >&2; exit 2; }
  ADMIN=(mariadb --protocol=TCP --host=127.0.0.1 --port="${MYSQL_PORT:-3306}" --user="${MYSQL_USER:-root}")  # password: MYSQL_PWD env
else
  ADMIN=(sudo -n mariadb)
fi
created=0

case "$DB" in sor|solarinet|solarinet_stage|sor_probe) echo "refusing live DB name $DB" >&2; exit 2 ;; esac
if "${ADMIN[@]}" -N -B -e "SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='$DB'" | grep -q .; then
  echo "refusing to run: scratch database '$DB' already exists" >&2
  exit 2
fi
cleanup() { if [[ "$created" == 1 ]]; then "${ADMIN[@]}" -e "DROP DATABASE IF EXISTS \`$DB\`" >/dev/null; fi; }
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
name_of() { q "SELECT pt.name FROM product_types pt JOIN hardware_models hm ON hm.product_type_id=pt.id WHERE hm.id=$1"; }

[[ -f "$M035" && -f "$D035" ]] || die "035 migration or rollback missing"
"${ADMIN[@]}" -e "CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
created=1
apply "$SOR/schema.sql"
SRC="(SELECT id FROM sources WHERE slug='manual')"
for migration in "$SOR"/migrations/*.sql; do
  base="$(basename "$migration")"; number="${base:0:3}"
  if (( 10#$number < 33 )); then apply "$migration"; fi
done
# Legacy models present before 033 backfill, covering each naming rule.
q "INSERT INTO hardware_models(id,make,model,hw_type,source_id,asserted_kind) VALUES
  (1,'Raspberry Pi','400','sbc',$SRC,'human'),
  (2,'Eufy','Eufybase','other',$SRC,'human'),
  (3,'Google Nest','Nest','other',$SRC,'human'),
  (4,'','ILI9481','other',$SRC,'human'),
  (5,'ArkKVM','ArkKVM','other',$SRC,'human')"
for migration in "$SOR"/migrations/*.sql; do
  base="$(basename "$migration")"; number="${base:0:3}"
  if (( 10#$number >= 33 && 10#$number < 35 )); then apply "$migration"; fi
done
q "UPDATE product_types SET name='Operator choice' WHERE id=5"
ok "migrations through 034 applied with legacy fixtures"

apply "$M035"
ok "035 clean apply"
eq "brand + model"               "SELECT name FROM product_types WHERE id=1" "Raspberry Pi 400"
eq "model already carries brand" "SELECT name FROM product_types WHERE id=2" "Eufybase"
eq "brand already ends in model" "SELECT name FROM product_types WHERE id=3" "Google Nest"
eq "empty brand keeps model"        "SELECT name FROM product_types WHERE id=4" "ILI9481"
eq "operator-edited name untouched by backfill" "SELECT name FROM product_types WHERE id=5" "Operator choice"

q "INSERT INTO hardware_models(make,model,hw_type,source_id,asserted_kind) VALUES('Khadas','VIM4','sbc',$SRC,'human')"
eq "insert trigger derives display name" "SELECT pt.name FROM product_types pt JOIN hardware_models hm ON hm.product_type_id=pt.id WHERE hm.model='VIM4'" "Khadas VIM4"

q "UPDATE hardware_models SET model='500' WHERE id=1"
eq "legacy model edit follows an untouched name" "SELECT CONCAT(name,'|',model) FROM product_types WHERE id=1" "Raspberry Pi 500|500"

q "UPDATE product_types SET name='Keyboard computer', kind='component' WHERE id=1"
q "UPDATE hardware_models SET notes='seeded', hw_type='other' WHERE id=1"
eq "legacy edit keeps operator name and kind" "SELECT CONCAT(name,'|',kind,'|',notes) FROM product_types WHERE id=1" "Keyboard computer|component|seeded"

q "UPDATE product_types SET notes='operator notes' WHERE id=2"
q "UPDATE hardware_models SET notes='seed notes', make='Anker Eufy' WHERE id=2"
eq "legacy edit keeps operator notes, follows untouched brand" "SELECT CONCAT(brand,'|',notes,'|',name) FROM product_types WHERE id=2" "Anker Eufy|operator notes|Anker Eufy Eufybase"

q "INSERT INTO hardware_models(id,make,model,hw_type,source_id,asserted_kind) VALUES(7,'Cafe','CaféBook','other',$SRC,'human')"
eq "prefix test is byte-exact (accents differ)" "SELECT pt.name FROM product_types pt JOIN hardware_models hm ON hm.product_type_id=pt.id WHERE hm.id=7" "Cafe CaféBook"
q "UPDATE product_types pt JOIN hardware_models hm ON hm.product_type_id=pt.id SET pt.name='cafe caféBook' WHERE hm.id=7"
q "UPDATE hardware_models SET model='CaféBook 2' WHERE id=7"
eq "case-only operator rename survives a legacy edit" "SELECT pt.name FROM product_types pt JOIN hardware_models hm ON hm.product_type_id=pt.id WHERE hm.id=7" "cafe caféBook"

q "UPDATE hardware_models SET deleted_at=NOW() WHERE id=4"
eq "legacy soft-delete still propagates" "SELECT deleted_at IS NOT NULL FROM product_types WHERE id=4" 1

apply "$D035"
eq "rollback restores bare model on untouched derived names" "SELECT name FROM product_types WHERE id=3" "Nest"
eq "rollback leaves operator names" "SELECT name FROM product_types WHERE id=1" "Keyboard computer"
q "UPDATE hardware_models SET notes='after rollback' WHERE id=1"
eq "rollback restores the 033 unconditional sync" "SELECT CONCAT(name,'|',notes) FROM product_types WHERE id=1" "500|after rollback"

apply "$M035"
eq "035 reapplies after rollback" "SELECT name FROM product_types WHERE id=3" "Google Nest"
echo "PASS: $pass assertions"
