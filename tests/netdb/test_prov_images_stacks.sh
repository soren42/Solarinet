#!/usr/bin/env bash
# ===========================================================================
# test_prov_images_stacks.sh — migration 029 (provisioning images + stacks)
# on a SCRATCH database this script creates and drops itself.
#
#   * applies schema.sql + every migration (incl. 029), then 029 again;
#   * seed: pool of 21 entries; core = 19 pkg members, dev-tools = role +
#     p10k pin with a requires edge pin -> role; no conflicts; Tumbleweed x86_64;
#   * seed-once: a soft-deleted seed row is not resurrected by a re-run;
#   * pool: every prov_software CHECK with a paired accept/reject, live-unique
#     logical_name, members (live-unique, shared across stacks, RESTRICT),
#     requires (self, duplicate, RESTRICT), conflicts (canonical lo<hi,
#     self-pair, duplicate, CASCADE), spec.build_deps refused;
#   * constraints: live-unique, self-dep, '=' on pkg, unpinned remote script,
#     build/role/artifact shape (custom build+install split, case-sensitive
#     system), every check element a string, repo fields that could inject
#     config lines, control chars in logical_name,
#     sha256 format, pi_netboot arch, prov_runs
#     mode/image + JSON array, prov_runs immutability and no-delete;
#   * stackgen --from-db (as an ephemeral SELECT-only user) renders
#     byte-identically to tests/deploy/fixtures/stacks_seed.json, rejects a
#     DB conflict pair, and refuses a live member of a soft-deleted entry;
#   * rollback 029-down.sql refuses non-seed state (runs, operator rows,
#     edits made within the seed's own second, soft deletes), its documented
#     digest-recompute command reproduces the file's constants, then it
#     succeeds on the untouched seed, then 029 re-applies (forward recovery).
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
  eq "seed pool = 21 live entries, 19 pkg ($1)" \
     "SELECT CONCAT(COUNT(*),'/',SUM(item_type='pkg')) FROM prov_software WHERE deleted_at IS NULL" "21/19"
  eq "seed core = 19 pkg members ($1)" \
     "SELECT COUNT(*) FROM prov_stack_members m JOIN prov_stacks s ON s.id=m.stack_id JOIN prov_software p ON p.id=m.software_id WHERE s.slug='core' AND p.item_type='pkg' AND m.deleted_at IS NULL" 19
  eq "seed dev-tools members ($1)" \
     "SELECT GROUP_CONCAT(CONCAT(p.item_type,':',p.logical_name,'@',IFNULL(m.item_order,'-')) ORDER BY p.logical_name) FROM prov_stack_members m JOIN prov_stacks s ON s.id=m.stack_id JOIN prov_software p ON p.id=m.software_id WHERE s.slug='dev-tools'" \
     "role:dev-tools role@-,script:powerlevel10k pin@700"
  eq "seed requires p10k pin -> dev-tools role ($1)" \
     "SELECT GROUP_CONCAT(CONCAT(a.logical_name,'->',b.logical_name)) FROM prov_software_requires r JOIN prov_software a ON a.id=r.software_id JOIN prov_software b ON b.id=r.requires_software_id" \
     "powerlevel10k pin->dev-tools role"
  eq "seed no conflicts, no artifacts ($1)" \
     "SELECT (SELECT COUNT(*) FROM prov_software_conflicts)+(SELECT COUNT(*) FROM prov_software_artifacts)" 0
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

eq "nine prov_* tables, exactly" "SELECT GROUP_CONCAT(TABLE_NAME ORDER BY TABLE_NAME) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME LIKE 'prov\\_%'" \
   "prov_images,prov_runs,prov_software,prov_software_artifacts,prov_software_conflicts,prov_software_requires,prov_stacks,prov_stack_deps,prov_stack_members"
eq "no repo_ref column (dependencies are requires edges)" \
   "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME LIKE 'prov\\_%' AND COLUMN_NAME='repo_ref'" 0
eq "prov_runs guard triggers" "SELECT GROUP_CONCAT(TRIGGER_NAME ORDER BY TRIGGER_NAME) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND EVENT_OBJECT_TABLE LIKE 'prov\\_%' AND TRIGGER_NAME LIKE 'trg\\_prov\\_runs\\_b%'" "trg_prov_runs_bd,trg_prov_runs_bu"
eq "prov_* CDC triggers come from 030 (9 tables x ai/au/ad)" "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND ACTION_STATEMENT LIKE '%prov\\_%' AND ACTION_STATEMENT LIKE '%sor_outbox%'" 27
eq "functions table untouched by 029" "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='functions' AND COLUMN_NAME LIKE 'prov%'" 0
seed_counts "after double apply"

# ---- seed-once ----------------------------------------------------------------
q "UPDATE prov_software SET deleted_at=NOW() WHERE logical_name='powerlevel10k pin'"
apply "$SOR/migrations/029_provisioning_images_stacks.sql"
eq "re-run does not resurrect a soft-deleted seed row" \
   "SELECT CONCAT(COUNT(*),'/',SUM(deleted_at IS NULL)) FROM prov_software WHERE logical_name='powerlevel10k pin'" "1/0"
