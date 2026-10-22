#!/usr/bin/env bash
# Exercise the real importer with its narrowly scoped database privileges.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/require_scratch_sor_db.sh"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
test_user=solarinet_unifi_import
# The database container sees the runner as its bridge gateway, not the
# runner's loopback address. This wildcard exists only in the disposable DB.
test_host=%
test_password=solarinet_unifi_ci_fixture
grant_file="$(mktemp)"
cleanup() {
  rm -f "$grant_file"
  "${MYSQL[@]}" -e "DROP USER IF EXISTS '$test_user'@'$test_host'" >/dev/null 2>&1 || true
}
trap cleanup EXIT

if ! grep -q "'$test_user'@'10.0.0.20'" "$repo_root/deploy/unifi/sor-unifi-grants.sql"; then
  echo "Refusing grant test: production grants template host changed unexpectedly." >&2
  exit 2
fi
sed -e "s/\`sor\`./\`$DB\`./g" \
    -e "s/'10.0.0.20'/'$test_host'/g" \
  "$repo_root/deploy/unifi/sor-unifi-grants.sql" > "$grant_file"
if grep -Eq "10.0.0.20|\`sor\`" "$grant_file"; then
  echo "Refusing grant test: grants template target was not fully rewritten." >&2
  exit 2
fi

"${MYSQL[@]}" -e "DROP USER IF EXISTS '$test_user'@'$test_host'; CREATE USER '$test_user'@'$test_host' IDENTIFIED BY '$test_password'"
"${MYSQL[@]}" < "$grant_file"

SOR_TEST_DB="$DB" SOR_TEST_HOST="$MYSQL_HOST" SOR_TEST_PORT="$MYSQL_PORT" \
SOR_TEST_USER="$test_user" SOR_TEST_PASSWORD="$test_password" \
PYTHONPATH="$repo_root/deploy/unifi:$repo_root/netdb/sor" \
python3 - <<'PY'
import os
import pymysql
import unifi_network_sor as importer

site = {"id": "ci-site", "name": "CI Site"}
detail = {
    "id": "ci-network", "name": "CI Import Fixture", "vlanId": 3201,
    "enabled": True, "management": "UNMANAGED", "isDefault": False,
    "ipv4Configuration": {
        "hostIpAddress": "198.51.100.1", "prefixLength": 24,
        "dhcpConfiguration": {
            "mode": "SERVER", "leaseTimeSeconds": 3600,
            "ipAddressRange": {"start": "198.51.100.100", "stop": "198.51.100.200"},
        },
    },
}
row = importer.normalize_network(site, detail)
snapshot = {"site_id": "ci-site", "networks": [row]}
connection = pymysql.connect(
    host=os.environ["SOR_TEST_HOST"], port=int(os.environ["SOR_TEST_PORT"]),
    user=os.environ["SOR_TEST_USER"], password=os.environ["SOR_TEST_PASSWORD"],
    database=os.environ["SOR_TEST_DB"], charset="utf8mb4", autocommit=False)
try:
    importer.apply_snapshots(connection, [snapshot])
    row = importer.normalize_network(site, detail)
    importer.apply_snapshots(connection, [{"site_id": "ci-site", "networks": [row]}])
    cur = connection.cursor()
    cur.execute("SELECT allocator_eligible FROM networks WHERE name='CI Import Fixture' AND deleted_at IS NULL")
    result = cur.fetchone()
    assert result == (0,), f"expected imported network to remain ineligible, got {result!r}"
    cur.execute("SELECT COUNT(*) FROM external_refs WHERE external_id LIKE 'ci-site:%' AND deleted_at IS NULL")
    assert cur.fetchone()[0] == 4, "expected network, VLAN, subnet, and DHCP references"
    connection.commit()
finally:
    connection.close()
print("UniFi importer least-privilege apply/replay passed.")
PY

actual="$("${MYSQL[@]}" -e "SELECT COUNT(*) FROM information_schema.TABLE_PRIVILEGES WHERE GRANTEE=\"'$test_user'@'$test_host'\" AND TABLE_SCHEMA=DATABASE()")"
[[ "$actual" == 17 ]] || { echo "Expected 17 table privileges, found $actual." >&2; exit 1; }
global="$("${MYSQL[@]}" -e "SELECT COUNT(*) FROM information_schema.USER_PRIVILEGES WHERE GRANTEE=\"'$test_user'@'$test_host'\" AND PRIVILEGE_TYPE <> 'USAGE'")"
[[ "$global" == 0 ]] || { echo "Unexpected global privileges: $global." >&2; exit 1; }
echo "UniFi importer grant matrix passed: 17 table privileges, no global privileges."
