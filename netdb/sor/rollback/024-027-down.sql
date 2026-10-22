-- Manual rollback for the unpromoted 024-027 provisioning candidate.
-- This file intentionally lives outside migrations/ so the forward-only
-- migration runner cannot discover or execute it.
--
-- Before use: stop the UniFi importer, allocator, and RackWire SoR writer;
-- take and verify a fresh SoR backup; run every preflight in README.md and
-- require zero rows for every destructive-data check. MariaDB DDL autocommits;
-- this is not a transactional rollback. Apply only on Cesium, never Benzene.

-- 027: cable metadata. Preflight requires both columns to contain no values.
ALTER TABLE `interconnects`
  DROP COLUMN IF EXISTS `jacket_color`,
  DROP COLUMN IF EXISTS `media_standard`;

-- 026: CDC for imported network facts.
DROP TRIGGER IF EXISTS `trg_external_refs_ad`;
DROP TRIGGER IF EXISTS `trg_external_refs_au`;
DROP TRIGGER IF EXISTS `trg_external_refs_ai`;
DROP TRIGGER IF EXISTS `trg_dhcp_ranges_ad`;
DROP TRIGGER IF EXISTS `trg_dhcp_ranges_au`;
DROP TRIGGER IF EXISTS `trg_dhcp_ranges_ai`;
DROP TRIGGER IF EXISTS `trg_vlans_ad`;
DROP TRIGGER IF EXISTS `trg_vlans_au`;
DROP TRIGGER IF EXISTS `trg_vlans_ai`;

-- 025: allocator gate and source facts. Preflight requires no non-default
-- imported/approved values; dropping these fields otherwise loses that state.
ALTER TABLE `networks`
  DROP INDEX IF EXISTS `ix_networks_allocator`,
  DROP COLUMN IF EXISTS `allocator_approved_at`,
  DROP COLUMN IF EXISTS `allocator_approved_by`,
  DROP COLUMN IF EXISTS `allocator_eligible`,
  DROP COLUMN IF EXISTS `is_default`,
  DROP COLUMN IF EXISTS `management`,
  DROP COLUMN IF EXISTS `enabled`;

-- 024: CDC for provisioning taxonomy/source records.
DROP TRIGGER IF EXISTS `trg_function_incompatibility_ad`;
DROP TRIGGER IF EXISTS `trg_function_incompatibility_au`;
DROP TRIGGER IF EXISTS `trg_function_incompatibility_ai`;
DROP TRIGGER IF EXISTS `trg_entity_functions_ad`;
DROP TRIGGER IF EXISTS `trg_entity_functions_au`;
DROP TRIGGER IF EXISTS `trg_entity_functions_ai`;
DROP TRIGGER IF EXISTS `trg_functions_ad`;
DROP TRIGGER IF EXISTS `trg_functions_au`;
DROP TRIGGER IF EXISTS `trg_functions_ai`;
DROP TRIGGER IF EXISTS `trg_sources_ad`;
DROP TRIGGER IF EXISTS `trg_sources_au`;
DROP TRIGGER IF EXISTS `trg_sources_ai`;