eq "re-run adds no second member or edge for it" \
   "SELECT CONCAT((SELECT COUNT(*) FROM prov_stack_members m JOIN prov_software p ON p.id=m.software_id WHERE p.logical_name='powerlevel10k pin'),'/',(SELECT COUNT(*) FROM prov_software_requires))" "1/1"

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
rejects "duplicate live logical_name in the pool" \
  "INSERT INTO prov_software (logical_name,name_suse,source_id) VALUES ('git','git',$SRC)"
accepts "pool entry" "INSERT INTO prov_software (logical_name,name_suse,source_id) VALUES ('tmp-sw','x',$SRC)"
rejects "same logical_name while the first is live" \
  "INSERT INTO prov_software (logical_name,name_suse,source_id) VALUES ('tmp-sw','y',$SRC)"
accepts "same logical_name once the first is soft-deleted" \
  "UPDATE prov_software SET deleted_at=NOW() WHERE logical_name='tmp-sw'; INSERT INTO prov_software (logical_name,name_suse,source_id) VALUES ('tmp-sw','y',$SRC)"
q "DELETE FROM prov_software WHERE logical_name='tmp-sw'"
rejects "'=' as a pkg name" \
  "INSERT INTO prov_software (logical_name,name_suse,source_id) VALUES ('t1','=',$SRC)"
rejects "real name on a role item" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,ansible_role,source_id) VALUES ('t2','role','dev-tools','dev-tools',$SRC)"
rejects "role without ansible_role" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,source_id) VALUES ('t3','role','=',$SRC)"
rejects "spec on a pkg item" \
  "INSERT INTO prov_software (logical_name,name_suse,spec,source_id) VALUES ('t4','x','{}',$SRC)"
rejects "unpinned remote script (url, no sha256)" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,spec,source_id) VALUES ('t6','script','=','{\"url\":\"https://get.example.invalid/i.sh\",\"creates\":\"/x\"}',$SRC)"
rejects "script with both body and url" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,spec,source_id) VALUES ('t7','script','=','{\"body\":\"true\",\"url\":\"u\",\"sha256\":\"$(printf 'a%.0s' {1..64})\"}',$SRC)"
rejects "script without spec" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,source_id) VALUES ('t8','script','=',$SRC)"
accepts "pinned remote script" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,spec,source_id) VALUES ('t9','script','=','{\"url\":\"https://get.example.invalid/i.sh\",\"sha256\":\"$(printf 'a%.0s' {1..64})\",\"creates\":\"/x\"}',$SRC)"
rejects "build without source" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,version_pin,spec,source_id) VALUES ('t10','build','=','v1','{\"system\":\"make\"}',$SRC)"
rejects "build without version_pin" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,spec,source_id) VALUES ('t11','build','=','{\"source\":{\"git\":\"u\"},\"system\":\"make\"}',$SRC)"
rejects "artifact without version_pin" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,source_id) VALUES ('t12','artifact','=',$SRC)"
accepts "artifact item" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,version_pin,source_id) VALUES ('t13','artifact','=','1.0',$SRC)"
rejects "malformed artifact sha256" \
  "INSERT INTO prov_software_artifacts (software_id,arch,url,sha256,source_id) VALUES ((SELECT id FROM prov_software WHERE logical_name='t13'),'any','u','XYZ',$SRC)"
rejects "invalid spec JSON" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,spec,source_id) VALUES ('t14','repo','=','{nope',$SRC)"

