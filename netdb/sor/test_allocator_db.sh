#!/usr/bin/env bash
# ===========================================================================
# test_allocator_db.sh — stand up a SCRATCH MariaDB, prove the allocator's
# concurrency guarantee, then tear it down. NEVER touches live `sor`.
#
# Builds an ephemeral `sor_test` database from the real DDL (schema.sql), the
# CDC outbox tables and core triggers, migrations 020–028, and provisioning /
# UniFi integration fixtures, then runs the allocator proof and drops the
# schema.
#
# Requires: passwordless `sudo mariadb` (root via unix_socket, as on this dev
# box) and the sor venv python with pymysql (netdb/sor/.venv).
#
# Usage:  netdb/sor/test_allocator_db.sh
# Exit:   0 = concurrency proof passed; non-zero otherwise.
# ===========================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DB="sor_test"
PYBIN="${SOR_TEST_PYTHON:-$HERE/.venv/bin/python3}"
DASH_USER="sor_dash_ci_$$"

[ -x "$PYBIN" ] || { echo "missing sor venv python at $PYBIN" >&2; exit 2; }

# Never clobber a pre-existing database of this name — it might be a real dev DB.
# We only ever operate on a scratch DB WE create, and the EXIT trap only tears
# down once we have taken ownership (created == 1).
if sudo mariadb -N -B -e \
    "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='$DB'" \
    | grep -q .; then
  echo "refusing to run: database '$DB' already exists — drop it yourself if it is scratch" >&2
  exit 2
fi

# Fixture accounts are global MariaDB principals. Never take ownership of an
# account another local test or service may already use, even when the schema
# name itself is free.
existing_fixture_users="$(sudo mariadb -N -B -e \
    "SELECT CONCAT(User, '@', Host) FROM mysql.user WHERE (User='sor_test' AND Host='127.0.0.1') OR (User='$DASH_USER' AND Host='%')")"
if [ -n "$existing_fixture_users" ]; then
  echo "refusing to run: fixture account already exists: $existing_fixture_users" >&2
  exit 2
fi

