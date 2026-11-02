#!/usr/bin/env bash
# Exercise migration 033 and its compensating rollback on a disposable SoR only.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOR="$ROOT/netdb/sor"
DB="sor_test_inventory033"
M033="$SOR/migrations/033_inventory_catalog.sql"
D033="$SOR/rollback/033-down.sql"
ADMIN=(sudo -n mariadb)
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

[[ -f "$M033" && -f "$D033" ]] || die "033 migration or rollback missing"
"${ADMIN[@]}" -e "CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
created=1
apply "$SOR/schema.sql"
for migration in "$SOR"/migrations/*.sql; do
  base="$(basename "$migration")"
  number="${base:0:3}"
  if (( 10#$number < 33 )); then apply "$migration"; fi
done
ok "schema.sql and all available migrations before 033 applied to scratch"

# Fixture IDs include a sparse high ID to prove that later catalog IDs do not
# collide with the legacy model space. These are synthetic test records.
SRC="(SELECT id FROM sources WHERE slug='dashboard')"
q "INSERT INTO hardware_models(id,make,model,hw_type,notes,source_id) VALUES
  (50000,'Inventory033','Printer','printer','printer notes',$SRC),
  (50001,'Inventory033','Scanner','scanner',NULL,$SRC),
  (50002,'Inventory033','Other','other',NULL,$SRC),
  (50003,'Inventory033','Router','router',NULL,$SRC)"
q "INSERT INTO hardware_units(model_id,serial,source_id) VALUES
  (50000,'unit-printer',$SRC),(50001,'unit-scanner',$SRC),
  (50002,'unit-other',$SRC),(50003,'unit-router',$SRC)"
apply "$M033"
ok "033 clean apply"

eq "four source models backfilled one-to-one with preserved IDs" \
  "SELECT CONCAT(COUNT(*),':',COUNT(DISTINCT id),':',MIN(id),':',MAX(id)) FROM product_types WHERE id BETWEEN 50000 AND 50003" \
  "4:4:50000:50003"
eq "all fixture units linked through model_id mapping" \
  "SELECT COUNT(*) FROM hardware_units hu JOIN hardware_models hm ON hm.id=hu.model_id JOIN product_types pt ON pt.id=hu.product_type_id WHERE hm.id BETWEEN 50000 AND 50003 AND pt.id=hm.id" 4
eq "deterministic kind mapping" \
  "SELECT GROUP_CONCAT(CONCAT(id,':',kind) ORDER BY id) FROM product_types WHERE id BETWEEN 50000 AND 50003" \
  "50000:peripheral,50001:peripheral,50002:other,50003:hardware"
eq "nullable brand/model and notes copy" \
  "SELECT CONCAT(brand,':',model,':',notes) FROM product_types WHERE id=50000" \
  "Inventory033:Printer:printer notes"
eq "nullable brand/model remain supported" \
  "SELECT COUNT(*) FROM product_types WHERE id BETWEEN 50000 AND 50003 AND brand IS NOT NULL AND model IS NOT NULL" 4
q "INSERT INTO product_types(kind,name,source_id,asserted_kind) VALUES('component','Inventory033 model-less',$SRC,'human')"
eq "product types permit NULL brand and model" \
  "SELECT COUNT(*) FROM product_types WHERE name='Inventory033 model-less' AND brand IS NULL AND model IS NULL" 1
q "DELETE FROM product_types WHERE name='Inventory033 model-less'"
eq "commercial barcode table remains empty" "SELECT COUNT(*) FROM product_barcodes" 0
eq "new auto-increment is above preserved model IDs" \
  "SELECT AUTO_INCREMENT>50003 FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='product_types'" 1
eq "all model mappings are unique" \
  "SELECT COUNT(*) FROM (SELECT product_type_id FROM hardware_models WHERE id BETWEEN 50000 AND 50003 GROUP BY product_type_id HAVING COUNT(*)>1) duplicates" 0
eq "installed-on FK uses SET NULL" \
  "SELECT DELETE_RULE FROM information_schema.REFERENTIAL_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE() AND CONSTRAINT_NAME='fk_hw_units_installed_on_unit'" SET\ NULL
eq "system barcode subject metadata includes product types" \
  "SELECT COLUMN_COMMENT LIKE '%product_types%' FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='barcodes' AND COLUMN_NAME='subject_table'" 1
ok "backfill count and schema assertions"

# Prove D1/D2 target validation works: a hardware target is accepted, while a
# software target and self-reference are rejected. All rows are test fixtures.
SOFT="$(q "INSERT INTO product_types(kind,name,source_id,asserted_kind) VALUES('software','Inventory033 licence',$SRC,'human'); SELECT LAST_INSERT_ID()")"
SRC_UNIT="$(q "SELECT id FROM hardware_units WHERE model_id=50000")"
TARGET_UNIT="$(q "SELECT id FROM hardware_units WHERE model_id=50003")"
SOFT_UNIT="$(q "INSERT INTO hardware_units(model_id,product_type_id,serial,source_id) VALUES(50003,$SOFT,'unit-software',$SRC); SELECT LAST_INSERT_ID()")"
q "UPDATE hardware_units SET installed_on_unit_id=$TARGET_UNIT WHERE id=$SOFT_UNIT"
eq "software unit accepts a non-software install target" \
  "SELECT installed_on_unit_id=$TARGET_UNIT FROM hardware_units WHERE id=$SOFT_UNIT" 1
if "${ADMIN[@]}" --database="$DB" -e "UPDATE product_types SET kind='software' WHERE id=50003" >/dev/null 2>"$ERR"; then
  die "catalog update unexpectedly changed an active installation target to software"
fi
ok "catalog update cannot invalidate an active hardware target"
if "${ADMIN[@]}" --database="$DB" -e "UPDATE hardware_units SET installed_on_unit_id=$SOFT_UNIT WHERE id=$SOFT_UNIT" >/dev/null 2>"$ERR"; then
  die "self-installation unexpectedly accepted"
fi
ok "installed-on guard rejects self-reference"
if "${ADMIN[@]}" --database="$DB" -e "UPDATE hardware_units SET installed_on_unit_id=$SOFT_UNIT WHERE id=$SRC_UNIT" >/dev/null 2>"$ERR"; then
  die "software installation target unexpectedly accepted"
fi
ok "installed-on guard rejects a software target"
q "UPDATE hardware_units SET installed_on_unit_id=NULL WHERE id=$SOFT_UNIT"
q "DELETE FROM hardware_units WHERE id=$SOFT_UNIT"
q "DELETE FROM product_types WHERE id=$SOFT"

# Refusal must leave all migration DDL intact. The test adds then removes one
# post-migration product barcode inside this scratch DB only.
q "INSERT INTO product_barcodes(product_type_id,code,code_type,source_id,asserted_kind) VALUES(50000,'033-ROLLBACK-REFUSAL','sku',$SRC,'human')"
if "${ADMIN[@]}" --database="$DB" < "$D033" >/dev/null 2>"$ERR"; then
  die "rollback unexpectedly accepted post-migration product barcode data"
fi
grep -q "product_barcodes contains post-migration data" "$ERR" || { cat "$ERR" >&2; die "rollback refused for an unexpected reason"; }
eq "refused rollback leaves catalog tables and columns intact" \
  "SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('product_types','product_barcodes'))+(SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_units' AND COLUMN_NAME='licence_reference')" 3
q "DELETE FROM product_barcodes WHERE code='033-ROLLBACK-REFUSAL'"
q "DROP PROCEDURE IF EXISTS _solarinet_rollback_m033_assert_reversible"
ok "rollback refusal on post-migration data"

q "UPDATE hardware_units SET licence_reference='fixture://inventory-033' WHERE model_id=50000"
if "${ADMIN[@]}" --database="$DB" < "$D033" >/dev/null 2>"$ERR"; then
  die "rollback unexpectedly accepted populated licence-reference data"
fi
grep -q "post-migration licence or installation data" "$ERR" || { cat "$ERR" >&2; die "licence rollback refused for an unexpected reason"; }
q "UPDATE hardware_units SET licence_reference=NULL WHERE model_id=50000"
q "DROP PROCEDURE IF EXISTS _solarinet_rollback_m033_assert_reversible"
ok "rollback refusal on populated licence fields"

apply "$D033"
eq "rollback removes catalog tables" \
  "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('product_types','product_barcodes')" 0
eq "rollback removes added unit columns" \
  "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_units' AND COLUMN_NAME IN ('product_type_id','licence_seats','licence_expires_on','licence_reference','installed_on_unit_id')" 0
eq "rollback removes model column and restores barcode comment" \
  "SELECT (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_models' AND COLUMN_NAME='product_type_id')+(SELECT COLUMN_COMMENT LIKE '%product_types%' FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='barcodes' AND COLUMN_NAME='subject_table')" 0
ok "rollback succeeds on untouched migration fixture"
echo "PASS: $pass assertions"
