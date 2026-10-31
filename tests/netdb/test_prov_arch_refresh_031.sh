#!/usr/bin/env bash
# ===========================================================================
# test_prov_arch_refresh_031.sh — migration 031 (hardware_models.arch +
# prov_runs mode 'refresh') on SCRATCH databases this script creates and
# drops itself.
#
#   * schema.sql + every migration before 031 applies; a schema digest
#     (columns, CHECKs, indexes, FKs, triggers — order-insensitive) is taken;
#   * 031 applies, re-applies as a no-op (identical digest), and creates or
#     replaces no trigger (29 prov_* triggers: 27 CDC from 030 + 2 guards);
#   * hardware_models.arch: nullable, NULL default, after hw_type, exactly the
#     prov_images.arch vocabulary; every value accepted, a bad one rejected;
#   * ck_prov_runs_shape: one accept per mode (reimage entity+image, apply
#     entity only, refresh image only) and every wrong shape rejected; the 029
#     ck_prov_runs_image is gone;
#   * a refresh run publishes CDC like any prov_runs row; the 029 guard still
#     makes its NULL entity_id (and image/mode) immutable, the forward-only
#     lifecycle holds, terminal is final, deletes are refused;
#   * rollback/031-down.sql refuses (DDL untouched) while refresh runs or arch
#     values exist; then the schema digest after rollback equals the pre-031
#     digest exactly; a second rollback is a no-op; the 029 shape is enforced
#     again; re-applying 031 restores the post-031 digest (forward recovery);
#   * ledger rehearsal (tools/migrate.py) on a second scratch database: clean
#     ledgered install applies 031, a second run is a no-op, --check matches,
#     --verify-live matches netdb/sor/schema.snapshot.sql, and an upgrade from
#     a 030-only ledger (031-down + ledger row removed) applies only 031.
#
# Never touches live `sor` / `solarinet` / `solarinet_stage` / `sor_probe`:
# database names are fixed below, the script refuses if either already exists,
# and teardown only drops what it created.
#
# Local:  passwordless `sudo mariadb` (unix_socket root), as on the dev box;
#         the ledger rehearsal uses a throwaway account + 0600 option file.
# CI:     SOLARI_TEST_SOR_DB=1 MYSQL_HOST=127.0.0.1 [MYSQL_PORT] [MYSQL_USER]
#         MYSQL_PWD=<root password of the disposable service container>.
# ===========================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOR="$ROOT/netdb/sor"
DB="sor_test_prov031"
LDB="sor_test_prov031_ledger"
LUSER="prov031_$$"
M031="$SOR/migrations/031_provisioning_arch_refresh.sql"
D031="$SOR/rollback/031-down.sql"

for n in "$DB" "$LDB"; do
  case "$n" in sor|solarinet|solarinet_stage|sor_probe) echo "refusing live database name $n" >&2; exit 2 ;; esac
done

if [[ "${SOLARI_TEST_SOR_DB:-}" == "1" ]]; then
  [[ "${MYSQL_HOST:-}" == "127.0.0.1" ]] || { echo "refusing: CI mode needs MYSQL_HOST=127.0.0.1" >&2; exit 2; }
  ADMIN=(mariadb --protocol=TCP --host=127.0.0.1 --port="${MYSQL_PORT:-3306}" --user="${MYSQL_USER:-root}")
  MIG_ARGS=(--host 127.0.0.1 --port "${MYSQL_PORT:-3306}" --user "${MYSQL_USER:-root}")  # password: MYSQL_PWD env
  local_mode=0
else
  ADMIN=(sudo -n mariadb)
  local_mode=1
fi

for n in "$DB" "$LDB"; do
  if "${ADMIN[@]}" -N -B -e "SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='$n'" | grep -q .; then
    echo "refusing to run: database '$n' already exists — drop it yourself if it is scratch" >&2
    exit 2
  fi
done

