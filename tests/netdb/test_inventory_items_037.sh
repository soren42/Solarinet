#!/usr/bin/env bash
# Scratch-only migration and compensating rollback coverage for 037.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOR="$ROOT/netdb/sor"; DB="solarinet_i1_items_037"
M="$SOR/migrations/037_inventory_items_closeout.sql"; D="$SOR/rollback/037-down.sql"
if [[ "${SOLARI_TEST_SOR_DB:-}" == "1" ]]; then
  [[ "${MYSQL_HOST:-}" == "127.0.0.1" ]] || { echo "refusing: CI mode needs MYSQL_HOST=127.0.0.1" >&2; exit 2; }
  ADMIN=(mariadb --protocol=TCP --host=127.0.0.1 --port="${MYSQL_PORT:-3306}" --user="${MYSQL_USER:-root}")  # password: MYSQL_PWD env
else
  ADMIN=(sudo -n mariadb)
fi
ERR="$(mktemp)"; created=0
case "$DB" in sor|solarinet|solarinet_stage|sor_probe) echo "refusing live DB name $DB" >&2; exit 2 ;; esac
if "${ADMIN[@]}" -N -B -e "SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='$DB'" | /usr/bin/grep -q .; then echo "refusing to run: scratch database '$DB' already exists" >&2; exit 2; fi
cleanup() { if [[ "$created" == 1 ]]; then "${ADMIN[@]}" -e "DROP DATABASE IF EXISTS \`$DB\`" >/dev/null; fi; rm -f "$ERR"; }
trap cleanup EXIT
pass=0; ok() { pass=$((pass+1)); echo "ok - $*"; }
die() { echo "FAIL: $*" >&2; [[ -s "$ERR" ]] && cat "$ERR" >&2; exit 1; }
q() { "${ADMIN[@]}" -N -B --database="$DB" -e "$1"; }
apply() { "${ADMIN[@]}" --database="$DB" < "$1" || die "apply $(basename "$1")"; }
eq() { local label="$1" sql="$2" want="$3" got; got="$(q "$sql")"; [[ "$got" == "$want" ]] || die "$label expected '$want', got '$got'"; ok "$label"; }
refuse() { if "${ADMIN[@]}" --database="$DB" < "$1" >"$ERR" 2>&1; then die "expected refusal: $2"; fi; /usr/bin/grep -q "$2" "$ERR" || die "wrong refusal for $2"; ok "$2"; : >"$ERR"; }
[[ -f "$M" && -f "$D" ]] || die "037 migration or rollback missing"
"${ADMIN[@]}" -e "CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"; created=1
"${ADMIN[@]}" --database="$DB" -e "CREATE TABLE _solarinet_test_scratch(marker VARCHAR(64) NOT NULL); INSERT INTO _solarinet_test_scratch VALUES('solarinet-ci-scratch-v1')"
[[ "$(q "SELECT marker FROM _solarinet_test_scratch")" == solarinet-ci-scratch-v1 ]] || die "scratch sentinel mismatch"
apply "$SOR/schema.sql"
for migration in "$SOR"/migrations/*.sql; do base="$(basename "$migration")"; number="${base:0:3}"; if (( 10#$number < 37 )); then apply "$migration"; fi; done
ok "clean SoR migration history applied on marked scratch DB"
SRC="(SELECT id FROM sources WHERE slug='manual')"
# hardware_models' 033 trigger gives each legacy model its own product type.
q "INSERT INTO hardware_models(make,model,hw_type,source_id) VALUES('Acme','Box One','server',$SRC),('Acme','Box Two','server',$SRC)"
M1="$(q "SELECT id FROM hardware_models WHERE model='Box One'")"; M2="$(q "SELECT id FROM hardware_models WHERE model='Box Two'")"
T1="$(q "SELECT product_type_id FROM hardware_models WHERE id=$M1")"; T2="$(q "SELECT product_type_id FROM hardware_models WHERE id=$M2")"
q "INSERT INTO product_types(kind,name,source_id) VALUES('consumable','Cable ties',$SRC),('software','Office suite',$SRC)"
TC="$(q "SELECT id FROM product_types WHERE name='Cable ties'")"; TS="$(q "SELECT id FROM product_types WHERE name='Office suite'")"
q "INSERT INTO hardware_units(model_id,serial,source_id) VALUES($M1,'SER-1',$SRC)"; U1="$(q "SELECT id FROM hardware_units WHERE serial='SER-1'")"
eq "033 trigger derives the legacy unit product type" "SELECT product_type_id FROM hardware_units WHERE id=$U1" "$T1"
# Refusal fixtures are individually removed before the successful migration.
q "UPDATE hardware_units SET product_type_id=$T2 WHERE id=$U1"
refuse "$M" '037 preflight: hardware unit model and product type disagree'; q "UPDATE hardware_units SET product_type_id=$T1 WHERE id=$U1"
q "INSERT INTO locations(name,loc_type,code,source_id) VALUES('Site A','property','DUP',$SRC),('Site B','property','DUP',$SRC)"
refuse "$M" '037 preflight: duplicate live location codes DUP'
q "UPDATE locations SET deleted_at=NOW() WHERE name='Site B'"
q "INSERT INTO locations(name,loc_type,code,source_id) VALUES('Blank 1','property','',$SRC),('Blank 2','property','',$SRC)"
q "UPDATE hardware_units SET updated_at='2020-01-02 03:04:05' WHERE id=$U1; UPDATE locations SET updated_at='2020-01-02 03:04:05' WHERE name='Site A'"
apply "$M"; ok "037 clean apply after two preflight refusal checks (deleted duplicate and empty codes tolerated)"
eq "apply preserves unit updated_at" "SELECT updated_at FROM hardware_units WHERE id=$U1" "2020-01-02 03:04:05"
eq "apply preserves location lock value at microsecond precision" "SELECT updated_at FROM locations WHERE name='Site A'" "2020-01-02 03:04:05.000000"
eq "model_id is nullable" "SELECT IS_NULLABLE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_units' AND COLUMN_NAME='model_id'" YES
eq "lock columns are DATETIME(6)" "SELECT GROUP_CONCAT(CONCAT(TABLE_NAME,':',COLUMN_TYPE) ORDER BY TABLE_NAME) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND COLUMN_NAME='updated_at' AND TABLE_NAME IN ('locations','product_types','container_types')" "container_types:datetime(6),locations:datetime(6),product_types:datetime(6)"
# Product-type-only units.
q "INSERT INTO hardware_units(product_type_id,source_id) VALUES($TC,$SRC)"; UC="$(q "SELECT MAX(id) FROM hardware_units")"
eq "catalog-only product type creates a model-less unit" "SELECT CONCAT(IFNULL(model_id,'NULL'),':',product_type_id) FROM hardware_units WHERE id=$UC" "NULL:$TC"
q "INSERT INTO hardware_units(product_type_id,serial,source_id) VALUES($T2,'SER-2',$SRC)"; U2="$(q "SELECT id FROM hardware_units WHERE serial='SER-2'")"
eq "product type with a live legacy model fills model_id" "SELECT model_id FROM hardware_units WHERE id=$U2" "$M2"
q "INSERT INTO hardware_units(source_id) VALUES($SRC)" >"$ERR" 2>&1 && die "trigger accepted a unit without model or product type"
/usr/bin/grep -q 'product_type_id is required' "$ERR" || die "wrong missing-type result"; : >"$ERR"; ok "trigger requires a product type"
q "INSERT INTO hardware_units(model_id,product_type_id,source_id) VALUES($M1,$T2,$SRC)" >"$ERR" 2>&1 && die "trigger accepted mismatched model and type"
/usr/bin/grep -q 'model_id and product_type_id disagree' "$ERR" || die "wrong mismatch result"; : >"$ERR"; ok "trigger rejects mismatched model and product type on insert"
q "UPDATE hardware_units SET model_id=$M2, product_type_id=$TC WHERE id=$U1" >"$ERR" 2>&1 && die "trigger accepted mismatched update"
/usr/bin/grep -q 'model_id and product_type_id disagree' "$ERR" || die "wrong update mismatch result"; : >"$ERR"; ok "trigger rejects mismatched model and product type on update"
q "UPDATE hardware_units SET product_type_id=NULL, model_id=NULL WHERE id=$U1" >"$ERR" 2>&1 && die "trigger accepted clearing both"
/usr/bin/grep -q 'product_type_id is required' "$ERR" || die "wrong clear result"; : >"$ERR"; ok "trigger rejects clearing model and product type"
q "UPDATE hardware_units SET product_type_id=$TC WHERE id=$U2"
eq "catalog change follows the catalog (model cleared)" "SELECT CONCAT(IFNULL(model_id,'NULL'),':',product_type_id) FROM hardware_units WHERE id=$U2" "NULL:$TC"
q "UPDATE hardware_units SET model_id=$M2 WHERE id=$U2"
eq "model change follows the model" "SELECT CONCAT(model_id,':',product_type_id) FROM hardware_units WHERE id=$U2" "$M2:$T2"
q "INSERT INTO hardware_units(product_type_id,installed_on_unit_id,licence_seats,source_id) VALUES($TS,$U1,5,$SRC)"; US="$(q "SELECT MAX(id) FROM hardware_units")"
ok "software item installs on a hardware unit"
q "INSERT INTO hardware_units(product_type_id,installed_on_unit_id,source_id) VALUES($TS,$US,$SRC)" >"$ERR" 2>&1 && die "software installed on software"
/usr/bin/grep -q 'must reference a non-software unit' "$ERR" || die "wrong install target result"; : >"$ERR"; ok "installed_on target check retained"
q "INSERT INTO hardware_units(product_type_id,serial,source_id) VALUES($TC,'TIE-1',$SRC)"
q "INSERT INTO hardware_units(product_type_id,serial,source_id) VALUES($TC,'TIE-1',$SRC)" >"$ERR" 2>&1 && die "duplicate serial within a model-less product type"
/usr/bin/grep -q 'uq_hw_units_type_serial' "$ERR" || die "wrong serial duplicate result"; : >"$ERR"; ok "serial is unique within a product type"
# Location code uniqueness.
q "INSERT INTO locations(name,loc_type,code,source_id) VALUES('Site C','property','DUP',$SRC)" >"$ERR" 2>&1 && die "duplicate live code accepted"
/usr/bin/grep -q 'uq_locations_live_code' "$ERR" || die "wrong code duplicate result"; : >"$ERR"; ok "live location codes are globally unique"
q "INSERT INTO locations(name,loc_type,code,source_id) VALUES('Site D','property','dup',$SRC)" >"$ERR" 2>&1 && die "case-variant live code accepted"
: >"$ERR"; ok "location code uniqueness follows the case-insensitive collation"
q "INSERT INTO locations(name,loc_type,code,source_id) VALUES('Blank 3','property','',$SRC),('Null 1','property',NULL,$SRC),('Null 2','property',NULL,$SRC)"; ok "empty and NULL codes repeat freely"
q "UPDATE locations SET deleted_at=NOW() WHERE name='Site A'; INSERT INTO locations(name,loc_type,code,source_id) VALUES('Site E','property','DUP',$SRC)"; ok "a retired location releases its code"
# Two edits inside one second produce distinct lock tokens.
eq "same-second edits produce distinct lock tokens" "UPDATE product_types SET notes='a' WHERE id=$TC; SELECT updated_at INTO @a FROM product_types WHERE id=$TC; UPDATE product_types SET notes='b' WHERE id=$TC; SELECT updated_at INTO @b FROM product_types WHERE id=$TC; SELECT @a<>@b" 1
# Rollback.
refuse "$D" 'refusing rollback: hardware units without a legacy model_id exist'
q "DELETE FROM hardware_units WHERE model_id IS NULL"
apply "$D"
eq "rollback restores model_id NOT NULL" "SELECT IS_NULLABLE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_units' AND COLUMN_NAME='model_id'" NO
eq "rollback restores whole-second lock columns" "SELECT GROUP_CONCAT(COLUMN_TYPE ORDER BY TABLE_NAME) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND COLUMN_NAME='updated_at' AND TABLE_NAME IN ('locations','product_types','container_types')" "datetime,datetime,datetime"
eq "rollback removes 037 keys" "SELECT COUNT(*) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND INDEX_NAME IN ('uq_locations_live_code','uq_hw_units_type_serial')" 0
eq "rollback restores the 033 unit triggers" "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME IN ('trg_hw_units_installed_target_ins','trg_hw_units_installed_target_upd') AND ACTION_STATEMENT NOT LIKE '%product_type_id is required%'" 2
q "INSERT INTO hardware_units(model_id,serial,source_id) VALUES($M1,'SER-3',$SRC)"
eq "legacy model writes still derive the product type after rollback" "SELECT product_type_id FROM hardware_units WHERE serial='SER-3'" "$T1"
ok "rollback succeeds once model-less units are gone"
echo "PASS: $pass assertions"
