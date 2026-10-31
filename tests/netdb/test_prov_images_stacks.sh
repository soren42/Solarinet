#!/usr/bin/env bash
# ===========================================================================
# test_prov_images_stacks.sh — migration 029 (provisioning images + stacks)
# on a SCRATCH database this script creates and drops itself.
#
#   * applies schema.sql + every migration (incl. 029), then 029 again;
#   * seed: core = 19 items, dev-tools role + p10k pin, Tumbleweed x86_64;
#   * seed-once: a soft-deleted seed row is not resurrected by a re-run;
#   * constraints: live-unique, self-dep, '=' on pkg, unpinned remote script,
#     build/role/artifact shape, sha256 format, pi_netboot arch, prov_runs
#     mode/image + JSON array, prov_runs immutability and no-delete;
#   * stackgen --from-db (as an ephemeral SELECT-only user) renders
#     byte-identically to tests/deploy/fixtures/stacks_seed.json;
#   * rollback 029-down.sql refuses non-seed state, then succeeds, then 029
#     re-applies cleanly (forward recovery).
#
# Never touches live `sor` / `solarinet`: the database name is fixed below,
# the script refuses if it already exists, and teardown only drops what it
# created.
#
# Local:  passwordless `sudo mariadb` (unix_socket root), as on the dev box.
#         STACKGEN_TEST_PYTHON=<python with pymysql> (default python3).
# CI:     SOLARI_TEST_SOR_DB=1 MYSQL_HOST=127.0.0.1 [MYSQL_PORT] [MYSQL_USER]
#         MYSQL_PWD=<root password of the disposable service container>;
#         STACKGEN_TEST_USER_HOST=% when the client is not on loopback.
# ===========================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOR="$ROOT/netdb/sor"
DB="sor_test_prov029"
PY="${STACKGEN_TEST_PYTHON:-python3}"
RO_USER="stackgen_ro_$$"
RO_HOST="${STACKGEN_TEST_USER_HOST:-127.0.0.1}"

case "$DB" in sor|solarinet|solarinet_stage|sor_probe) echo "refusing live database name $DB" >&2; exit 2 ;; esac

if [[ "${SOLARI_TEST_SOR_DB:-}" == "1" ]]; then
  [[ "${MYSQL_HOST:-}" == "127.0.0.1" ]] || { echo "refusing: CI mode needs MYSQL_HOST=127.0.0.1" >&2; exit 2; }
  ADMIN=(mariadb --protocol=TCP --host=127.0.0.1 --port="${MYSQL_PORT:-3306}" --user="${MYSQL_USER:-root}")
  DB_PORT="${MYSQL_PORT:-3306}"
else
  ADMIN=(sudo -n mariadb)
  DB_PORT=3306
fi
"$PY" -c 'import pymysql' 2>/dev/null || { echo "need pymysql in $PY (set STACKGEN_TEST_PYTHON)" >&2; exit 2; }

if "${ADMIN[@]}" -N -B -e "SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='$DB'" | grep -q .; then
  echo "refusing to run: database '$DB' already exists — drop it yourself if it is scratch" >&2
  exit 2
fi