created=0
lcreated=0
user_created=0
CNF=""
ERR="$(mktemp)"
cleanup() {
  if [[ "$user_created" == 1 ]]; then "${ADMIN[@]}" -e "DROP USER IF EXISTS '$LUSER'@'localhost'" || true; fi
  if [[ "$lcreated" == 1 ]]; then "${ADMIN[@]}" -e "DROP DATABASE IF EXISTS \`$LDB\`" || true; fi
  if [[ "$created" == 1 ]]; then "${ADMIN[@]}" -e "DROP DATABASE IF EXISTS \`$DB\`" || true; fi
  [[ -z "$CNF" ]] || rm -f "$CNF"
  rm -f "$ERR"
}
trap cleanup EXIT

pass=0
ok() { pass=$((pass + 1)); echo "ok - $*"; }
die() { echo "FAIL: $*" >&2; exit 1; }
q() { "${ADMIN[@]}" -N -B "$DB" -e "$1"; }
apply() { "${ADMIN[@]}" "$DB" < "$1" || die "apply $(basename "$1")"; }
eq() { local got; got="$(q "$2")"; [[ "$got" == "$3" ]] || die "$1: expected '$3', got '$got'"; ok "$1"; }
# Statements run under the strict sql_mode the migrations and helpers use, so a
# bad ENUM value is an error rather than a silent ''.
STRICT="SET SESSION sql_mode='STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';"
rejects() {  # label, SQL — must fail
  if "${ADMIN[@]}" "$DB" -e "$STRICT $2" >/dev/null 2>&1; then die "accepted but should reject: $1"; fi
  ok "rejects $1"
}
accepts() { "${ADMIN[@]}" "$DB" -e "$STRICT $2" >/dev/null 2>&1 || die "rejected but should accept: $1"; ok "accepts $1"; }
ins() {  # SQL (one INSERT) -> prints the new AUTO_INCREMENT id
  "${ADMIN[@]}" -N -B "$DB" -e "$STRICT $1; SELECT LAST_INSERT_ID();"
}

# Order-insensitive structural digest of the whole schema: every column
# (position, type, nullability, default, comment), CHECK clause, index, FK and
# trigger body. CHECK constraints are compared by name, so a rollback that
# re-adds ck_prov_runs_image last still matches the 029 CREATE TABLE.
schema_digest() {
  q "SELECT SHA2(CONCAT_WS('#',
    (SELECT GROUP_CONCAT(CONCAT_WS('|',TABLE_NAME,ORDINAL_POSITION,COLUMN_NAME,COLUMN_TYPE,IS_NULLABLE,IFNULL(COLUMN_DEFAULT,'<none>'),EXTRA,COLUMN_COMMENT)
            ORDER BY TABLE_NAME,ORDINAL_POSITION SEPARATOR '\n') FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE()),
    (SELECT GROUP_CONCAT(CONCAT_WS('|',TABLE_NAME,CONSTRAINT_NAME,CHECK_CLAUSE)
            ORDER BY TABLE_NAME,CONSTRAINT_NAME SEPARATOR '\n') FROM information_schema.CHECK_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE()),
    (SELECT GROUP_CONCAT(CONCAT_WS('|',TABLE_NAME,INDEX_NAME,NON_UNIQUE,SEQ_IN_INDEX,COLUMN_NAME)
            ORDER BY TABLE_NAME,INDEX_NAME,SEQ_IN_INDEX SEPARATOR '\n') FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE()),
    (SELECT GROUP_CONCAT(CONCAT_WS('|',TABLE_NAME,CONSTRAINT_NAME,REFERENCED_TABLE_NAME,UPDATE_RULE,DELETE_RULE)
            ORDER BY TABLE_NAME,CONSTRAINT_NAME SEPARATOR '\n') FROM information_schema.REFERENTIAL_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE()),
    (SELECT GROUP_CONCAT(CONCAT_WS('|',TRIGGER_NAME,EVENT_OBJECT_TABLE,EVENT_MANIPULATION,ACTION_TIMING,ACTION_ORDER,ACTION_STATEMENT)
            ORDER BY TRIGGER_NAME SEPARATOR '\n') FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE())
  ), 256)"
}
NPROV_TRG="SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND EVENT_OBJECT_TABLE LIKE 'prov\\_%'"
NHW_TRG="SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND EVENT_OBJECT_TABLE='hardware_models'"
CKS="SELECT GROUP_CONCAT(CONSTRAINT_NAME ORDER BY CONSTRAINT_NAME) FROM information_schema.CHECK_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE() AND TABLE_NAME='prov_runs'"
ARCH_COL="SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_models' AND COLUMN_NAME='arch'"
RUNS_SHAPE="SELECT GROUP_CONCAT(CONCAT_WS(':',COLUMN_NAME,COLUMN_TYPE,IS_NULLABLE) ORDER BY ORDINAL_POSITION) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='prov_runs' AND COLUMN_NAME IN ('mode','entity_id','image_id')"

