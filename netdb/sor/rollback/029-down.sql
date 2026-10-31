-- Manual rollback for 029_provisioning_images_stacks.sql.
-- This file intentionally lives outside migrations/ so the forward-only
-- migration runner cannot discover or execute it.
--
-- Before use: stop anything writing prov_* (dashboard Provisioning API, P3+),
-- take and verify a fresh SoR backup. MariaDB DDL autocommits; this is not a
-- transactional rollback. Apply only on Cesium, never Benzene. Afterwards
-- remove the 029 row from the migration ledger (tools/migrate.py) by hand.
--
-- Preflight: refuses unless the prov_* tables hold nothing but the untouched
-- 029 seed. Any run recorded, any operator-added/edited/soft-deleted row, or
-- any row from a source other than 'provisioner' blocks the rollback, because
-- dropping the tables would destroy that state.
DROP PROCEDURE IF EXISTS `_solarinet_rollback_m029_assert_seed_only`;
DELIMITER //
CREATE PROCEDURE `_solarinet_rollback_m029_assert_seed_only`()
BEGIN
  DECLARE prov SMALLINT UNSIGNED;
  SELECT id INTO prov FROM sources WHERE slug = 'provisioner' AND deleted_at IS NULL LIMIT 1;

  IF EXISTS (SELECT 1 FROM prov_runs) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT = 'refusing rollback: prov_runs holds audit records';
  END IF;
  IF EXISTS (SELECT 1 FROM prov_stack_artifacts) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT = 'refusing rollback: prov_stack_artifacts has rows (029 seeds none)';
  END IF;
  IF EXISTS (SELECT 1 FROM prov_stacks WHERE slug NOT IN ('core','dev-tools'))
     OR (SELECT COUNT(*) FROM prov_stack_items) > 21
     OR (SELECT COUNT(*) FROM prov_stack_deps) > 1
     OR (SELECT COUNT(*) FROM prov_images) > 6 THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT = 'refusing rollback: prov_* holds rows beyond the 029 seed';
  END IF;
  IF EXISTS (SELECT 1 FROM prov_stacks WHERE source_id <> prov OR deleted_at IS NOT NULL OR updated_at > created_at)
     OR EXISTS (SELECT 1 FROM prov_stack_items WHERE source_id <> prov OR deleted_at IS NOT NULL OR updated_at > created_at)
     OR EXISTS (SELECT 1 FROM prov_stack_deps WHERE source_id <> prov OR deleted_at IS NOT NULL OR updated_at > created_at)
     OR EXISTS (SELECT 1 FROM prov_images WHERE source_id <> prov OR deleted_at IS NOT NULL OR updated_at > created_at
                                          OR sha256 IS NOT NULL OR fetched_at IS NOT NULL OR netboot_verified <> 0) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT = 'refusing rollback: prov_* rows were edited, soft-deleted, fetched or asserted by another source';
  END IF;
END//
DELIMITER ;
CALL `_solarinet_rollback_m029_assert_seed_only`();
DROP PROCEDURE `_solarinet_rollback_m029_assert_seed_only`;

DROP TRIGGER IF EXISTS `trg_prov_runs_bd`;
DROP TRIGGER IF EXISTS `trg_prov_runs_bu`;
DROP TABLE IF EXISTS `prov_runs`;
DROP TABLE IF EXISTS `prov_stack_artifacts`;
DROP TABLE IF EXISTS `prov_stack_deps`;
DROP TABLE IF EXISTS `prov_stack_items`;
DROP TABLE IF EXISTS `prov_stacks`;
DROP TABLE IF EXISTS `prov_images`;
