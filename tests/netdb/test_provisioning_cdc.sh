#!/usr/bin/env bash
# Exercise migration 022 against a scratch MariaDB schema. All fixture changes
# and the outbox rows are rolled back together at the end of the test.
set -euo pipefail

DB="${MYSQL_DATABASE:-solarinet_stage}"
MYSQL_HOST="${MYSQL_HOST:-${SOR_DB_HOST:-127.0.0.1}}"
MYSQL_PORT="${MYSQL_PORT:-${SOR_DB_PORT:-3306}}"
MYSQL_USER="${MYSQL_USER:-root}"
source "$(dirname "${BASH_SOURCE[0]}")/require_scratch_sor_db.sh"
MYSQL=(mariadb --host="$MYSQL_HOST" --port="$MYSQL_PORT" --user="$MYSQL_USER"
       --database="$DB" --batch --skip-column-names --silent)
if [[ -n "${MYSQL_PWD:-}" ]]; then MYSQL+=("--password=$MYSQL_PWD"); fi

actual="$("${MYSQL[@]}" <<'SQL'
START TRANSACTION;
INSERT INTO sources (slug, display_name, kind, endpoint)
VALUES (CONCAT('cdc-test-', CONNECTION_ID()), 'CDC test', 'sync', 'test://cdc');
SET @source_id = LAST_INSERT_ID();
INSERT INTO functions (slug, display_name, source_id)
VALUES (CONCAT('cdc-test-a-', CONNECTION_ID()), 'CDC test A', @source_id);
SET @function_a = LAST_INSERT_ID();
INSERT INTO functions (slug, display_name, source_id)
VALUES (CONCAT('cdc-test-b-', CONNECTION_ID()), 'CDC test B', @source_id);
SET @function_b = LAST_INSERT_ID();
INSERT INTO entities (name, entity_type, source_id)
VALUES (CONCAT('cdc-test-', CONNECTION_ID()), 'server', @source_id);
SET @entity_id = LAST_INSERT_ID();
INSERT INTO entity_functions (entity_id, function_id, is_primary, source_id)
VALUES (@entity_id, @function_a, 1, @source_id);
SET @membership_id = LAST_INSERT_ID();
INSERT INTO function_incompatibility (function_lo_id, function_hi_id, reason, source_id)
VALUES (@function_a, @function_b, 'CDC integration test', @source_id);
SET @incompatibility_id = LAST_INSERT_ID();

UPDATE functions SET description='updated' WHERE id=@function_a;
UPDATE entity_functions SET notes='updated' WHERE id=@membership_id;
UPDATE function_incompatibility SET reason='updated' WHERE id=@incompatibility_id;
UPDATE sources SET display_name='CDC test updated' WHERE id=@source_id;

DELETE FROM function_incompatibility WHERE id=@incompatibility_id;
DELETE FROM entity_functions WHERE id=@membership_id;
DELETE FROM entities WHERE id=@entity_id;
DELETE FROM functions WHERE id IN (@function_a, @function_b);
DELETE FROM sources WHERE id=@source_id;

SELECT IF(
  (SELECT COUNT(*) FROM sor_outbox WHERE table_name='sources' AND row_id=@source_id AND action='insert')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='sources' AND row_id=@source_id AND action='update')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='sources' AND row_id=@source_id AND action='delete')=1
  AND
  (SELECT COUNT(*) FROM sor_outbox WHERE table_name='functions' AND row_id IN (@function_a,@function_b) AND action='insert')=2
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='functions' AND row_id=@function_a AND action='update')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='functions' AND row_id IN (@function_a,@function_b) AND action='delete')=2
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='entity_functions' AND row_id=@membership_id AND action='insert')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='entity_functions' AND row_id=@membership_id AND action='update')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='entity_functions' AND row_id=@membership_id AND action='delete')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='function_incompatibility' AND row_id=@incompatibility_id AND action='insert')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='function_incompatibility' AND row_id=@incompatibility_id AND action='update')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='function_incompatibility' AND row_id=@incompatibility_id AND action='delete')=1,
  'PASS', 'FAIL');
ROLLBACK;
SQL
)"

if [[ "$actual" != "PASS" ]]; then
  echo "provisioning CDC integration failed: expected PASS, got '$actual'" >&2
  exit 1
fi
echo "provisioning CDC integration passed (insert/update/delete for sources and mutable taxonomy tables; rolled back)"