# ---- review regressions (cross-lab review of 11db0e9/f01e45a) ------------------
H64="$(printf 'a%.0s' {1..64})"; C40="$(printf 'b%.0s' {1..40})"
ins_item() {  # logical_name, item_type, version_pin-SQL, spec-SQL
  echo "INSERT INTO prov_software (logical_name,item_type,name_suse,version_pin,spec,source_id) VALUES ('$1','$2','=',$3,$4,$SRC)"
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
  "INSERT INTO prov_software_artifacts (software_id,arch,url,source_id) VALUES ((SELECT id FROM prov_software WHERE logical_name='t13'),'any','https://e.invalid/a',$SRC)"
rejects "artifact row with uppercase sha256" \
  "INSERT INTO prov_software_artifacts (software_id,arch,url,sha256,source_id) VALUES ((SELECT id FROM prov_software WHERE logical_name='t13'),'any','https://e.invalid/a','${H64^^}',$SRC)"
rejects "image with uppercase sha256" \
  "INSERT INTO prov_images (distro,family,release_name,arch,boot_method,sha256,source_id) VALUES ('debian','debian','13','x86_64','ipxe','${H64^^}',$SRC)"
eq "p10k seed pinned by full commit SHA with an argv check" \
   "SELECT CONCAT(version_pin,' ',JSON_TYPE(JSON_EXTRACT(spec,'\$.check'))) FROM prov_software WHERE logical_name='powerlevel10k pin' AND deleted_at IS NOT NULL" \
   "ff0311157d6b24fea21aa70699783f362b0f554f ARRAY"
# ---- review regressions (Luna cross-lab review of b027b22) ----------------------
# Each reject has a near-identical accept, so a reject cannot pass by tripping
# some unrelated CHECK. Specs are built with JSON_OBJECT/JSON_ARRAY so values
# are JSON-escaped exactly as an API would store them.
TARSRC="JSON_OBJECT('tarball','https://e.invalid/t.tgz','sha256','$H64')"
bspec() {  # system-SQL [extra JSON_OBJECT key/value pairs...]
  local sys="$1"; shift; local extra=""; for kv in "$@"; do extra="$extra,$kv"; done
  echo "JSON_OBJECT('source',$TARSRC,'system',$sys$extra)"
}
# 1. custom build = separate build (unprivileged) + install (root) commands
accepts "custom build with build + install" "$(ins_item r24 build "'1.0'" "$(bspec "'custom'" "'build','make'" "'install','make install'")")"
rejects "custom build without install" "$(ins_item r25 build "'1.0'" "$(bspec "'custom'" "'build','make'")")"
rejects "custom build without build" "$(ins_item r26 build "'1.0'" "$(bspec "'custom'" "'install','make install'")")"
rejects "custom build with whitespace-only install" "$(ins_item r27 build "'1.0'" "$(bspec "'custom'" "'build','make'" "'install','   '")")"
rejects "custom build with the legacy commands key" "$(ins_item r28 build "'1.0'" "$(bspec "'custom'" "'build','make'" "'install','make install'" "'commands','make install'")")"
rejects "custom build with a non-string build" "$(ins_item r29 build "'1.0'" "$(bspec "'custom'" "'build',JSON_ARRAY('make')" "'install','make install'")")"
accepts "cmake build" "$(ins_item r30 build "'1.0'" "$(bspec "'cmake'")")"
rejects "cmake build carrying an install command" "$(ins_item r31 build "'1.0'" "$(bspec "'cmake'" "'install','make install'")")"
rejects "cmake build carrying a build command" "$(ins_item r32 build "'1.0'" "$(bspec "'cmake'" "'build','make'")")"
# build deps are requires edges, never spec.build_deps (pair: r30 accepted above)
rejects "build with spec.build_deps" "$(ins_item r32b build "'1.0'" "$(bspec "'cmake'" "'build_deps',JSON_ARRAY('git')")")"
accepts "build with an optional configure_args list" "$(ins_item r32c build "'1.0'" "$(bspec "'cmake'" "'configure_args',JSON_ARRAY('-DX=1')")")"
# 4. build system is matched case-sensitively
rejects "build system 'CMake' (case-insensitive collation)" "$(ins_item r33 build "'1.0'" "$(bspec "'CMake'")")"
rejects "build system 'CUSTOM'" "$(ins_item r34 build "'1.0'" "$(bspec "'CUSTOM'" "'build','make'" "'install','make install'")")"
# 5. every element of check is a string (not just check[0])
cspec() { echo "JSON_OBJECT('body','true','check',$1)"; }
accepts "check argv with an escaped quote and an empty later arg" "$(ins_item r35 script NULL "$(cspec "JSON_ARRAY('/x','a\"b\\\\c','')")")"
rejects "check with a number element" "$(ins_item r36 script NULL "$(cspec "JSON_ARRAY('/x',1)")")"
rejects "check with a null element" "$(ins_item r37 script NULL "$(cspec "JSON_ARRAY('/x',NULL)")")"
rejects "check with a boolean element" "$(ins_item r38 script NULL "$(cspec "JSON_ARRAY('/x',TRUE)")")"
rejects "check with a nested array element" "$(ins_item r39 script NULL "$(cspec "JSON_ARRAY('/x',JSON_ARRAY('y'))")")"
rejects "check with an object element" "$(ins_item r40 script NULL "$(cspec "JSON_ARRAY('/x',JSON_OBJECT('a','b'))")")"
rejects "check with an empty argv[0]" "$(ins_item r41 script NULL "$(cspec "JSON_ARRAY('','x')")")"
# 3. repo fields cannot inject sources.list / .repo lines
U="'https://repo.e.invalid/deb'"; K="'https://repo.e.invalid/key.asc'"; F="'$C40'"; S="'stable'"; CMP="JSON_ARRAY('main','contrib')"
NL="CHAR(10)"; LS="_utf8mb4 0xE280A8"
rspec() {  # family url key fpr suite components (SQL; '-' = omit suite/components)
  local tail=""
  [[ "$5" == "-" ]] || tail="$tail,'suite',$5"
  [[ "$6" == "-" ]] || tail="$tail,'components',$6"
  echo "JSON_OBJECT('$1',JSON_OBJECT('url',$2,'key_url',$3,'fingerprint',$4$tail))"
}
accepts "debian repo with valid fields" "$(ins_item rp1 repo NULL "$(rspec debian "$U" "$K" "$F" "$S" "$CMP")")"
accepts "debian repo, slash suite + PHP-escaped slash component" \
  "$(ins_item rp2 repo NULL "'{\"debian\":{\"url\":\"https:\\\\/\\\\/repo.e.invalid\\\\/deb\",\"key_url\":\"https://repo.e.invalid/k\",\"fingerprint\":\"$C40\",\"suite\":\"stable/updates\",\"components\":[\"main\\\\/sub\",\"non-free\"]}}'")"
rejects "repo suite with a newline" "$(ins_item rp3 repo NULL "$(rspec debian "$U" "$K" "$F" "CONCAT('stable',$NL,'deb [trusted=yes] http://evil.invalid/ x main')" "$CMP")")"
rejects "repo suite with a leading dash" "$(ins_item rp4 repo NULL "$(rspec debian "$U" "$K" "$F" "'-oAPT::x'" "$CMP")")"
rejects "repo suite with a space" "$(ins_item rp5 repo NULL "$(rspec debian "$U" "$K" "$F" "'stable main'" "$CMP")")"
rejects "repo component with a leading dash" "$(ins_item rp6 repo NULL "$(rspec debian "$U" "$K" "$F" "$S" "JSON_ARRAY('main','-x')")")"
rejects "repo component with a space" "$(ins_item rp7 repo NULL "$(rspec debian "$U" "$K" "$F" "$S" "JSON_ARRAY('main contrib')")")"
rejects "repo component with a newline" "$(ins_item rp8 repo NULL "$(rspec debian "$U" "$K" "$F" "$S" "JSON_ARRAY(CONCAT('main',$NL,'deb x'))")")"
rejects "repo components empty list" "$(ins_item rp9 repo NULL "$(rspec debian "$U" "$K" "$F" "$S" "JSON_ARRAY()")")"
rejects "repo component not a string" "$(ins_item rp10 repo NULL "$(rspec debian "$U" "$K" "$F" "$S" "JSON_ARRAY('main',1)")")"
rejects "repo components as a string" "$(ins_item rp11 repo NULL "$(rspec debian "$U" "$K" "$F" "$S" "'main contrib'")")"
rejects "repo url with a space" "$(ins_item rp12 repo NULL "$(rspec debian "'https://a.invalid/ x main'" "$K" "$F" "$S" "$CMP")")"
rejects "repo url with a newline" "$(ins_item rp13 repo NULL "$(rspec debian "CONCAT('https://a.invalid/',$NL,'deb x')" "$K" "$F" "$S" "$CMP")")"
rejects "repo url with U+2028" "$(ins_item rp14 repo NULL "$(rspec debian "CONCAT('https://a.invalid/',$LS,'x')" "$K" "$F" "$S" "$CMP")")"
rejects "repo key_url with a CR" "$(ins_item rp15 repo NULL "$(rspec debian "$U" "CONCAT('https://a.invalid/k',CHAR(13))" "$F" "$S" "$CMP")")"
rejects "repo fingerprint with a newline" "$(ins_item rp16 repo NULL "$(rspec debian "$U" "$K" "CONCAT('$C40',$NL,'x')" "$S" "$CMP")")"
rejects "debian repo without suite" "$(ins_item rp17 repo NULL "$(rspec debian "$U" "$K" "$F" - "$CMP")")"
accepts "suse repo without suite/components" "$(ins_item rp18 repo NULL "$(rspec suse "$U" "$K" "$F" - -)")"
rejects "suse repo url with a newline" "$(ins_item rp19 repo NULL "$(rspec suse "CONCAT('https://a.invalid/',$NL,'x')" "$K" "$F" - -)")"
rejects "fedora repo key_url with a tab" "$(ins_item rp20 repo NULL "$(rspec fedora "$U" "CONCAT('https://a',CHAR(9),'b')" "$F" - -)")"
rejects "repo family block not an object" "$(ins_item rp21 repo NULL "JSON_OBJECT('fedora','https://a.invalid/')")"
# logical_name carries no control characters
rejects "logical_name with a newline" \
  "INSERT INTO prov_software (logical_name,name_suse,source_id) VALUES (CONCAT('ln',$NL,'x'),'x',$SRC)"
rejects "logical_name with U+2028" \
  "INSERT INTO prov_software (logical_name,name_suse,source_id) VALUES (CONCAT('ln',$LS,'x'),'x',$SRC)"
accepts "logical_name with spaces and punctuation" \
  "INSERT INTO prov_software (logical_name,name_suse,source_id) VALUES ('ln ok (x+y)','x',$SRC)"
eq "accepted check argv round-trips intact" \
   "SELECT JSON_LENGTH(spec,'\$.check') FROM prov_software WHERE logical_name='r35'" 3
q "DELETE FROM prov_software WHERE logical_name IN ('r24','r30','r32c','r35','rp1','rp2','rp18','ln ok (x+y)')"
q "DELETE FROM prov_software WHERE logical_name IN ('r6','r11','r17')"
q "DELETE FROM prov_software WHERE logical_name IN ('t9','t13')"

# ---- pool: each prov_software CHECK as an accept/reject pair --------------------
# Every reject differs from its accept in the one column the CHECK guards.
SW() { echo "(SELECT id FROM prov_software WHERE logical_name='$1' AND deleted_at IS NULL)"; }
pk() {  # logical_name item_type suse debian fedora role spec  (SQL literals)
  echo "INSERT INTO prov_software (logical_name,item_type,name_suse,name_debian,name_fedora,ansible_role,spec,source_id) VALUES ('$1','$2',$3,$4,$5,$6,$7,$SRC)"
}
accepts "ck_role: role with ansible_role"          "$(pk c1 role "'='" "'='" "'='" "'r'" NULL)"
rejects "ck_role: role without ansible_role"       "$(pk c2 role "'='" "'='" "'='" NULL NULL)"
rejects "ck_role: ansible_role on a pkg"           "$(pk c3 pkg "'x'" "'x'" "'x'" "'r'" NULL)"
accepts "ck_suse/debian/fedora: pkg real names"     "$(pk c4 pkg "'a'" "'b'" "'c'" NULL NULL)"
accepts "ck_suse/debian/fedora: pkg unsupported (NULL)" "$(pk c5 pkg NULL NULL NULL NULL NULL)"
rejects "ck_suse: '=' as a pkg name"                "$(pk c6 pkg "'='" "'b'" "'c'" NULL NULL)"
rejects "ck_debian: '=' as a pkg name"              "$(pk c7 pkg "'a'" "'='" "'c'" NULL NULL)"
rejects "ck_fedora: '=' as a pkg name"              "$(pk c8 pkg "'a'" "'b'" "'='" NULL NULL)"
rejects "ck_debian: empty pkg name"                 "$(pk c9 pkg "'a'" "''" "'c'" NULL NULL)"
rejects "ck_suse: real name on a role"              "$(pk c10 role "'x'" "'='" "'='" "'r'" NULL)"
rejects "ck_debian: real name on a role"            "$(pk c11 role "'='" "'x'" "'='" "'r'" NULL)"
rejects "ck_fedora: real name on a role"            "$(pk c12 role "'='" "'='" "'x'" "'r'" NULL)"
accepts "ck_spec: artifact with a JSON object spec" "INSERT INTO prov_software (logical_name,item_type,name_suse,version_pin,spec,source_id) VALUES ('c13','artifact','=','1','{}',$SRC)"
rejects "ck_spec: artifact with a JSON array spec"  "INSERT INTO prov_software (logical_name,item_type,name_suse,version_pin,spec,source_id) VALUES ('c14','artifact','=','1','[]',$SRC)"
rejects "ck_spec: artifact with invalid JSON"       "INSERT INTO prov_software (logical_name,item_type,name_suse,version_pin,spec,source_id) VALUES ('c15','artifact','=','1','{x',$SRC)"
rejects "ck_spec_req: spec on a role"               "$(pk c16 role "'='" "'='" "'='" "'r'" "'{}'")"
rejects "ck_spec_req: repo without spec"            "$(pk c17 repo "'='" "'='" "'='" NULL NULL)"
accepts "ck_pin: artifact with version_pin"         "INSERT INTO prov_software (logical_name,item_type,name_suse,version_pin,source_id) VALUES ('c18','artifact','=','2.0',$SRC)"
rejects "ck_pin: build without version_pin" \
  "INSERT INTO prov_software (logical_name,item_type,name_suse,spec,source_id) VALUES ('c19','build','=',$(bspec "'cmake'"),$SRC)"
accepts "ck_artifacts: well-formed artifact row" \
  "INSERT INTO prov_software_artifacts (software_id,arch,url,sha256,source_id) VALUES ($(SW c18),'any','https://e.invalid/a','$H64',$SRC)"
rejects "ck_artifacts_url: empty url" \
  "INSERT INTO prov_software_artifacts (software_id,arch,url,sha256,source_id) VALUES ($(SW c18),'arm64','','$H64',$SRC)"
rejects "ck_artifacts_sha256: short sha256" \
  "INSERT INTO prov_software_artifacts (software_id,arch,url,sha256,source_id) VALUES ($(SW c18),'arm64','https://e.invalid/a','${H64:1}',$SRC)"
rejects "artifacts live-unique (entry, arch)" \
  "INSERT INTO prov_software_artifacts (software_id,arch,url,sha256,source_id) VALUES ($(SW c18),'any','https://e.invalid/b','$H64',$SRC)"
accepts "deleting an entry cascades its artifact rows" "DELETE FROM prov_software WHERE logical_name='c18'"
eq "no orphan artifact rows" "SELECT COUNT(*) FROM prov_software_artifacts" 0

# ---- pool: memberships ------------------------------------------------------------
q "INSERT INTO prov_stacks (slug,name,source_id) VALUES ('tmp-a','Tmp A',$SRC)"
TMPA="(SELECT id FROM prov_stacks WHERE slug='tmp-a')"
rejects "duplicate live member (core, git)" \
  "INSERT INTO prov_stack_members (stack_id,software_id,source_id) VALUES ($CORE,$(SW git),$SRC)"
accepts "same entry in a second stack" \
  "INSERT INTO prov_stack_members (stack_id,software_id,item_order,source_id) VALUES ($TMPA,$(SW git),50,$SRC)"
accepts "member re-added once the first is soft-deleted" \
  "UPDATE prov_stack_members SET deleted_at=NOW() WHERE stack_id=$TMPA; INSERT INTO prov_stack_members (stack_id,software_id,source_id) VALUES ($TMPA,$(SW git),$SRC)"
rejects "hard-deleting an entry a stack includes (RESTRICT)" "DELETE FROM prov_software WHERE logical_name='git'"
accepts "hard-deleting a stack cascades its members" "DELETE FROM prov_stacks WHERE slug='tmp-a'"
eq "only core/dev-tools members remain" "SELECT COUNT(*) FROM prov_stack_members" 21

# ---- pool: requirements -------------------------------------------------------------
rejects "requires itself" \
  "INSERT INTO prov_software_requires (software_id,requires_software_id,source_id) VALUES ($(SW c4),$(SW c4),$SRC)"
accepts "requires edge c4 -> c5" \
  "INSERT INTO prov_software_requires (software_id,requires_software_id,source_id) VALUES ($(SW c4),$(SW c5),$SRC)"
rejects "duplicate live requires edge" \
  "INSERT INTO prov_software_requires (software_id,requires_software_id,source_id) VALUES ($(SW c4),$(SW c5),$SRC)"
rejects "hard-deleting a required entry (RESTRICT)" "DELETE FROM prov_software WHERE logical_name='c5'"
accepts "hard-deleting the dependent cascades its edges" "DELETE FROM prov_software WHERE logical_name='c4'"
eq "only the seeded requires edge remains" "SELECT COUNT(*) FROM prov_software_requires" 1

# ---- pool: conflicts (canonical lo < hi) ------------------------------------------------
q "$(pk cf1 pkg "'a'" "'a'" "'a'" NULL NULL); $(pk cf2 pkg "'b'" "'b'" "'b'" NULL NULL)"
accepts "canonical conflict pair (lo < hi)" \
  "INSERT INTO prov_software_conflicts (software_lo_id,software_hi_id,source_id) SELECT LEAST(a.id,b.id),GREATEST(a.id,b.id),$SRC FROM prov_software a, prov_software b WHERE a.logical_name='cf1' AND b.logical_name='cf2'"
rejects "non-canonical conflict pair (hi, lo)" \
  "INSERT INTO prov_software_conflicts (software_lo_id,software_hi_id,source_id) SELECT GREATEST(a.id,b.id),LEAST(a.id,b.id),$SRC FROM prov_software a, prov_software b WHERE a.logical_name='cf1' AND b.logical_name='cf2'"
rejects "conflict self-pair" \
  "INSERT INTO prov_software_conflicts (software_lo_id,software_hi_id,source_id) VALUES ($(SW cf1),$(SW cf1),$SRC)"
rejects "duplicate live conflict pair" \
  "INSERT INTO prov_software_conflicts (software_lo_id,software_hi_id,source_id) SELECT LEAST(a.id,b.id),GREATEST(a.id,b.id),$SRC FROM prov_software a, prov_software b WHERE a.logical_name='cf1' AND b.logical_name='cf2'"
accepts "hard-deleting a side cascades the pair" "DELETE FROM prov_software WHERE logical_name='cf2'"
eq "no conflict rows remain" "SELECT COUNT(*) FROM prov_software_conflicts" 0
q "DELETE FROM prov_software WHERE logical_name IN ('c1','c5','c13','cf1')"
eq "pool back to the 21 seeded rows" "SELECT COUNT(*) FROM prov_software" 21

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
q "DELETE FROM prov_stack_members WHERE software_id=(SELECT id FROM prov_software WHERE logical_name='powerlevel10k pin')"
q "DELETE FROM prov_software WHERE logical_name='powerlevel10k pin'"   # drop the soft-deleted copy (its requires edge cascades)
apply "$SOR/migrations/029_provisioning_images_stacks.sql"                # seed-once re-adds it fresh
RO_PASS="$("$PY" -c 'import secrets; print(secrets.token_hex(16))')"
"${ADMIN[@]}" -e "CREATE USER '$RO_USER'@'$RO_HOST' IDENTIFIED BY '$RO_PASS'"
user_created=1
"${ADMIN[@]}" -e "GRANT SELECT ON \`$DB\`.* TO '$RO_USER'@'$RO_HOST'"
TMP_A="$(mktemp)"; TMP_B="$(mktemp)"
dbgen() {
  SOR_DB_HOST=127.0.0.1 SOR_DB_PORT="$DB_PORT" SOR_DB_NAME="$DB" SOR_DB_USER="$RO_USER" SOR_DB_PASS="$RO_PASS" \
    "$PY" "$ROOT/deploy/fleet/stackgen.py" --from-db "$@"
}
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

# a conflict pair from the DB is enforced by the generator, naming both stacks
q "$(pk ntp pkg "'ntp'" "'ntp'" "'ntp'" NULL NULL)"
q "INSERT INTO prov_software_conflicts (software_lo_id,software_hi_id,source_id) SELECT LEAST(a.id,b.id),GREATEST(a.id,b.id),$SRC FROM prov_software a, prov_software b WHERE a.logical_name='chrony' AND b.logical_name='ntp' AND a.deleted_at IS NULL AND b.deleted_at IS NULL"
q "INSERT INTO prov_stacks (slug,name,source_id) VALUES ('timesync','Timesync',$SRC)"
q "INSERT INTO prov_stack_members (stack_id,software_id,source_id) VALUES ((SELECT id FROM prov_stacks WHERE slug='timesync'),$(SW ntp),$SRC)"
if dbgen --family suse --arch x86_64 --stack timesync > "$TMP_A" 2>"$TMP_B"; then die "DB conflict pair not enforced"; fi
grep -q "conflicting entries 'chrony' (stack 'core') and 'ntp' (stack 'timesync')" "$TMP_B" \
  || { cat "$TMP_B" >&2; die "conflict error does not name both entries and stacks"; }
ok "stackgen --from-db rejects a DB conflict pair, naming both entries and stacks"
dbgen --family suse --arch x86_64 --stack dev-tools > /dev/null || die "unrelated selection broken by the pair"
ok "stackgen --from-db: the pair does not affect selections without both sides"
q "DELETE FROM prov_stacks WHERE slug='timesync'; DELETE FROM prov_software WHERE logical_name='ntp'"
# a live membership of a soft-deleted entry is an error, never a silent drop
# (scratch rows only: seed rows stay unedited for the rollback legs below)
q "$(pk gone pkg "'gone'" "'gone'" "'gone'" NULL NULL); INSERT INTO prov_stacks (slug,name,source_id) VALUES ('tmp-b','Tmp B',$SRC)"
q "INSERT INTO prov_stack_members (stack_id,software_id,source_id) VALUES ((SELECT id FROM prov_stacks WHERE slug='tmp-b'),$(SW gone),$SRC)"
q "UPDATE prov_software SET deleted_at=NOW() WHERE logical_name='gone'"
if dbgen --family suse --arch x86_64 > "$TMP_A" 2>"$TMP_B"; then die "live member of a soft-deleted entry rendered"; fi
grep -q "'gone', which is soft-deleted" "$TMP_B" || { cat "$TMP_B" >&2; die "soft-deleted member error unclear"; }
ok "stackgen --from-db refuses a live member of a soft-deleted entry"
q "DELETE FROM prov_stacks WHERE slug='tmp-b'; DELETE FROM prov_software WHERE logical_name='gone'"

# ---- rollback -----------------------------------------------------------------------
if "${ADMIN[@]}" "$DB" < "$SOR/rollback/029-down.sql" >/dev/null 2>&1; then die "rollback accepted recorded runs"; fi
ok "rollback refuses while prov_runs holds records"
q "DROP TRIGGER trg_prov_runs_bd; DELETE FROM prov_runs; DROP PROCEDURE IF EXISTS _solarinet_rollback_m029_assert_seed_only"
q "INSERT INTO prov_software_conflicts (software_lo_id,software_hi_id,source_id) SELECT LEAST(a.id,b.id),GREATEST(a.id,b.id),$SRC FROM prov_software a, prov_software b WHERE a.logical_name='chrony' AND b.logical_name='git'"
if "${ADMIN[@]}" "$DB" < "$SOR/rollback/029-down.sql" >/dev/null 2>&1; then die "rollback accepted a conflict row"; fi
ok "rollback refuses operator conflict pairs"
q "DROP PROCEDURE IF EXISTS _solarinet_rollback_m029_assert_seed_only; DELETE FROM prov_software_conflicts"
q "INSERT INTO prov_software_requires (software_id,requires_software_id,source_id) VALUES ($(SW git),$(SW curl),$SRC)"
if "${ADMIN[@]}" "$DB" < "$SOR/rollback/029-down.sql" >/dev/null 2>&1; then die "rollback accepted an operator requires edge"; fi
ok "rollback refuses operator requires edges"
q "DROP PROCEDURE IF EXISTS _solarinet_rollback_m029_assert_seed_only; DELETE FROM prov_software_requires WHERE software_id=$(SW git)"
q "UPDATE prov_images SET netboot_verified=1 WHERE release_name='tumbleweed'"
if "${ADMIN[@]}" "$DB" < "$SOR/rollback/029-down.sql" >/dev/null 2>&1; then die "rollback accepted an edited image"; fi
ok "rollback refuses operator-edited rows"
q "DROP PROCEDURE IF EXISTS _solarinet_rollback_m029_assert_seed_only; DELETE FROM prov_images WHERE release_name='tumbleweed'"
apply "$SOR/migrations/029_provisioning_images_stacks.sql"   # re-seed the image fresh

# The guard compares exact counts + content digests, so an edit stamped in the
# seed's own second (updated_at = created_at, invisible to a timestamp check)
# and a soft delete are refused too, naming the table.
refused() {  # label, expected table — rollback must refuse and keep every prov_* table
  if "${ADMIN[@]}" "$DB" < "$SOR/rollback/029-down.sql" >/dev/null 2>"$TMP_B"; then die "rollback accepted $1"; fi
  grep -q "not the untouched 029 seed.*$2 (rows" "$TMP_B" || { cat "$TMP_B" >&2; die "rollback refused $1 for another reason"; }
  eq "rollback refuses $1 (names $2, tables kept)" \
     "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME LIKE 'prov\\_%'" 9
}
q "CREATE TABLE _bk_m029 AS SELECT id, notes, updated_at FROM prov_software WHERE logical_name='git'"
q "UPDATE prov_software SET notes='operator note', updated_at=created_at WHERE logical_name='git'"
eq "same-second edit is invisible to updated_at > created_at" \
   "SELECT updated_at > created_at FROM prov_software WHERE logical_name='git'" 0
refused "a same-second edit of a seed pool row" prov_software
q "UPDATE prov_software p JOIN _bk_m029 b ON b.id=p.id SET p.notes=b.notes, p.updated_at=b.updated_at; DROP TABLE _bk_m029"
q "UPDATE prov_stacks SET name='Core!', updated_at=created_at WHERE slug='core'"
refused "a same-second edit of a seed stack" prov_stacks
q "UPDATE prov_stacks SET name='Core', updated_at=created_at WHERE slug='core'"
q "UPDATE prov_stack_members m JOIN prov_software p ON p.id=m.software_id SET m.deleted_at=m.created_at, m.updated_at=m.created_at WHERE p.logical_name='curl'"
refused "a soft-deleted seed member" prov_stack_members
q "UPDATE prov_stack_members m JOIN prov_software p ON p.id=m.software_id SET m.deleted_at=NULL, m.updated_at=m.created_at WHERE p.logical_name='curl'"
q "UPDATE prov_images SET source_id=(SELECT id FROM sources WHERE slug<>'provisioner' ORDER BY id LIMIT 1), updated_at=created_at WHERE release_name='tumbleweed'"
refused "a seed image re-asserted by another source" prov_images
q "UPDATE prov_images SET source_id=$SRC, updated_at=created_at WHERE release_name='tumbleweed'"
q "INSERT INTO prov_stack_members (stack_id,software_id,source_id) VALUES ((SELECT id FROM prov_stacks WHERE slug='dev-tools'),$(SW git),$SRC)"
refused "an extra member row" prov_stack_members
q "DELETE m FROM prov_stack_members m JOIN prov_stacks s ON s.id=m.stack_id WHERE s.slug='dev-tools' AND m.software_id=$(SW git)"
# NULL shift: moving a value into a neighbouring NULL column serialized identically
# under bare CONCAT_WS(QUOTE()) (NULLs skipped); the 'NULL' marker must catch it.
q "UPDATE prov_images SET version=install_template, install_template=NULL, updated_at=created_at WHERE release_name='tumbleweed'"
refused "a value shifted into a neighbouring NULL column" prov_images
q "UPDATE prov_images SET install_template=version, version=NULL, updated_at=created_at WHERE release_name='tumbleweed'"
# Truncation: the guard sets a 16 MiB group_concat_max_len that the seed never
# approaches, so shrink it in a copy to prove a truncated digest input is refused.
sed 's/group_concat_max_len = 16777216/group_concat_max_len = 64/' "$SOR/rollback/029-down.sql" > "$TMP_A"
grep -q 'group_concat_max_len = 64;' "$TMP_A" || die "truncation copy not patched"
# Strict sql_mode turns the cut into ER 1260; a non-strict session only warns,
# which is what the explicit length guard is for. Procedures keep the sql_mode
# they were created under, so the prefix decides which path runs.
for mode in strict lax; do
  if [ "$mode" = strict ]; then want="cut by group_concat"; pre=""; else want="seed digest input truncated"; pre="SET SESSION sql_mode='';"; fi
  if { echo "$pre"; cat "$TMP_A"; } | "${ADMIN[@]}" "$DB" >/dev/null 2>"$TMP_B"; then die "rollback accepted a truncated digest input ($mode)"; fi
  grep -q "$want" "$TMP_B" || { cat "$TMP_B" >&2; die "truncation ($mode) refused for another reason"; }
  eq "rollback refuses a truncated digest input ($mode sql_mode, tables kept)" \
     "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME LIKE 'prov\\_%'" 9
  q "DROP PROCEDURE IF EXISTS _solarinet_rollback_m029_assert_seed_only; DROP PROCEDURE IF EXISTS _solarinet_rollback_m029_seed_digests"
done

# The header's recompute command must reproduce the constants in the file.
{ sed '/^-- RECOMPUTE STOP/q' "$SOR/rollback/029-down.sql"
  echo 'CALL _solarinet_rollback_m029_seed_digests();'
  echo 'SELECT tbl, n, digest FROM _solarinet_m029_seed_actual ORDER BY tbl;'
  echo 'DROP PROCEDURE _solarinet_rollback_m029_seed_digests;'
  echo 'DROP PROCEDURE _solarinet_rollback_m029_assert_seed_only;'
} | "${ADMIN[@]}" -N -B "$DB" > "$TMP_A" || die "digest recompute command"
"$PY" - "$TMP_A" "$SOR/rollback/029-down.sql" <<'PYCHK' || die "recomputed seed digests differ from the 029-down.sql constants"
import re, sys
got = {l.split("\t")[0]: tuple(l.split("\t")[1:]) for l in open(sys.argv[1]).read().splitlines()}
src = open(sys.argv[2]).read()
want = {m[0]: (m[1], m[2]) for m in re.findall(r"SELECT '(prov_[a-z_]+)'(?: AS tbl)?, (\d+)(?: AS n)?, '([0-9a-f]{64})'", src)}
assert len(want) == 6 and got == want, (got, want)
PYCHK
ok "documented recompute command reproduces the 6 seed count/digest constants"

apply "$SOR/rollback/029-down.sql"
eq "rollback removed all prov_* tables" "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME LIKE 'prov\\_%'" 0
eq "rollback removed prov_runs triggers" "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME LIKE 'trg_prov\\_runs%'" 0
eq "rollback left no helper procedure" "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA=DATABASE() AND ROUTINE_NAME LIKE '%m029%'" 0
eq "rollback left 021 provisioner source intact" "SELECT COUNT(*) FROM sources WHERE slug='provisioner'" 1

apply "$SOR/migrations/029_provisioning_images_stacks.sql"
ok "029 re-applies after rollback (forward recovery)"
seed_counts "after forward recovery"

echo "PASS: $pass checks"