# ---- build: everything before 031 ---------------------------------------------------
echo "== creating scratch $DB =="
"${ADMIN[@]}" -e "CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
created=1
[[ -f "$M031" ]] || die "031 missing"
apply "$SOR/schema.sql"
n_pre=0
for m in "$SOR"/migrations/*.sql; do
  [[ "$m" == "$M031" ]] && continue
  b="$(basename "$m")"
  (( 10#${b:0:3} < 31 )) || die "migration after 031 ($b): extend this test's pre-031 baseline"
  apply "$m"; n_pre=$((n_pre + 1))
done
ok "schema.sql + $n_pre pre-031 migrations applied"
D_PRE="$(schema_digest)"
eq "pre-031: 029 shape (mode reimage/apply, entity NOT NULL)" "$RUNS_SHAPE" \
   "mode:enum('reimage','apply'):NO,entity_id:bigint(20) unsigned:NO,image_id:int(10) unsigned:YES"
eq "pre-031: ck_prov_runs_image present" "$CKS" "ck_prov_runs_dry,ck_prov_runs_image,ck_prov_runs_json"
eq "pre-031: no hardware_models.arch" "$ARCH_COL" 0
TRG_PRE="$(q "$NPROV_TRG")"
[[ "$TRG_PRE" == 29 ]] || die "pre-031 prov_* trigger count $TRG_PRE != 29"

# ---- apply + idempotent re-apply --------------------------------------------------------
apply "$M031"
ok "031 applied"
D_POST="$(schema_digest)"
[[ "$D_POST" != "$D_PRE" ]] || die "031 changed nothing"
apply "$M031"
[[ "$(schema_digest)" == "$D_POST" ]] || die "031 re-apply changed the schema"
ok "031 re-apply is a no-op (identical schema digest)"

eq "031 shape (mode + refresh, entity NULL-able)" "$RUNS_SHAPE" \
   "mode:enum('reimage','apply','refresh'):NO,entity_id:bigint(20) unsigned:YES,image_id:int(10) unsigned:YES"
eq "ck_prov_runs_shape replaces ck_prov_runs_image" "$CKS" "ck_prov_runs_dry,ck_prov_runs_json,ck_prov_runs_shape"
eq "hardware_models.arch: nullable, NULL default, after hw_type, prov_images vocabulary" \
   "SELECT CONCAT_WS('|',c.COLUMN_TYPE,c.IS_NULLABLE,IFNULL(c.COLUMN_DEFAULT,'NULL'),p.COLUMN_NAME)
      FROM information_schema.COLUMNS c JOIN information_schema.COLUMNS p
        ON p.TABLE_SCHEMA=c.TABLE_SCHEMA AND p.TABLE_NAME=c.TABLE_NAME AND p.ORDINAL_POSITION=c.ORDINAL_POSITION-1
     WHERE c.TABLE_SCHEMA=DATABASE() AND c.TABLE_NAME='hardware_models' AND c.COLUMN_NAME='arch'" \
   "enum('x86_64','arm64','armhf','riscv64','i386')|YES|NULL|hw_type"
eq "arch vocabulary identical to prov_images.arch" \
   "SELECT (SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_models' AND COLUMN_NAME='arch')
         = (SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='prov_images' AND COLUMN_NAME='arch')" 1
eq "031 created/replaced no trigger (29 prov_* = 27 CDC + 2 guards)" "$NPROV_TRG" "$TRG_PRE"
eq "hardware_models still carries no triggers" "$NHW_TRG" 0

# ---- hardware_models.arch values ------------------------------------------------------------
SRC="(SELECT id FROM sources WHERE slug='provisioner')"
HM=$(ins "INSERT INTO hardware_models (make,model,hw_type,source_id) VALUES ('Test031','Box','server',$SRC)")
eq "new hardware model defaults to arch NULL (unknown)" "SELECT IFNULL(arch,'NULL') FROM hardware_models WHERE id=$HM" NULL
for a in x86_64 arm64 armhf riscv64 i386; do
  accepts "arch '$a'" "UPDATE hardware_models SET arch='$a' WHERE id=$HM"
done
rejects "arch 'aarch64' (not the vocabulary)" "UPDATE hardware_models SET arch='aarch64' WHERE id=$HM"
rejects "arch 'any' (artifact-only value)" "UPDATE hardware_models SET arch='any' WHERE id=$HM"
rejects "arch '' (empty)" "UPDATE hardware_models SET arch='' WHERE id=$HM"
accepts "arch back to NULL (unknown)" "UPDATE hardware_models SET arch=NULL WHERE id=$HM"

# ---- ck_prov_runs_shape ----------------------------------------------------------------------
q "INSERT INTO entities (name, entity_type, source_id) VALUES ('prov031-host', 'server', $SRC)"
ENT="(SELECT id FROM entities WHERE name='prov031-host')"
IMG="(SELECT id FROM prov_images WHERE deleted_at IS NULL ORDER BY id LIMIT 1)"
run() {  # mode entity-SQL image-SQL
  echo "INSERT INTO prov_runs (mode,entity_id,image_id,stack_slugs,requested_by,source_id) VALUES ('$1',$2,$3,'[]','prov031',$SRC)"
}
accepts "reimage: entity + image" "$(run reimage "$ENT" "$IMG")"
accepts "apply: entity, no image" "$(run apply "$ENT" NULL)"
R_RF=$(ins "$(run refresh NULL "$IMG")"); ok "accepts refresh: image, no entity (run $R_RF)"
rejects "reimage without an image"    "$(run reimage "$ENT" NULL)"
rejects "reimage without an entity"   "$(run reimage NULL "$IMG")"
rejects "apply with an image"         "$(run apply "$ENT" "$IMG")"
rejects "apply without an entity"     "$(run apply NULL NULL)"
rejects "refresh with an entity"      "$(run refresh "$ENT" "$IMG")"
rejects "refresh without an image"    "$(run refresh NULL NULL)"
rejects "refresh with an entity, no image" "$(run refresh "$ENT" NULL)"
rejects "unknown mode"                "$(run bogus NULL "$IMG")"
eq "only the three valid shapes were stored" \
   "SELECT GROUP_CONCAT(CONCAT(mode,':',entity_id IS NOT NULL,':',image_id IS NOT NULL) ORDER BY id) FROM prov_runs" \
   "reimage:1:1,apply:1:0,refresh:0:1"
eq "refresh run published to the 030 outbox" \
   "SELECT COUNT(*) FROM sor_outbox WHERE table_name='prov_runs' AND row_id=$R_RF AND action='insert'" 1

# ---- 029 guard on a refresh run ---------------------------------------------------------------
rejects "refresh run: setting entity_id (NULL is immutable)" "UPDATE prov_runs SET entity_id=$ENT WHERE id=$R_RF"
rejects "refresh run: changing image_id" "UPDATE prov_runs SET image_id=NULL WHERE id=$R_RF"
rejects "refresh run: changing mode" "UPDATE prov_runs SET mode='reimage', entity_id=$ENT WHERE id=$R_RF"
accepts "refresh run: queued -> running" "UPDATE prov_runs SET status='running', started_at=NOW() WHERE id=$R_RF"
rejects "refresh run: running -> queued (forward-only)" "UPDATE prov_runs SET status='queued' WHERE id=$R_RF"
accepts "refresh run: running -> succeeded" "UPDATE prov_runs SET status='succeeded', finished_at=NOW(), detail='ok' WHERE id=$R_RF"
rejects "refresh run: terminal status is final" "UPDATE prov_runs SET detail='x' WHERE id=$R_RF"
rejects "refresh run: delete (audit log)" "DELETE FROM prov_runs WHERE id=$R_RF"
eq "refresh run kept its shape and terminal status" \
   "SELECT CONCAT(mode,'/',IFNULL(entity_id,'NULL'),'/',status) FROM prov_runs WHERE id=$R_RF" "refresh/NULL/succeeded"
eq "refresh run lifecycle published two updates" \
   "SELECT COUNT(*) FROM sor_outbox WHERE table_name='prov_runs' AND row_id=$R_RF AND action='update'" 2

# ---- rollback ------------------------------------------------------------------------------------
refused() {  # label, message fragment — 031-down must refuse and change no DDL
  if "${ADMIN[@]}" "$DB" < "$D031" >/dev/null 2>"$ERR"; then die "rollback accepted $1"; fi
  grep -q "$2" "$ERR" || { cat "$ERR" >&2; die "rollback refused $1 for another reason"; }
  [[ "$(schema_digest)" == "$D_POST" ]] || die "refused rollback ($1) changed the schema"
  q "DROP PROCEDURE IF EXISTS _solarinet_rollback_m031_assert_reversible"
  ok "rollback refuses $1 (schema untouched)"
}
q "UPDATE hardware_models SET arch='arm64' WHERE id=$HM"
refused "while refresh runs exist" "refresh runs or NULL entity_id"
# Scratch only: drop the delete guard to clear the audit rows, then restore it
# by re-applying 029 (CREATE OR REPLACE TRIGGER; seed-once, tables untouched).
q "DROP TRIGGER trg_prov_runs_bd; DELETE FROM prov_runs WHERE mode='refresh'"
apply "$SOR/migrations/029_provisioning_images_stacks.sql"
[[ "$(schema_digest)" == "$D_POST" ]] || die "029 re-apply did not restore the exact post-031 schema"
ok "scratch: refresh rows cleared, 029 guard restored (post-031 digest intact)"
refused "while hardware_models.arch holds a value" "hardware_models.arch holds operator values"
q "UPDATE hardware_models SET arch=NULL WHERE id=$HM"

apply "$D031"
ok "031-down applied"
[[ "$(schema_digest)" == "$D_PRE" ]] || die "schema after rollback differs from the pre-031 schema"
ok "schema digest after rollback == pre-031 digest (columns, comments, CHECKs, indexes, FKs, triggers)"
eq "rollback left the reimage/apply audit rows" \
   "SELECT GROUP_CONCAT(mode ORDER BY id) FROM prov_runs" "reimage,apply"
eq "rollback left no helper procedure" \
   "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA=DATABASE() AND ROUTINE_NAME LIKE '%m031%'" 0
apply "$D031"
[[ "$(schema_digest)" == "$D_PRE" ]] || die "second rollback changed the schema"
ok "031-down is idempotent (second apply, digest unchanged)"
rejects "029 shape again: refresh mode" "$(run refresh NULL "$IMG")"
rejects "029 shape again: NULL entity on apply" "$(run apply NULL NULL)"
rejects "029 shape again: apply with an image" "$(run apply "$ENT" "$IMG")"
rejects "029 shape again: hardware_models.arch is gone" "UPDATE hardware_models SET arch='x86_64' WHERE id=$HM"

apply "$M031"
[[ "$(schema_digest)" == "$D_POST" ]] || die "forward recovery differs from the original post-031 schema"
ok "031 re-applied after rollback: post-031 digest restored (forward recovery)"

# ---- ledger rehearsal (tools/migrate.py) ---------------------------------------------------------
echo "== ledger rehearsal on scratch $LDB =="
"${ADMIN[@]}" -e "CREATE DATABASE \`$LDB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
lcreated=1
if [[ "$local_mode" == 1 ]]; then
  umask 077
  CNF="$(mktemp /tmp/prov031.XXXXXX.cnf)"
  PW="$(openssl rand -hex 16)"
  printf "CREATE USER '%s'@'localhost' IDENTIFIED BY '%s'; GRANT ALL ON \`%s\`.* TO '%s'@'localhost';\n" \
    "$LUSER" "$PW" "$LDB" "$LUSER" | "${ADMIN[@]}"
  user_created=1
  printf '[client]\nuser=%s\npassword=%s\n' "$LUSER" "$PW" > "$CNF"
  MIG_ARGS=(--defaults-file "$CNF")
  unset PW
fi
L() { "${ADMIN[@]}" -N -B "$LDB" -e "$1"; }
mig() { python3 "$ROOT/tools/migrate.py" --database sor --db-name "$LDB" "${MIG_ARGS[@]}" "$@"; }
MID="031_provisioning_arch_refresh"

out="$(mig)" || die "migrate.py clean install failed: $out"
n_files=$(( $(find "$SOR/migrations" -maxdepth 1 -name '*.sql' | wc -l) + 1 ))   # + schema.sql
[[ "$(grep -c '^APPLIED ' <<<"$out")" == "$n_files" ]] || die "expected $n_files APPLIED lines, got: $out"
grep -q "^APPLIED $MID " <<<"$out" || die "031 not applied by the runner: $out"
ok "ledgered clean install applied $n_files files incl. $MID"
[[ "$(L "SELECT state FROM solari_migration_ledger WHERE migration_id='$MID'")" == "applied" ]] || die "031 ledger state"
[[ "$(L "SELECT checksum FROM solari_migration_ledger WHERE migration_id='$MID'")" == "$(sha256sum "$M031" | cut -d' ' -f1)" ]] || die "031 ledger checksum"
ok "031 ledger row applied, checksum = sha256 of the file"

out="$(mig)" || die "second run failed: $out"
[[ -z "$out" ]] || die "second run was not a no-op: $out"
ok "second run is a no-op"
out="$(mig --check)" || die "--check failed: $out"
[[ "$out" == "LEDGER MATCH: $n_files applied migration files" ]] || die "--check output: $out"
ok "--check: $out"
out="$(mig --verify-live)" || die "--verify-live failed: $out"
grep -q '^MATCH: live schema matches the candidate snapshot' <<<"$out" || die "--verify-live output: $out"
ok "--verify-live: live schema matches netdb/sor/schema.snapshot.sql (snapshot includes 031)"

# upgrade path: a ledger that stops at 030 picks up only 031
"${ADMIN[@]}" "$LDB" < "$D031" || die "031-down on the ledger database"
L "DELETE FROM solari_migration_ledger WHERE migration_id='$MID'"
out="$(mig --check)" || die "--check on 030-only ledger failed: $out"
grep -q "^PENDING $MID " <<<"$out" || die "031 not reported pending: $out"
[[ "$(grep -c '^PENDING ' <<<"$out")" == 1 ]] || die "more than 031 pending: $out"
ok "030-only ledger: --check reports exactly 031 pending"
out="$(mig)" || die "upgrade run failed: $out"
[[ "$out" =~ ^APPLIED\ 031_provisioning_arch_refresh\ [0-9a-f]{12}$ ]] || die "upgrade applied more than 031: $out"
ok "upgrade run applied only 031"
out="$(mig --verify-live)" || die "--verify-live after upgrade failed: $out"
ok "--verify-live after the 030 -> 031 upgrade"

echo "PASS: $pass checks"
