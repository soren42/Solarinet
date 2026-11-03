#!/usr/bin/env bash
# Scratch-only migration and compensating rollback coverage for 036.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOR="$ROOT/netdb/sor"; DB="solarinet_wp3_locations_036"
M="$SOR/migrations/036_inventory_locations.sql"; D="$SOR/rollback/036-down.sql"
ADMIN=(sudo -n mariadb); ERR="$(mktemp)"; created=0
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
[[ -f "$M" && -f "$D" ]] || die "036 migration or rollback missing"
"${ADMIN[@]}" -e "CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"; created=1
"${ADMIN[@]}" --database="$DB" -e "CREATE TABLE _solarinet_test_scratch(marker VARCHAR(64) NOT NULL); INSERT INTO _solarinet_test_scratch VALUES('solarinet-ci-scratch-v1')"
[[ "$(q "SELECT marker FROM _solarinet_test_scratch")" == solarinet-ci-scratch-v1 ]] || die "scratch sentinel mismatch"
apply "$SOR/schema.sql"
for migration in "$SOR"/migrations/*.sql; do base="$(basename "$migration")"; number="${base:0:3}"; if (( 10#$number < 36 )); then apply "$migration"; fi; done
ok "clean SoR migration history applied on marked scratch DB"
SRC="(SELECT id FROM sources WHERE slug='manual')"
q "INSERT INTO locations(id,name,loc_type,source_id,asserted_kind,asserted_at,deleted_at) VALUES (9103,'Painted Fern','site',$SRC,'human',NOW(),NULL),(9104,'Painted Fern','site',$SRC,'human',NOW(),NOW())"
q "INSERT INTO locations(name,loc_type,source_id,asserted_kind,asserted_at) VALUES ('Building','building',$SRC,'human',NOW()),('Floor','floor',$SRC,'human',NOW()),('Room','room',$SRC,'human',NOW()),('Rack','rack',$SRC,'human',NOW()),('Bag','container',$SRC,'human',NOW())"
SITE="$(q "SELECT id FROM locations WHERE name='Painted Fern' AND deleted_at IS NULL")"; DELETED="$(q "SELECT id FROM locations WHERE name='Painted Fern' AND deleted_at IS NOT NULL")"; BUILD="$(q "SELECT id FROM locations WHERE name='Building'")"; FLOOR="$(q "SELECT id FROM locations WHERE name='Floor'")"; ROOM="$(q "SELECT id FROM locations WHERE name='Room'")"; RACK="$(q "SELECT id FROM locations WHERE name='Rack'")"; BAG="$(q "SELECT id FROM locations WHERE name='Bag'")"
q "UPDATE locations SET parent_id=NULL WHERE id=$BUILD; UPDATE locations SET parent_id=$BUILD WHERE id=$FLOOR; UPDATE locations SET parent_id=$FLOOR WHERE id=$ROOM; UPDATE locations SET parent_id=$SITE WHERE id=$RACK; UPDATE locations SET parent_id=$RACK WHERE id=$BAG"
# Refusal fixtures are individually removed before the successful migration.
q "INSERT INTO locations(name,loc_type,source_id,asserted_kind,asserted_at) VALUES('Painted Fern','site',$SRC,'human',NOW())"
refuse "$M" '036 preflight: live sibling duplicates'
q "DELETE FROM locations WHERE name='Painted Fern' AND id<>$SITE AND deleted_at IS NULL"
q "INSERT INTO locations(name,loc_type,parent_id,source_id,asserted_kind,asserted_at) VALUES('Bad child','room',$SITE,$SRC,'human',NOW())"; BAD="$(q "SELECT id FROM locations WHERE name='Bad child'")"
refuse "$M" '036 preflight: invalid location parent/level transition'; q "DELETE FROM locations WHERE id=$BAD"
q "UPDATE locations SET parent_id=$BAG WHERE id=$SITE"
refuse "$M" '036 preflight: location parent cycle'; q "UPDATE locations SET parent_id=NULL WHERE id=$SITE"
q "INSERT INTO locations(name,loc_type,code,source_id,asserted_kind,asserted_at) VALUES('Code A','site','DUP',$SRC,'human',NOW()),('Code B','site','DUP',$SRC,'human',NOW())"
refuse "$M" '036 preflight: duplicate location codes'; q "DELETE FROM locations WHERE name IN ('Code A','Code B')"
q "SET FOREIGN_KEY_CHECKS=0; INSERT INTO locations(name,loc_type,parent_id,source_id) VALUES('Orphan','site',4294967295,$SRC)"
refuse "$M" '036 preflight: orphan location parent'; q "DELETE FROM locations WHERE name='Orphan'; SET FOREIGN_KEY_CHECKS=1"
apply "$M"; ok "036 clean apply after five preflight refusal checks"
eq "legacy root maps to property" "SELECT loc_type FROM locations WHERE id=$SITE" property
eq "soft-deleted root name key is NULL" "SELECT CONCAT(loc_type,':',location_name_key IS NULL) FROM locations WHERE id=$DELETED" property:1
eq "legacy rack and container map to storage_unit" "SELECT GROUP_CONCAT(loc_type ORDER BY id) FROM locations WHERE name IN ('Rack','Bag')" storage_unit,storage_unit
eq "generated live sibling unique index exists" "SELECT COUNT(*) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='locations' AND INDEX_NAME='uq_locations_live_sibling' AND NON_UNIQUE=0" 2
q "SET SESSION sql_mode=CONCAT(@@sql_mode,',NO_AUTO_VALUE_ON_ZERO'); INSERT INTO locations(id,name,loc_type,source_id) VALUES(0,'Zero ID','property',$SRC)" >"$ERR" 2>&1 && die "database accepted reserved location ID 0"
/usr/bin/grep -q 'location id 0 is reserved' "$ERR" || die "wrong zero-ID constraint result"; : >"$ERR"; ok "database reserves location ID 0 for root sentinel"
q "INSERT INTO container_types(name,source_id) VALUES('Fixture type',$SRC)"
refuse "$D" 'refusing rollback: container_types contains post-migration data'
q "DELETE FROM container_types WHERE name='Fixture type'"
# Valid transition matrix, plus nested storage.
q "INSERT INTO locations(name,loc_type,parent_id,source_id) VALUES('New Floor','floor',$SITE,$SRC)"; NF="$(q "SELECT id FROM locations WHERE name='New Floor'")"
q "INSERT INTO locations(name,loc_type,parent_id,source_id) VALUES('New Room','room',$NF,$SRC)"; NR="$(q "SELECT id FROM locations WHERE name='New Room'")"
q "INSERT INTO locations(name,loc_type,parent_id,source_id) VALUES('New Section','section',$NR,$SRC),('Storage at property','storage_unit',$SITE,$SRC),('Nested storage','storage_unit',$BAG,$SRC)"
ok "triggers accept valid hierarchy and nested storage units"
q "INSERT INTO locations(name,loc_type,source_id) VALUES('Invalid root','room',$SRC)" >"$ERR" 2>&1 && die "trigger accepted non-property root"
/usr/bin/grep -q 'location root must be property' "$ERR" || die "wrong invalid-root trigger result"; : >"$ERR"; ok "trigger rejects non-property roots"
q "INSERT INTO locations(name,loc_type,parent_id,source_id) VALUES('Invalid transition','room',$SITE,$SRC)" >"$ERR" 2>&1 && die "trigger accepted invalid transition"
/usr/bin/grep -q 'invalid location parent/level transition' "$ERR" || die "wrong transition trigger result"; : >"$ERR"; ok "trigger rejects invalid parent level"
q "UPDATE locations SET parent_id=$BAG WHERE id=$SITE" >"$ERR" 2>&1 && die "trigger accepted cycle"
/usr/bin/grep -q 'location parent cycle' "$ERR" || die "wrong cycle trigger result"; : >"$ERR"; ok "trigger rejects ancestor cycles"
q "DELETE FROM locations WHERE name IN ('New Section','Storage at property','Nested storage'); DELETE FROM locations WHERE id=$NR; DELETE FROM locations WHERE id=$NF"
apply "$D"
eq "rollback restores original enum label" "SELECT loc_type FROM locations WHERE id=$SITE" site
eq "rollback retains original deleted duplicate" "SELECT COUNT(*) FROM locations WHERE id=$DELETED AND deleted_at IS NOT NULL" 1
eq "rollback removes 036-only schema and triggers" "SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='container_types')+(SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME IN ('trg_locations_nonzero_id','trg_locations_level_ins','trg_locations_level_upd'))" 0
ok "rollback succeeds on untouched baseline after test fixtures removed"
echo "PASS: $pass assertions"
