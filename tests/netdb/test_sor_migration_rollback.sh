#!/usr/bin/env bash
# Rehearse the manual 024-027 rollback and forward recovery in an explicit
# loopback scratch schema. This test must never be pointed at a live SoR.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/require_scratch_sor_db.sh"

rollback_sql="$(dirname "${BASH_SOURCE[0]}")/../../netdb/sor/rollback/024-027-down.sql"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

query() { "${MYSQL[@]}" -e "$1"; }
expect_zero() {
  local label="$1" value="$2"
  if [[ "$value" != 0 ]]; then
    echo "Refusing rollback rehearsal: $label has $value rows/objects." >&2
    exit 2
  fi
}

# Values added by these migrations make structural rollback destructive.
network_values="$(query "SELECT COUNT(*) FROM networks WHERE enabled <> 1 OR management IS NOT NULL OR is_default <> 0 OR allocator_eligible <> 0 OR allocator_approved_by IS NOT NULL OR allocator_approved_at IS NOT NULL")"
cable_values="$(query "SELECT COUNT(*) FROM interconnects WHERE media_standard IS NOT NULL OR jacket_color IS NOT NULL")"
expect_zero "network state" "$network_values"
expect_zero "cable metadata" "$cable_values"

# Ensure this really is a post-migration scratch schema before dropping anything.
candidate_columns="$(query "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND ((TABLE_NAME='networks' AND COLUMN_NAME IN ('enabled','management','is_default','allocator_eligible','allocator_approved_by','allocator_approved_at')) OR (TABLE_NAME='interconnects' AND COLUMN_NAME IN ('media_standard','jacket_color')))")"
if [[ "$candidate_columns" != 8 ]]; then
  echo "Expected all eight candidate columns before rollback, found $candidate_columns." >&2
  exit 2
fi

"${MYSQL[@]}" < "$rollback_sql"
remaining_columns="$(query "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND ((TABLE_NAME='networks' AND COLUMN_NAME IN ('enabled','management','is_default','allocator_eligible','allocator_approved_by','allocator_approved_at')) OR (TABLE_NAME='interconnects' AND COLUMN_NAME IN ('media_standard','jacket_color')))")"
remaining_triggers="$(query "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND (TRIGGER_NAME LIKE 'trg_sources_%' OR TRIGGER_NAME LIKE 'trg_functions_%' OR TRIGGER_NAME LIKE 'trg_entity_functions_%' OR TRIGGER_NAME LIKE 'trg_function_incompatibility_%' OR TRIGGER_NAME LIKE 'trg_vlans_%' OR TRIGGER_NAME LIKE 'trg_dhcp_ranges_%' OR TRIGGER_NAME LIKE 'trg_external_refs_%')")"
remaining_allocator_index="$(query "SELECT COUNT(DISTINCT INDEX_NAME) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='networks' AND INDEX_NAME='ix_networks_allocator'")"
prior_cdc="$(query "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME IN ('trg_entities_ai','trg_entities_au','trg_entities_ad')")"
[[ "$remaining_columns" == 0 && "$remaining_triggers" == 0 && "$remaining_allocator_index" == 0 && "$prior_cdc" == 3 ]] || {
  echo "Rollback postcondition failed (columns=$remaining_columns triggers=$remaining_triggers index=$remaining_allocator_index prior_cdc=$prior_cdc)." >&2
  exit 1
}

# Restore the exact candidate by reapplying the immutable forward SQL. The
# ledger remains unchanged because this rehearsal is confined to scratch.
for migration in 024_provisioning_cdc.sql 025_network_allocator_gate.sql \
                 026_unifi_network_cdc.sql 027_interconnect_cable_details.sql; do
  "${MYSQL[@]}" < "$repo_root/netdb/sor/migrations/$migration"
done
restored_columns="$(query "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND ((TABLE_NAME='networks' AND COLUMN_NAME IN ('enabled','management','is_default','allocator_eligible','allocator_approved_by','allocator_approved_at')) OR (TABLE_NAME='interconnects' AND COLUMN_NAME IN ('media_standard','jacket_color')))")"
restored_triggers="$(query "SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE() AND (TRIGGER_NAME LIKE 'trg_sources_%' OR TRIGGER_NAME LIKE 'trg_functions_%' OR TRIGGER_NAME LIKE 'trg_entity_functions_%' OR TRIGGER_NAME LIKE 'trg_function_incompatibility_%' OR TRIGGER_NAME LIKE 'trg_vlans_%' OR TRIGGER_NAME LIKE 'trg_dhcp_ranges_%' OR TRIGGER_NAME LIKE 'trg_external_refs_%')")"
allocator_index="$(query "SELECT COUNT(DISTINCT INDEX_NAME) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='networks' AND INDEX_NAME='ix_networks_allocator'")"
[[ "$restored_columns" == 8 && "$restored_triggers" == 21 && "$allocator_index" == 1 ]] || {
  echo "Forward recovery postcondition failed (columns=$restored_columns triggers=$restored_triggers index=$allocator_index)." >&2
  exit 1
}

echo "SoR migration rollback and forward recovery rehearsal passed in $DB."