created=0
user_created=0
cleanup() {
  if [[ "$user_created" == 1 ]]; then "${ADMIN[@]}" -e "DROP USER IF EXISTS '$RO_USER'@'$RO_HOST'" || true; fi
  if [[ "$created" == 1 ]]; then "${ADMIN[@]}" -e "DROP DATABASE IF EXISTS \`$DB\`" || true; fi
  rm -f "${TMP_A:-}" "${TMP_B:-}"
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
accepts() { "${ADMIN[@]}" "$DB" -e "$2" >/dev/null 2>&1 || die "rejected but should accept: $1"; ok "accepts $1"; }

seed_counts() {
  eq "seed stacks ($1)" "SELECT GROUP_CONCAT(slug ORDER BY slug) FROM prov_stacks" "core,dev-tools"
  eq "seed core = 19 pkg items ($1)" \
     "SELECT COUNT(*) FROM prov_stack_items i JOIN prov_stacks s ON s.id=i.stack_id WHERE s.slug='core' AND i.item_type='pkg' AND i.deleted_at IS NULL" 19
  eq "seed dev-tools items ($1)" \
     "SELECT GROUP_CONCAT(CONCAT(i.item_type,':',i.logical_name) ORDER BY i.logical_name) FROM prov_stack_items i JOIN prov_stacks s ON s.id=i.stack_id WHERE s.slug='dev-tools'" \
     "role:dev-tools role,script:powerlevel10k pin"
  eq "seed deps dev-tools->core ($1)" \
     "SELECT CONCAT(a.slug,'->',b.slug) FROM prov_stack_deps d JOIN prov_stacks a ON a.id=d.stack_id JOIN prov_stacks b ON b.id=d.depends_on_stack_id" "dev-tools->core"
  eq "seed images = 6 ($1)" "SELECT COUNT(*) FROM prov_images" 6
  eq "seed Tumbleweed x86_64 iPXE, unfetched ($1)" \
     "SELECT CONCAT(family,'/',boot_method,'/',refresh_policy,'/',IFNULL(sha256,'nosha'),'/',netboot_verified) FROM prov_images WHERE distro='opensuse' AND release_name='tumbleweed' AND arch='x86_64' AND deleted_at IS NULL" \
     "suse/ipxe/weekly/nosha/0"
  eq "no seeded sha256 anywhere ($1)" "SELECT COUNT(*) FROM prov_images WHERE sha256 IS NOT NULL OR version IS NOT NULL" 0
}

# ---- build ------------------------------------------------------------------
echo "== creating scratch $DB =="
"${ADMIN[@]}" -e "CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
created=1
apply "$SOR/schema.sql"
for m in "$SOR"/migrations/*.sql; do apply "$m"; done
ok "schema.sql + $(ls "$SOR"/migrations/*.sql | wc -l) migrations applied"
apply "$SOR/migrations/029_provisioning_images_stacks.sql"
ok "029 re-applied (idempotent)"

eq "six prov_* tables" "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('prov_images','prov_stacks','prov_stack_items','prov_stack_artifacts','prov_stack_deps','prov_runs')" 6
eq "prov_runs guard triggers" "SELECT GROUP_CONCAT(TRIGGER_NAME ORDER BY TRIGGER_NAME) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND EVENT_OBJECT_TABLE LIKE 'prov\\_%'" "trg_prov_runs_bd,trg_prov_runs_bu"
eq "no CDC on prov_* (no consumer yet)" "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND ACTION_STATEMENT LIKE '%prov\\_%' AND ACTION_STATEMENT LIKE '%sor_outbox%'" 0
eq "functions table untouched by 029" "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='functions' AND COLUMN_NAME LIKE 'prov%'" 0
seed_counts "after double apply"

# ---- seed-once ----------------------------------------------------------------
q "UPDATE prov_stack_items SET deleted_at=NOW() WHERE logical_name='powerlevel10k pin'"
apply "$SOR/migrations/029_provisioning_images_stacks.sql"
eq "re-run does not resurrect a soft-deleted seed row" \
   "SELECT CONCAT(COUNT(*),'/',SUM(deleted_at IS NULL)) FROM prov_stack_items WHERE logical_name='powerlevel10k pin'" "1/0"

# ---- constraints ----------------------------------------------------------------
SRC="(SELECT id FROM sources WHERE slug='provisioner')"
CORE="(SELECT id FROM prov_stacks WHERE slug='core')"
rejects "duplicate live image" \
  "INSERT INTO prov_images (distro,family,release_name,arch,boot_method,source_id) VALUES ('opensuse','suse','tumbleweed','x86_64','ipxe',$SRC)"
rejects "pi_netboot on x86_64" \
  "INSERT INTO prov_images (distro,family,release_name,arch,boot_method,source_id) VALUES ('raspios','debian','trixie','x86_64','pi_netboot',$SRC)"
rejects "malformed image sha256" \
  "INSERT INTO prov_images (distro,family,release_name,arch,boot_method,sha256,source_id) VALUES ('debian','debian','13','x86_64','ipxe','ABC',$SRC)"
rejects "duplicate live stack slug" "INSERT INTO prov_stacks (slug,name,source_id) VALUES ('core','Core 2',$SRC)"
accepts "temp stack" "INSERT INTO prov_stacks (slug,name,source_id) VALUES ('tmp-live','Tmp',$SRC)"
accepts "same slug once the first is soft-deleted" \
  "UPDATE prov_stacks SET deleted_at=NOW() WHERE slug='tmp-live'; INSERT INTO prov_stacks (slug,name,source_id) VALUES ('tmp-live','Tmp again',$SRC)"
q "DELETE FROM prov_stacks WHERE slug='tmp-live'"
rejects "self dependency" "INSERT INTO prov_stack_deps (stack_id,depends_on_stack_id,source_id) VALUES ($CORE,$CORE,$SRC)"
rejects "duplicate live item in a stack" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,name_suse,source_id) VALUES ($CORE,'git','git',$SRC)"
rejects "'=' as a pkg name" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,name_suse,source_id) VALUES ($CORE,'t1','=',$SRC)"
rejects "real name on a role item" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,ansible_role,source_id) VALUES ($CORE,'t2','role','dev-tools','dev-tools',$SRC)"
rejects "role without ansible_role" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,source_id) VALUES ($CORE,'t3','role','=',$SRC)"
rejects "spec on a pkg item" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,name_suse,spec,source_id) VALUES ($CORE,'t4','x','{}',$SRC)"
rejects "repo_ref on a non-pkg item" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,ansible_role,repo_ref,source_id) VALUES ($CORE,'t5','role','=','r','x',$SRC)"
rejects "unpinned remote script (url, no sha256)" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,spec,source_id) VALUES ($CORE,'t6','script','=','{\"url\":\"https://get.example.invalid/i.sh\",\"creates\":\"/x\"}',$SRC)"
rejects "script with both body and url" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,spec,source_id) VALUES ($CORE,'t7','script','=','{\"body\":\"true\",\"url\":\"u\",\"sha256\":\"$(printf 'a%.0s' {1..64})\"}',$SRC)"
rejects "script without spec" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,source_id) VALUES ($CORE,'t8','script','=',$SRC)"
accepts "pinned remote script" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,spec,source_id) VALUES ($CORE,'t9','script','=','{\"url\":\"https://get.example.invalid/i.sh\",\"sha256\":\"$(printf 'a%.0s' {1..64})\",\"creates\":\"/x\"}',$SRC)"
rejects "build without source" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,version_pin,spec,source_id) VALUES ($CORE,'t10','build','=','v1','{\"system\":\"make\"}',$SRC)"
rejects "build without version_pin" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,spec,source_id) VALUES ($CORE,'t11','build','=','{\"source\":{\"git\":\"u\"},\"system\":\"make\"}',$SRC)"
rejects "artifact without version_pin" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,source_id) VALUES ($CORE,'t12','artifact','=',$SRC)"
accepts "artifact item" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,version_pin,source_id) VALUES ($CORE,'t13','artifact','=','1.0',$SRC)"
rejects "malformed artifact sha256" \
  "INSERT INTO prov_stack_artifacts (item_id,arch,url,sha256,source_id) VALUES ((SELECT id FROM prov_stack_items WHERE logical_name='t13'),'any','u','XYZ',$SRC)"
rejects "invalid spec JSON" \
  "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,spec,source_id) VALUES ($CORE,'t14','repo','=','{nope',$SRC)"

# ---- review regressions (cross-lab review of 11db0e9/f01e45a) ------------------
H64="$(printf 'a%.0s' {1..64})"; C40="$(printf 'b%.0s' {1..40})"
ins_item() {  # logical_name, item_type, version_pin-SQL, spec-SQL
  echo "INSERT INTO prov_stack_items (stack_id,logical_name,item_type,name_suse,version_pin,spec,source_id) VALUES ($CORE,'$1','$2','=',$3,$4,$SRC)"
}
# 1. version_pin is one safe path component
rejects "path-escaping version_pin" "$(ins_item r1 artifact "'../../../../etc/cron.d'" NULL)"
rejects "version_pin '..'" "$(ins_item r2 artifact "'..'" NULL)"
rejects "version_pin with a slash" "$(ins_item r3 artifact "'a/b'" NULL)"
rejects "version_pin with a trailing newline" "$(ins_item r4 artifact "CONCAT('v1',CHAR(10))" NULL)"
rejects "version_pin with a leading dash" "$(ins_item r5 artifact "'-rf'" NULL)"
accepts "version_pin 2024.10+git~r1" "$(ins_item r6 artifact "'2024.10+git~r1'" NULL)"
# 2. git builds are pinned by full commit SHA only
GITSPEC="'{\"source\":{\"git\":\"https://e.invalid/x.git\",\"tag\":\"v1\"},\"system\":\"cmake\",\"creates\":\"/x\"}'"
rejects "git build pinned by tag" "$(ins_item r7 build "'v1.0.0'" "$GITSPEC")"
rejects "git build pinned by branch-ish ref" "$(ins_item r8 build "'feature-release'" "$GITSPEC")"
rejects "git build pinned by abbreviated SHA" "$(ins_item r9 build "'${C40:0:12}'" "$GITSPEC")"
rejects "git build pinned by uppercase SHA" "$(ins_item r10 build "'${C40^^}'" "$GITSPEC")"
accepts "git build pinned by full commit SHA" "$(ins_item r11 build "'$C40'" "$GITSPEC")"
rejects "build with both git and tarball" "$(ins_item r12 build "'$C40'" "'{\"source\":{\"git\":\"u\",\"tarball\":\"t\",\"sha256\":\"$H64\"},\"system\":\"make\"}'")"
rejects "build tarball without sha256" "$(ins_item r13 build "'1.0'" "'{\"source\":{\"tarball\":\"https://e.invalid/t.tgz\"},\"system\":\"make\"}'")"
rejects "build with unknown system" "$(ins_item r14 build "'$C40'" "'{\"source\":{\"git\":\"u\"},\"system\":\"bazel\"}'")"
# 3. guard check is an argv list
rejects "check as a shell string" "$(ins_item r15 script NULL "'{\"body\":\"true\",\"check\":\"test -x /x && true\"}'")"
rejects "check as an empty list" "$(ins_item r16 script NULL "'{\"body\":\"true\",\"check\":[]}'")"
accepts "check as an argv list" "$(ins_item r17 script NULL "'{\"body\":\"true\",\"check\":[\"/x\",\"--version\"]}'")"
# 5. JSON null / empty / malformed values are not values
rejects "script url JSON null + sha256" "$(ins_item r18 script NULL "'{\"url\":null,\"sha256\":\"$H64\",\"creates\":\"/x\"}'")"
rejects "script url + sha256 JSON null" "$(ins_item r19 script NULL "'{\"url\":\"https://e.invalid/i.sh\",\"sha256\":null}'")"
rejects "script url + malformed sha256" "$(ins_item r20 script NULL "'{\"url\":\"https://e.invalid/i.sh\",\"sha256\":\"abc\"}'")"
rejects "script url + uppercase sha256" "$(ins_item r21 script NULL "'{\"url\":\"https://e.invalid/i.sh\",\"sha256\":\"${H64^^}\"}'")"
rejects "script body JSON null" "$(ins_item r22 script NULL "'{\"body\":null}'")"
rejects "script empty body" "$(ins_item r23 script NULL "'{\"body\":\"\"}'")"
rejects "artifact row without sha256" \
  "INSERT INTO prov_stack_artifacts (item_id,arch,url,source_id) VALUES ((SELECT id FROM prov_stack_items WHERE logical_name='t13'),'any','https://e.invalid/a',$SRC)"
rejects "artifact row with uppercase sha256" \
  "INSERT INTO prov_stack_artifacts (item_id,arch,url,sha256,source_id) VALUES ((SELECT id FROM prov_stack_items WHERE logical_name='t13'),'any','https://e.invalid/a','${H64^^}',$SRC)"
rejects "image with uppercase sha256" \
  "INSERT INTO prov_images (distro,family,release_name,arch,boot_method,sha256,source_id) VALUES ('debian','debian','13','x86_64','ipxe','${H64^^}',$SRC)"
eq "p10k seed pinned by full commit SHA with an argv check" \
   "SELECT CONCAT(version_pin,' ',JSON_TYPE(JSON_EXTRACT(spec,'\$.check'))) FROM prov_stack_items WHERE logical_name='powerlevel10k pin' AND deleted_at IS NOT NULL" \
   "ff0311157d6b24fea21aa70699783f362b0f554f ARRAY"
q "DELETE FROM prov_stack_items WHERE logical_name IN ('r6','r11','r17')"
q "DELETE FROM prov_stack_items WHERE logical_name IN ('t9','t13')"

# ---- prov_runs ------------------------------------------------------------------
q "INSERT INTO entities (name, entity_type, source_id) VALUES ('prov029-test', 'server', $SRC)"
ENT="(SELECT id FROM entities WHERE name='prov029-test')"
IMG="(SELECT id FROM prov_images WHERE release_name='tumbleweed')"
rejects "reimage without an image" \
  "INSERT INTO prov_runs (mode,entity_id,stack_slugs,requested_by,source_id) VALUES ('reimage',$ENT,'[\"core\"]','t',$SRC)"
rejects "apply with an image" \
  "INSERT INTO prov_runs (mode,entity_id,image_id,stack_slugs,requested_by,source_id) VALUES ('apply',$ENT,$IMG,'[\"core\"]','t',$SRC)"
rejects "stack_slugs not a JSON array" \
  "INSERT INTO prov_runs (mode,entity_id,stack_slugs,requested_by,source_id) VALUES ('apply',$ENT,'{\"a\":1}','t',$SRC)"
accepts "reimage run" \
  "INSERT INTO prov_runs (mode,entity_id,image_id,stack_slugs,requested_by,dry_run,source_id) VALUES ('reimage',$ENT,$IMG,'[\"core\",\"dev-tools\"]','jason',1,$SRC)"
RUN="(SELECT MAX(id) FROM prov_runs)"
accepts "status queued -> running" "UPDATE prov_runs SET status='running', started_at=NOW() WHERE id=$RUN"
rejects "editing a request column" "UPDATE prov_runs SET stack_slugs='[\"core\"]' WHERE id=$RUN"
rejects "status back to queued" "UPDATE prov_runs SET status='queued' WHERE id=$RUN"
accepts "status running -> succeeded" "UPDATE prov_runs SET status='succeeded', finished_at=NOW(), detail='ok' WHERE id=$RUN"
rejects "changing a terminal run" "UPDATE prov_runs SET detail='rewritten' WHERE id=$RUN"
rejects "deleting a run" "DELETE FROM prov_runs WHERE id=$RUN"

# ---- stackgen --from-db as a SELECT-only user -----------------------------------
q "DELETE FROM prov_stack_items WHERE logical_name='powerlevel10k pin'"   # drop the soft-deleted copy
apply "$SOR/migrations/029_provisioning_images_stacks.sql"                # seed-once re-adds it fresh
RO_PASS="$("$PY" -c 'import secrets; print(secrets.token_hex(16))')"
"${ADMIN[@]}" -e "CREATE USER '$RO_USER'@'$RO_HOST' IDENTIFIED BY '$RO_PASS'"
user_created=1
"${ADMIN[@]}" -e "GRANT SELECT ON \`$DB\`.* TO '$RO_USER'@'$RO_HOST'"
TMP_A="$(mktemp)"; TMP_B="$(mktemp)"
for target in "suse x86_64" "debian arm64" "debian x86_64"; do
  set -- $target
  SOR_DB_HOST=127.0.0.1 SOR_DB_PORT="$DB_PORT" SOR_DB_NAME="$DB" SOR_DB_USER="$RO_USER" SOR_DB_PASS="$RO_PASS" \
    "$PY" "$ROOT/deploy/fleet/stackgen.py" --from-db --family "$1" --arch "$2" --stack dev-tools > "$TMP_A" \
    || die "stackgen --from-db $1/$2"
  python3 "$ROOT/deploy/fleet/stackgen.py" --from-json "$ROOT/tests/deploy/fixtures/stacks_seed.json" \
    --family "$1" --arch "$2" --stack dev-tools > "$TMP_B"
  cmp -s "$TMP_A" "$TMP_B" || { diff -u "$TMP_B" "$TMP_A" >&2; die "DB render != fixture render for $1/$2"; }
  ok "stackgen --from-db == fixture render ($1/$2, core+dev-tools)"
done
if SOR_DB_HOST=127.0.0.1 SOR_DB_PORT="$DB_PORT" SOR_DB_NAME="$DB" SOR_DB_USER="$RO_USER" SOR_DB_PASS="$RO_PASS" \
   "$PY" "$ROOT/deploy/fleet/stackgen.py" --from-db --family fedora --arch x86_64 >/dev/null 2>&1; then
  die "fedora core render should fail (netcat unmapped)"
fi
ok "stackgen --from-db fedora core is a hard error"

# ---- rollback -----------------------------------------------------------------------
if "${ADMIN[@]}" "$DB" < "$SOR/rollback/029-down.sql" >/dev/null 2>&1; then die "rollback accepted recorded runs"; fi
ok "rollback refuses while prov_runs holds records"
q "DROP TRIGGER trg_prov_runs_bd; DELETE FROM prov_runs; DROP PROCEDURE IF EXISTS _solarinet_rollback_m029_assert_seed_only"
q "UPDATE prov_images SET netboot_verified=1 WHERE release_name='tumbleweed'"
if "${ADMIN[@]}" "$DB" < "$SOR/rollback/029-down.sql" >/dev/null 2>&1; then die "rollback accepted an edited image"; fi
ok "rollback refuses operator-edited rows"
q "DROP PROCEDURE IF EXISTS _solarinet_rollback_m029_assert_seed_only; DELETE FROM prov_images WHERE release_name='tumbleweed'"
apply "$SOR/migrations/029_provisioning_images_stacks.sql"   # re-seed the image fresh
apply "$SOR/rollback/029-down.sql"
eq "rollback removed all prov_* tables" "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME LIKE 'prov\\_%'" 0
eq "rollback removed prov_runs triggers" "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME LIKE 'trg_prov\\_runs%'" 0
eq "rollback left no helper procedure" "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA=DATABASE() AND ROUTINE_NAME LIKE '%m029%'" 0
eq "rollback left 021 provisioner source intact" "SELECT COUNT(*) FROM sources WHERE slug='provisioner'" 1

apply "$SOR/migrations/029_provisioning_images_stacks.sql"
ok "029 re-applies after rollback (forward recovery)"
seed_counts "after forward recovery"

echo "PASS: $pass checks"
