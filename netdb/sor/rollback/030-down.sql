-- Manual rollback for 030_provisioning_cdc.sql.
-- This file intentionally lives outside migrations/ so the forward-only
-- migration runner cannot discover or execute it.
--
-- Drops exactly the 27 CDC triggers 030 created (trg_<prov table>_ai/_au/_ad)
-- and nothing else: no tables, rows, or 029 guard triggers
-- (trg_prov_runs_bu / trg_prov_runs_bd are untouched). Idempotent. Outbox rows
-- already written stay in sor_outbox and are still published by sor_emitd.
-- Apply as the DB root account on Cesium only, never Benzene. Afterwards
-- remove the 030 row from the migration ledger (tools/migrate.py) by hand.
-- The extra DEFAULT_OPTIONAL_TABLES names in sor_emitd are harmless to leave.
DROP TRIGGER IF EXISTS `trg_prov_images_ad`;
DROP TRIGGER IF EXISTS `trg_prov_images_au`;
DROP TRIGGER IF EXISTS `trg_prov_images_ai`;
DROP TRIGGER IF EXISTS `trg_prov_software_ad`;
DROP TRIGGER IF EXISTS `trg_prov_software_au`;
DROP TRIGGER IF EXISTS `trg_prov_software_ai`;
DROP TRIGGER IF EXISTS `trg_prov_software_artifacts_ad`;
DROP TRIGGER IF EXISTS `trg_prov_software_artifacts_au`;
DROP TRIGGER IF EXISTS `trg_prov_software_artifacts_ai`;
DROP TRIGGER IF EXISTS `trg_prov_software_requires_ad`;
DROP TRIGGER IF EXISTS `trg_prov_software_requires_au`;
DROP TRIGGER IF EXISTS `trg_prov_software_requires_ai`;
DROP TRIGGER IF EXISTS `trg_prov_software_conflicts_ad`;
DROP TRIGGER IF EXISTS `trg_prov_software_conflicts_au`;
DROP TRIGGER IF EXISTS `trg_prov_software_conflicts_ai`;
DROP TRIGGER IF EXISTS `trg_prov_stacks_ad`;
DROP TRIGGER IF EXISTS `trg_prov_stacks_au`;
DROP TRIGGER IF EXISTS `trg_prov_stacks_ai`;
DROP TRIGGER IF EXISTS `trg_prov_stack_members_ad`;
DROP TRIGGER IF EXISTS `trg_prov_stack_members_au`;
DROP TRIGGER IF EXISTS `trg_prov_stack_members_ai`;
DROP TRIGGER IF EXISTS `trg_prov_stack_deps_ad`;
DROP TRIGGER IF EXISTS `trg_prov_stack_deps_au`;
DROP TRIGGER IF EXISTS `trg_prov_stack_deps_ai`;
DROP TRIGGER IF EXISTS `trg_prov_runs_ad`;
DROP TRIGGER IF EXISTS `trg_prov_runs_au`;
DROP TRIGGER IF EXISTS `trg_prov_runs_ai`;
