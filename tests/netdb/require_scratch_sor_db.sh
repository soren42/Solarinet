#!/usr/bin/env bash
# Shared fail-closed identity check for integration tests that write SoR fixtures.
set -euo pipefail

DB="${MYSQL_DATABASE:-}"
MYSQL_HOST="${MYSQL_HOST:-${SOR_DB_HOST:-}}"
MYSQL_PORT="${MYSQL_PORT:-${SOR_DB_PORT:-3306}}"
MYSQL_USER="${MYSQL_USER:-root}"
if [[ "${SOLARI_TEST_SOR_DB:-}" != "1" || "$MYSQL_HOST" != "127.0.0.1" \
      || -z "$DB" || "$DB" != "${SOR_DB_NAME:-$DB}" ]]; then
  echo "Refusing SoR fixtures: explicit loopback scratch database opt-in is required." >&2
  exit 2
fi
case "$DB" in sor_test|solarinet_stage|solarinet_sorsync_ci) ;; *) echo "Refusing non-scratch schema: $DB" >&2; exit 2 ;; esac
MYSQL=(mariadb --protocol=TCP --host="$MYSQL_HOST" --port="$MYSQL_PORT" --user="$MYSQL_USER"
       --database="$DB" --batch --skip-column-names --silent)
if [[ -n "${MYSQL_PWD:-}" ]]; then MYSQL+=("--password=$MYSQL_PWD"); fi
marker="$("${MYSQL[@]}" -e "SELECT marker FROM _solarinet_test_scratch WHERE marker='solarinet-ci-scratch-v1'")"
if [[ "$marker" != "solarinet-ci-scratch-v1" ]]; then
  echo "Refusing SoR fixtures: scratch sentinel missing from '$DB'." >&2
  exit 2
fi
