#!/usr/bin/env bash
# ===========================================================================
# test_prov_cdc_030.sh — migration 030 (CDC outbox triggers for the nine
# 029 prov_* tables) on SCRATCH databases this script creates and drops itself.
#
#   * schema.sql + every migration (through 030) applies; 030 re-applies as a
#     no-op (identical trigger definitions, no outbox rows written);
#   * exactly 27 CDC triggers (3 x 9 tables), no collision with 029's
#     trg_prov_runs_bu/_bd guards, each one writes its own table's name;
#   * every table's insert / update / delete writes exactly one expected
#     sor_outbox row (action + row_id; prov_runs.id is BIGINT);
#   * prov_runs: insert + status updates publish; a DELETE is refused by the
#     029 guard and writes NOTHING; with the guard dropped (scratch only) the
#     AFTER DELETE trigger publishes; re-applying 029 restores the guard and
#     leaves the 030 triggers alone;
#   * rollback/030-down.sql removes exactly the 27 triggers (guards and the
#     024/026 CDC triggers untouched), is idempotent, DML then writes no outbox
#     rows; re-applying 030 brings them back;
#   * ledger rehearsal (tools/migrate.py) on a second scratch database: clean
#     ledgered install applies 030, a second run is a no-op, --check matches,
#     --verify-live matches netdb/sor/schema.snapshot.sql, and an upgrade from
#     a 029-only ledger (031 + 030 rolled back, rows removed) applies
#     exactly 030 then 031.
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
DB="sor_test_p3_cdc030"
LDB="sor_test_p3_cdc030_ledger"
LUSER="p3cdc030_$$"
M030="$SOR/migrations/030_provisioning_cdc.sql"
D030="$SOR/rollback/030-down.sql"

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
cleanup() {
  if [[ "$user_created" == 1 ]]; then "${ADMIN[@]}" -e "DROP USER IF EXISTS '$LUSER'@'localhost'" || true; fi
  if [[ "$lcreated" == 1 ]]; then "${ADMIN[@]}" -e "DROP DATABASE IF EXISTS \`$LDB\`" || true; fi
  if [[ "$created" == 1 ]]; then "${ADMIN[@]}" -e "DROP DATABASE IF EXISTS \`$DB\`" || true; fi
  [[ -z "$CNF" ]] || rm -f "$CNF"
}
trap cleanup EXIT

pass=0
ok() { pass=$((pass + 1)); echo "ok - $*"; }
die() { echo "FAIL: $*" >&2; exit 1; }
q() { "${ADMIN[@]}" -N -B "$DB" -e "$1"; }
apply() { "${ADMIN[@]}" "$DB" < "$1" || die "apply $(basename "$1")"; }
eq() { local got; got="$(q "$2")"; [[ "$got" == "$3" ]] || die "$1: expected '$3', got '$got'"; ok "$1"; }
rejects() {  # label, SQL — must fail
  if "${ADMIN[@]}" "$DB" -e "$2" >/dev/null 2>&1; then die "accepted but should reject: $1"; fi
  ok "rejects $1"
}

# ---- outbox helpers -----------------------------------------------------------
MARK=0
mark() { MARK="$(q "SELECT IFNULL(MAX(id),0) FROM sor_outbox")"; }
ob() { q "SELECT IFNULL(GROUP_CONCAT(CONCAT(table_name,'/',row_id,'/',action) ORDER BY id SEPARATOR ','),'') FROM sor_outbox WHERE id > $MARK"; }
expect_ob() {  # label expected — outbox rows since the last mark(), then re-mark
  local got; got="$(ob)"
  [[ "$got" == "$2" ]] || die "$1: outbox expected '$2', got '$got'"
  ok "$1 -> $2"; mark
}
ins() {  # SQL (one INSERT) -> prints the new AUTO_INCREMENT id
  "${ADMIN[@]}" -N -B "$DB" -e "$1; SELECT LAST_INSERT_ID();"
}
trig_fingerprint() {  # stable digest of every prov_* trigger definition
  q "SELECT MD5(GROUP_CONCAT(CONCAT_WS('|',TRIGGER_NAME,EVENT_MANIPULATION,ACTION_TIMING,EVENT_OBJECT_TABLE,ACTION_STATEMENT) ORDER BY TRIGGER_NAME SEPARATOR '#')) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND EVENT_OBJECT_TABLE LIKE 'prov\\_%'"
}
N030="SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME REGEXP '^trg_prov_[a-z_]+_a[iud]\$' AND EVENT_OBJECT_TABLE LIKE 'prov\\_%'"
NOTHER="SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND EVENT_OBJECT_TABLE NOT LIKE 'prov\\_%'"

