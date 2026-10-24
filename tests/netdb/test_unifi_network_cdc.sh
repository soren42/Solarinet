#!/usr/bin/env bash
# Verify imported VLAN, DHCP, and external-reference rows publish I/U/D events.
# The fixture and its outbox rows are rolled back in the scratch database.
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
SELECT id INTO @source_id FROM sources WHERE slug='unifi' AND deleted_at IS NULL LIMIT 1;
SET @tag = 4094;
INSERT INTO vlans (vlan_tag,name,source_id) VALUES (@tag,CONCAT('cdc-vlan-',CONNECTION_ID()),@source_id);
SET @vlan_id=LAST_INSERT_ID();
INSERT INTO networks (name,purpose,vlan_id,source_id)
VALUES (CONCAT('cdc-network-',CONNECTION_ID()),'CDC fixture',@vlan_id,@source_id);
SET @network_id=LAST_INSERT_ID();
INSERT INTO subnets (network_id,cidr,net_address,prefix_len,gateway_ip,source_id)
VALUES (@network_id,'192.0.2.0/24',CAST('192.0.2.0' AS INET6),24,CAST('192.0.2.1' AS INET6),@source_id);
SET @subnet_id=LAST_INSERT_ID();
INSERT INTO dhcp_ranges (subnet_id,range_start,range_end,kind,source_id)
VALUES (@subnet_id,CAST('192.0.2.100' AS INET6),CAST('192.0.2.200' AS INET6),'dynamic_pool',@source_id);
SET @dhcp_id=LAST_INSERT_ID();
INSERT INTO external_refs (source_id,subject_table,subject_id,external_id)
VALUES (@source_id,'networks',@network_id,CONCAT('cdc-test:',CONNECTION_ID()));
SET @ref_id=LAST_INSERT_ID();

UPDATE vlans SET name=CONCAT(name,'-updated') WHERE id=@vlan_id;
UPDATE dhcp_ranges SET notes='updated' WHERE id=@dhcp_id;
UPDATE external_refs SET external_url='https://example.invalid/cdc-test' WHERE id=@ref_id;

DELETE FROM external_refs WHERE id=@ref_id;
DELETE FROM dhcp_ranges WHERE id=@dhcp_id;
DELETE FROM subnets WHERE id=@subnet_id;
DELETE FROM networks WHERE id=@network_id;
DELETE FROM vlans WHERE id=@vlan_id;

SELECT IF(
  (SELECT COUNT(*) FROM sor_outbox WHERE table_name='vlans' AND row_id=@vlan_id AND action='insert')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='vlans' AND row_id=@vlan_id AND action='update')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='vlans' AND row_id=@vlan_id AND action='delete')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='dhcp_ranges' AND row_id=@dhcp_id AND action='insert')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='dhcp_ranges' AND row_id=@dhcp_id AND action='update')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='dhcp_ranges' AND row_id=@dhcp_id AND action='delete')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='external_refs' AND row_id=@ref_id AND action='insert')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='external_refs' AND row_id=@ref_id AND action='update')=1
  AND (SELECT COUNT(*) FROM sor_outbox WHERE table_name='external_refs' AND row_id=@ref_id AND action='delete')=1,
  'UniFi network CDC integration passed', 'FAIL: UniFi network CDC event missing');
ROLLBACK;
SQL
)"

[[ "$actual" == "UniFi network CDC integration passed" ]] || {
  echo "$actual" >&2
  exit 1
}
echo "$actual"