created=0
sor_user_created=0
dashboard_user_created=0
cleanup() {
  # Always runs (success, failure, or Ctrl-C). Only tears down what we created.
  if [ "$created" = "1" ]; then
    echo "== tearing down $DB =="
    sudo mariadb <<SQL
DROP DATABASE IF EXISTS \`$DB\`;
SQL
    if [ "$sor_user_created" = "1" ]; then
      sudo mariadb -e "DROP USER IF EXISTS 'sor_test'@'127.0.0.1';"
    fi
    if [ "$dashboard_user_created" = "1" ]; then
      sudo mariadb -e "DROP USER IF EXISTS '$DASH_USER'@'%';"
    fi
  fi
}
trap cleanup EXIT

echo "== creating $DB =="
sudo mariadb <<SQL
CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
SQL
created=1

echo "== loading schema.sql =="
sudo mariadb "$DB" < "$HERE/schema.sql"

echo "== applying inventory and barcode migrations 014-015 =="
sudo mariadb "$DB" < "$HERE/migrations/014_inventory.sql"
sudo mariadb "$DB" < "$HERE/migrations/015_barcodes.sql"

echo "== applying RackWire migration 020 =="
sudo mariadb "$DB" < "$HERE/migrations/020_rackwire.sql"

echo "== applying migration 021 =="
sudo mariadb "$DB" < "$HERE/migrations/021_provisioning_taxonomy.sql"

echo "== applying migration 022 (transactional outbox) =="
sudo mariadb "$DB" < "$HERE/migrations/022_transactional_outbox.sql"

echo "== applying migration 023 (core SoR CDC) =="
sudo mariadb "$DB" < "$HERE/migrations/023_sor_cdc_triggers.sql"

echo "== applying migration 024 (provisioning taxonomy CDC) =="
sudo mariadb "$DB" < "$HERE/migrations/024_provisioning_cdc.sql"

echo "== applying migration 025 (network allocator gate) =="
sudo mariadb "$DB" < "$HERE/migrations/025_network_allocator_gate.sql"
sudo mariadb "$DB" < "$HERE/migrations/025_network_allocator_gate.sql"

echo "== applying migration 026 (UniFi network CDC) =="
sudo mariadb "$DB" < "$HERE/migrations/026_unifi_network_cdc.sql"
sudo mariadb "$DB" < "$HERE/migrations/026_unifi_network_cdc.sql"

echo "== applying migration 027 (RackWire cable details) =="
sudo mariadb "$DB" < "$HERE/migrations/027_interconnect_cable_details.sql"
sudo mariadb "$DB" < "$HERE/migrations/027_interconnect_cable_details.sql"
echo "== applying migration 028 (entity-to-unit association) =="
sudo mariadb "$DB" < "$HERE/migrations/028_entity_unit_association.sql"
sudo mariadb "$DB" < "$HERE/migrations/028_entity_unit_association.sql"

echo "== marking the schema as disposable scratch state =="
sudo mariadb "$DB" -e \
  "CREATE TABLE _solarinet_test_scratch (marker VARCHAR(64) PRIMARY KEY); INSERT INTO _solarinet_test_scratch VALUES ('solarinet-ci-scratch-v1')"

# A dedicated scratch login so the Python driver connects over TCP like prod,
# not the root unix socket. Password is ephemeral and local-only by design.
echo "== creating scratch login =="
sudo mariadb -e "CREATE USER 'sor_test'@'127.0.0.1' IDENTIFIED BY 'sor_test_ephemeral';"
sor_user_created=1
sudo mariadb -e "GRANT ALL PRIVILEGES ON \`$DB\`.* TO 'sor_test'@'127.0.0.1';"

echo "== exercising restricted dashboard SoR grant matrix =="
SOR_DASHBOARD_TEST_DB="$DB" SOR_DASHBOARD_TEST_USER="$DASH_USER" \
SOR_DASHBOARD_TEST_USER_HOST=% \
  SOR_DASHBOARD_TEST_ALLOW_CREATE_USER=1 SOR_DASHBOARD_ADMIN_HOST=127.0.0.1 \
  SOR_DASHBOARD_ADMIN_SOCKET=1 SOR_DASHBOARD_TEST_KEEP_USER=1 \
  "$PYBIN" "$HERE/../../tests/deploy/test_sor_dashboard_grants.py"
dashboard_user_created=1

echo "== exercising provisioning CDC triggers =="
SOLARI_TEST_SOR_DB=1 SOR_DB_NAME="$DB" MYSQL_HOST=127.0.0.1 MYSQL_PORT=3306 \
  MYSQL_USER=sor_test MYSQL_PWD=sor_test_ephemeral MYSQL_DATABASE="$DB" \
  bash "$HERE/../../tests/netdb/test_provisioning_cdc.sh"
SOLARI_TEST_SOR_DB=1 SOR_DB_NAME="$DB" MYSQL_HOST=127.0.0.1 MYSQL_PORT=3306 \
  MYSQL_USER=sor_test MYSQL_PWD=sor_test_ephemeral MYSQL_DATABASE="$DB" \
  bash "$HERE/../../tests/netdb/test_unifi_network_cdc.sh"

echo "== exercising UniFi-to-SoR network import =="
SOR_TEST_DB="$DB" "$PYBIN" -m unittest \
  "$HERE/../../deploy/unifi/test_unifi_network_sor.py" -v

echo "== exercising dashboard SoR network review against scratch MariaDB =="
NA_REAL_DB_TEST=1 SOR_DB_HOST=127.0.0.1 SOR_DB_NAME="$DB" SOR_DB_USER="$DASH_USER" \
  SOR_DB_PASS=sor_dash_test_ephemeral php "$HERE/../../tests/dashboard/test_network_allocator_api.php"

echo "== exercising RackWire commit with restricted dashboard grants =="
RW_REAL_DB_TEST=1 RW_TEST_DB="$DB" SOR_DB_HOST=127.0.0.1 SOR_DB_NAME="$DB" SOR_DB_USER="$DASH_USER" \
  SOR_DB_PASS=sor_dash_test_ephemeral RW_FIXTURE_DB_USER=sor_test \
  RW_FIXTURE_DB_PASS=sor_test_ephemeral php "$HERE/../../tests/dashboard/test_rackwire_commit_db.php"

echo "== running concurrency proof =="
set +e
SOR_TEST_DB="$DB" "$PYBIN" "$HERE/test_allocator_db.py"
rc=$?
set -e

# Teardown handled by the EXIT trap.
exit $rc