# ---- build ------------------------------------------------------------------------
echo "== creating scratch $DB =="
"${ADMIN[@]}" -e "CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
created=1
apply "$SOR/schema.sql"
n_mig=0
for m in "$SOR"/migrations/*.sql; do apply "$m"; n_mig=$((n_mig + 1)); done
ok "schema.sql + $n_mig migrations applied (incl. 030)"
[[ -f "$M030" ]] || die "030 missing"


eq "27 CDC triggers on the nine prov_* tables" "$N030" 27
eq "029 guard triggers still present, untouched" \
   "SELECT GROUP_CONCAT(TRIGGER_NAME ORDER BY TRIGGER_NAME) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME IN ('trg_prov_runs_bu','trg_prov_runs_bd')" \
   "trg_prov_runs_bd,trg_prov_runs_bu"
eq "all prov_* triggers = 27 CDC + 2 guards" \
   "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND EVENT_OBJECT_TABLE LIKE 'prov\\_%'" 29
eq "each CDC trigger writes its OWN table name to sor_outbox, AFTER its event" \
   "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME REGEXP '^trg_prov_[a-z_]+_a[iud]\$' AND EVENT_OBJECT_TABLE LIKE 'prov\\_%' AND ACTION_STATEMENT LIKE '%sor_outbox%' AND ACTION_STATEMENT LIKE CONCAT('%VALUES (''',EVENT_OBJECT_TABLE,''',%') AND ACTION_TIMING='AFTER' AND TRIGGER_NAME=CONCAT('trg_',EVENT_OBJECT_TABLE,'_a',CASE EVENT_MANIPULATION WHEN 'INSERT' THEN 'i' WHEN 'UPDATE' THEN 'u' ELSE 'd' END)" 27
eq "nine distinct tables carry CDC" \
   "SELECT COUNT(DISTINCT EVENT_OBJECT_TABLE) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME REGEXP '^trg_prov_[a-z_]+_a[iud]\$' AND EVENT_OBJECT_TABLE LIKE 'prov\\_%'" 9
OTHER_BEFORE="$(q "$NOTHER")"

# ---- idempotent re-apply -------------------------------------------------------------
FP1="$(trig_fingerprint)"
mark
apply "$M030"
ok "030 re-applied"
[[ "$(trig_fingerprint)" == "$FP1" ]] || die "re-apply changed trigger definitions"
ok "re-apply is a no-op (identical trigger definitions)"
expect_ob "re-apply writes no outbox rows" ""
eq "still 27 CDC triggers after re-apply" "$N030" 27

# ---- per-table insert / update / delete ------------------------------------------------
SRC="(SELECT id FROM sources WHERE slug='provisioner')"
H64="$(printf 'a%.0s' $(seq 1 64))"
mark

IMG=$(ins "INSERT INTO prov_images (distro,family,release_name,arch,boot_method,source_id) VALUES ('debian','debian','cdc030','x86_64','ipxe',$SRC)")
expect_ob "prov_images insert" "prov_images/$IMG/insert"
q "UPDATE prov_images SET notes='cdc030 edit' WHERE id=$IMG"
expect_ob "prov_images update" "prov_images/$IMG/update"

SA=$(ins "INSERT INTO prov_software (logical_name,item_type,name_suse,name_debian,name_fedora,source_id) VALUES ('cdc030-a','pkg','a','a','a',$SRC)")
expect_ob "prov_software insert (pkg a)" "prov_software/$SA/insert"
SB=$(ins "INSERT INTO prov_software (logical_name,item_type,name_suse,name_debian,name_fedora,source_id) VALUES ('cdc030-b','pkg','b','b','b',$SRC)")
expect_ob "prov_software insert (pkg b)" "prov_software/$SB/insert"
SC=$(ins "INSERT INTO prov_software (logical_name,item_type,name_suse,version_pin,spec,source_id) VALUES ('cdc030-c','artifact','=','1','{}',$SRC)")
expect_ob "prov_software insert (artifact c)" "prov_software/$SC/insert"
q "UPDATE prov_software SET notes='cdc030 edit' WHERE id=$SA"
expect_ob "prov_software update" "prov_software/$SA/update"

AR=$(ins "INSERT INTO prov_software_artifacts (software_id,arch,url,sha256,source_id) VALUES ($SC,'any','https://e.invalid/c','$H64',$SRC)")
expect_ob "prov_software_artifacts insert" "prov_software_artifacts/$AR/insert"
q "UPDATE prov_software_artifacts SET url='https://e.invalid/c2' WHERE id=$AR"
expect_ob "prov_software_artifacts update" "prov_software_artifacts/$AR/update"

RQ=$(ins "INSERT INTO prov_software_requires (software_id,requires_software_id,source_id) VALUES ($SA,$SB,$SRC)")
expect_ob "prov_software_requires insert" "prov_software_requires/$RQ/insert"
q "UPDATE prov_software_requires SET notes='cdc030 edit' WHERE id=$RQ"
expect_ob "prov_software_requires update" "prov_software_requires/$RQ/update"

LO=$(( SA < SB ? SA : SB )); HI=$(( SA < SB ? SB : SA ))
CF=$(ins "INSERT INTO prov_software_conflicts (software_lo_id,software_hi_id,source_id) VALUES ($LO,$HI,$SRC)")
expect_ob "prov_software_conflicts insert" "prov_software_conflicts/$CF/insert"
q "UPDATE prov_software_conflicts SET notes='cdc030 edit' WHERE id=$CF"
expect_ob "prov_software_conflicts update" "prov_software_conflicts/$CF/update"

S1=$(ins "INSERT INTO prov_stacks (slug,name,source_id) VALUES ('cdc030-s1','CDC S1',$SRC)")
expect_ob "prov_stacks insert (s1)" "prov_stacks/$S1/insert"
S2=$(ins "INSERT INTO prov_stacks (slug,name,source_id) VALUES ('cdc030-s2','CDC S2',$SRC)")
expect_ob "prov_stacks insert (s2)" "prov_stacks/$S2/insert"
q "UPDATE prov_stacks SET description='cdc030 edit' WHERE id=$S1"
expect_ob "prov_stacks update" "prov_stacks/$S1/update"

MB=$(ins "INSERT INTO prov_stack_members (stack_id,software_id,source_id) VALUES ($S1,$SA,$SRC)")
expect_ob "prov_stack_members insert" "prov_stack_members/$MB/insert"
q "UPDATE prov_stack_members SET notes='cdc030 edit' WHERE id=$MB"
expect_ob "prov_stack_members update" "prov_stack_members/$MB/update"

DP=$(ins "INSERT INTO prov_stack_deps (stack_id,depends_on_stack_id,source_id) VALUES ($S2,$S1,$SRC)")
expect_ob "prov_stack_deps insert" "prov_stack_deps/$DP/insert"
q "UPDATE prov_stack_deps SET asserted_kind='machine' WHERE id=$DP"
expect_ob "prov_stack_deps update" "prov_stack_deps/$DP/update"

# prov_runs (BIGINT id): insert + two status updates
q "INSERT INTO entities (name, entity_type, source_id) VALUES ('cdc030-host', 'server', $SRC)"
mark
ENT="(SELECT id FROM entities WHERE name='cdc030-host')"
RUN=$(ins "INSERT INTO prov_runs (mode,entity_id,stack_slugs,requested_by,source_id) VALUES ('apply',$ENT,'[\"core\"]','cdc030',$SRC)")
expect_ob "prov_runs insert" "prov_runs/$RUN/insert"
q "UPDATE prov_runs SET status='running', started_at=NOW() WHERE id=$RUN"
expect_ob "prov_runs update (queued -> running)" "prov_runs/$RUN/update"
q "UPDATE prov_runs SET status='succeeded', finished_at=NOW(), detail='ok' WHERE id=$RUN"
expect_ob "prov_runs update (running -> succeeded)" "prov_runs/$RUN/update"
rejects "prov_runs update of a terminal run (029 guard)" "UPDATE prov_runs SET detail='x' WHERE id=$RUN"
expect_ob "refused prov_runs update writes no outbox row" ""
rejects "prov_runs DELETE (029 guard)" "DELETE FROM prov_runs WHERE id=$RUN"
expect_ob "refused prov_runs delete writes no outbox row" ""
eq "prov_runs row survived the refused delete" "SELECT COUNT(*) FROM prov_runs WHERE id=$RUN" 1

# prov_runs AFTER DELETE: fires only if the guard is gone (scratch-only drop).
q "DROP TRIGGER trg_prov_runs_bd"
q "DELETE FROM prov_runs WHERE id=$RUN"
expect_ob "prov_runs AFTER DELETE trigger publishes once the guard is dropped" "prov_runs/$RUN/delete"
apply "$SOR/migrations/029_provisioning_images_stacks.sql"
ok "029 re-applied (restores the guard)"
mark   # 029 seed re-runs may write outbox rows; not under test here
eq "029 re-apply restored the guard and left the 27 CDC triggers" \
   "SELECT CONCAT((SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME='trg_prov_runs_bd'),'/',($N030))" "1/27"
RUN2=$(ins "INSERT INTO prov_runs (mode,entity_id,stack_slugs,requested_by,source_id) VALUES ('apply',$ENT,'[\"core\"]','cdc030b',$SRC)")
expect_ob "prov_runs insert after guard restored" "prov_runs/$RUN2/insert"
rejects "prov_runs DELETE refused again" "DELETE FROM prov_runs WHERE id=$RUN2"
expect_ob "restored guard: delete writes nothing" ""

# deletes, leaf to root (FK cascades do not fire triggers, so no cascades relied on)
q "DELETE FROM prov_stack_deps WHERE id=$DP";               expect_ob "prov_stack_deps delete" "prov_stack_deps/$DP/delete"
q "DELETE FROM prov_stack_members WHERE id=$MB";            expect_ob "prov_stack_members delete" "prov_stack_members/$MB/delete"
q "DELETE FROM prov_stacks WHERE id=$S2";                   expect_ob "prov_stacks delete (s2)" "prov_stacks/$S2/delete"
q "DELETE FROM prov_stacks WHERE id=$S1";                   expect_ob "prov_stacks delete (s1)" "prov_stacks/$S1/delete"
q "DELETE FROM prov_software_conflicts WHERE id=$CF";       expect_ob "prov_software_conflicts delete" "prov_software_conflicts/$CF/delete"
q "DELETE FROM prov_software_requires WHERE id=$RQ";        expect_ob "prov_software_requires delete" "prov_software_requires/$RQ/delete"
q "DELETE FROM prov_software_artifacts WHERE id=$AR";       expect_ob "prov_software_artifacts delete" "prov_software_artifacts/$AR/delete"
q "DELETE FROM prov_software WHERE id=$SA";                 expect_ob "prov_software delete (a)" "prov_software/$SA/delete"
q "DELETE FROM prov_software WHERE id=$SB";                 expect_ob "prov_software delete (b)" "prov_software/$SB/delete"
q "DELETE FROM prov_software WHERE id=$SC";                 expect_ob "prov_software delete (c)" "prov_software/$SC/delete"
q "DELETE FROM prov_images WHERE id=$IMG";                  expect_ob "prov_images delete" "prov_images/$IMG/delete"

# soft delete is an UPDATE to consumers (deleted_at), never a delete event
IMG2=$(ins "INSERT INTO prov_images (distro,family,release_name,arch,boot_method,source_id) VALUES ('debian','debian','cdc030sd','x86_64','ipxe',$SRC)")
mark
q "UPDATE prov_images SET deleted_at=NOW() WHERE id=$IMG2"
expect_ob "soft delete publishes as an update" "prov_images/$IMG2/update"
q "DELETE FROM prov_images WHERE id=$IMG2"; mark

# ---- rollback ----------------------------------------------------------------------------
apply "$D030"
ok "030-down applied"
eq "rollback removed all 27 CDC triggers" "$N030" 0
eq "rollback left both 029 guards" \
   "SELECT GROUP_CONCAT(TRIGGER_NAME ORDER BY TRIGGER_NAME) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND EVENT_OBJECT_TABLE LIKE 'prov\\_%'" \
   "trg_prov_runs_bd,trg_prov_runs_bu"
eq "rollback left every non-prov trigger (022-026 CDC etc.) alone" "$NOTHER" "$OTHER_BEFORE"
eq "rollback left the nine tables" "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME LIKE 'prov\\_%'" 9
mark
SX=$(ins "INSERT INTO prov_stacks (slug,name,source_id) VALUES ('cdc030-off','Off',$SRC)")
q "UPDATE prov_stacks SET description='x' WHERE id=$SX"
q "DELETE FROM prov_stacks WHERE id=$SX"
expect_ob "with 030 rolled back, DML writes no prov_* outbox rows" ""
apply "$D030"
ok "030-down is idempotent (second apply)"
eq "still 0 CDC triggers" "$N030" 0

apply "$M030"
ok "030 re-applied after rollback"
eq "27 CDC triggers back" "$N030" 27
[[ "$(trig_fingerprint)" == "$FP1" ]] || die "forward recovery differs from the original trigger definitions"
ok "forward recovery restores identical trigger definitions"
mark
SX=$(ins "INSERT INTO prov_stacks (slug,name,source_id) VALUES ('cdc030-on','On',$SRC)")
expect_ob "DML publishes again after forward recovery" "prov_stacks/$SX/insert"
q "DELETE FROM prov_stacks WHERE id=$SX"; mark

# ---- ledger rehearsal (tools/migrate.py) ----------------------------------------------------
echo "== ledger rehearsal on scratch $LDB =="
"${ADMIN[@]}" -e "CREATE DATABASE \`$LDB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
lcreated=1
if [[ "$local_mode" == 1 ]]; then
  umask 077
  CNF="$(mktemp /tmp/p3cdc030.XXXXXX.cnf)"
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

out="$(mig)" || die "migrate.py clean install failed: $out"
n_files=$(( $(ls "$SOR"/migrations/*.sql | wc -l) + 1 ))   # + schema.sql
[[ "$(grep -c '^APPLIED ' <<<"$out")" == "$n_files" ]] || die "expected $n_files APPLIED lines, got: $out"
grep -q '^APPLIED 030_provisioning_cdc ' <<<"$out" || die "030 not applied by the runner: $out"
ok "ledgered clean install applied $n_files files incl. 030_provisioning_cdc"
[[ "$(L "SELECT COUNT(*) FROM solari_migration_ledger WHERE state='applied'")" == "$n_files" ]] || die "ledger row count"
[[ "$(L "SELECT state FROM solari_migration_ledger WHERE migration_id='030_provisioning_cdc'")" == "applied" ]] || die "030 ledger state"
ok "ledger has $n_files applied rows; 030 state=applied"
[[ "$(L "SELECT checksum FROM solari_migration_ledger WHERE migration_id='030_provisioning_cdc'")" == "$(sha256sum "$M030" | cut -d' ' -f1)" ]] || die "030 ledger checksum"
ok "030 ledger checksum = sha256 of the file"
[[ "$(L "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME REGEXP '^trg_prov_[a-z_]+_a[iud]\$'")" == 27 ]] || die "27 triggers via the runner"
ok "runner install created the 27 CDC triggers"

out="$(mig)" || die "second run failed: $out"
[[ -z "$out" ]] || die "second run was not a no-op: $out"
ok "second run is a no-op"
out="$(mig --check)" || die "--check failed: $out"
[[ "$out" == "LEDGER MATCH: $n_files applied migration files" ]] || die "--check output: $out"
ok "--check: $out"
out="$(mig --verify-live)" || die "--verify-live failed: $out"
grep -q '^MATCH: live schema matches the candidate snapshot' <<<"$out" || die "--verify-live output: $out"
ok "--verify-live: live schema matches netdb/sor/schema.snapshot.sql (snapshot includes the 27 triggers)"

# upgrade path: a ledger that stops at 029 picks up 030 (then 031, which
# follows it; the runner refuses a gap, so 031 is rolled back first too)
"${ADMIN[@]}" "$LDB" < "$SOR/rollback/031-down.sql"
"${ADMIN[@]}" "$LDB" < "$D030"
L "DELETE FROM solari_migration_ledger WHERE migration_id IN ('030_provisioning_cdc','031_provisioning_arch_refresh')"
out="$(mig --check)" || die "--check on 029-only ledger failed: $out"
[[ "$(grep '^PENDING ' <<<"$out" | cut -d' ' -f2 | paste -sd,)" == "030_provisioning_cdc,031_provisioning_arch_refresh" ]] \
  || die "029-only ledger: expected exactly 030,031 pending: $out"
ok "029-only ledger: --check reports exactly 030 then 031 pending"
out="$(mig)" || die "upgrade run failed: $out"
[[ "$(cut -d' ' -f1,2 <<<"$out" | paste -sd,)" == "APPLIED 030_provisioning_cdc,APPLIED 031_provisioning_arch_refresh" ]] \
  || die "upgrade applied something other than 030 then 031: $out"
ok "upgrade run applied only 030, then 031"
out="$(mig --verify-live)" || die "--verify-live after upgrade failed: $out"
ok "--verify-live after the 029 -> 030 upgrade"

echo "PASS: $pass checks"
