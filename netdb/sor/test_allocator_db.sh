#!/usr/bin/env bash
# ===========================================================================
# test_allocator_db.sh — stand up a SCRATCH MariaDB, prove the allocator's
# concurrency guarantee, then tear it down. NEVER touches live `sor`.
#
# Builds an ephemeral `sor_test` database from the real DDL (schema.sql), the
# CDC outbox tables (01-outbox.sql, so 021's shape matches production), and
# migration 021, then runs test_allocator_db.py against it and drops it.
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
PYBIN="$HERE/.venv/bin/python3"

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

created=0
cleanup() {
  # Always runs (success, failure, or Ctrl-C). Only tears down what we created.
  if [ "$created" = "1" ]; then
    echo "== tearing down $DB =="
    sudo mariadb <<SQL
DROP DATABASE IF EXISTS \`$DB\`;
DROP USER IF EXISTS 'sor_test'@'127.0.0.1';
SQL
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

echo "== loading CDC outbox (01-outbox.sql) =="
sudo mariadb "$DB" < "$HERE/../../deploy/sorsync/sql/01-outbox.sql"

echo "== applying migration 021 =="
sudo mariadb "$DB" < "$HERE/migrations/021_provisioning_taxonomy.sql"

# A dedicated scratch login so the Python driver connects over TCP like prod,
# not the root unix socket. Password is ephemeral and local-only by design.
echo "== creating scratch login =="
sudo mariadb <<SQL
CREATE USER IF NOT EXISTS 'sor_test'@'127.0.0.1' IDENTIFIED BY 'sor_test_ephemeral';
GRANT ALL PRIVILEGES ON \`$DB\`.* TO 'sor_test'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL

echo "== running concurrency proof =="
set +e
SOR_TEST_DB="$DB" "$PYBIN" "$HERE/test_allocator_db.py"
rc=$?
set -e

# Teardown handled by the EXIT trap.
exit $rc
