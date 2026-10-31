-- Manual rollback for 031_provisioning_arch_refresh.sql.
-- This file intentionally lives outside migrations/ so the forward-only
-- migration runner cannot discover or execute it.
--
-- Restores the exact 029/030 shape: prov_runs.mode back to ('reimage','apply'),
-- entity_id NOT NULL, the 029 column comments, ck_prov_runs_shape replaced by
-- the 029 ck_prov_runs_image, and hardware_models.arch dropped. Triggers are
-- untouched (031 created none): the 029 guards and 030 CDC triggers stay.
--
-- Before use: deactivate the P5 release (its helpers write mode 'refresh' and
-- its dashboard reads/edits hardware_models.arch), take and verify a fresh SoR
-- backup. MariaDB DDL autocommits; this is not a transactional rollback. Apply
-- as the DB root account on Cesium only, never Benzene. Afterwards remove the
-- 031 row from the migration ledger (tools/migrate.py) by hand.
--
-- Preflight: refuses, before any DDL, while the 031 shape holds data the 029
-- shape cannot represent or that dropping arch would destroy:
--   * any prov_runs row with mode='refresh' or a NULL entity_id. prov_runs is
--     an audit log (the 029 guard refuses deletes), so once a refresh run is
--     recorded this rollback stays refused; recovering needs a deliberate,
--     separately reviewed decision (restore the pre-031 backup, or drop the
--     guard and delete the refresh rows) — this file never does either;
--   * any hardware_models row with a non-NULL arch (operator data).
-- Idempotent: a second run (or a run on a pre-031 schema) passes the preflight
-- and re-states the 029 definitions.
DROP PROCEDURE IF EXISTS `_solarinet_rollback_m031_assert_reversible`;
DELIMITER //
CREATE PROCEDURE `_solarinet_rollback_m031_assert_reversible`()
BEGIN
  IF EXISTS (SELECT 1 FROM prov_runs WHERE `mode` = 'refresh' OR `entity_id` IS NULL) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT = 'refusing rollback: prov_runs holds refresh runs or NULL entity_id (audit rows the 029 shape cannot hold)';
  END IF;
  IF EXISTS (SELECT 1 FROM information_schema.COLUMNS
              WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'hardware_models' AND COLUMN_NAME = 'arch') THEN
    SET @_m031_arch := 0;
    PREPARE st FROM 'SELECT COUNT(*) INTO @_m031_arch FROM `hardware_models` WHERE `arch` IS NOT NULL';
    EXECUTE st; DEALLOCATE PREPARE st;
    IF @_m031_arch <> 0 THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'refusing rollback: hardware_models.arch holds operator values';
    END IF;
  END IF;
END//
DELIMITER ;
CALL `_solarinet_rollback_m031_assert_reversible`();
DROP PROCEDURE `_solarinet_rollback_m031_assert_reversible`;

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

ALTER TABLE `prov_runs`
  DROP CONSTRAINT IF EXISTS `ck_prov_runs_shape`;

ALTER TABLE `prov_runs`
  MODIFY COLUMN `mode` ENUM('reimage','apply') NOT NULL
    COMMENT 'reimage = wipe + install + stacks at first boot; apply = Ansible over SSH, no reinstall',
  MODIFY COLUMN `entity_id` BIGINT UNSIGNED NOT NULL
    COMMENT 'FK: target machine',
  MODIFY COLUMN `image_id` INT UNSIGNED NULL
    COMMENT 'FK: image installed (reimage only; NULL for apply)';

SET @ck := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
             WHERE CONSTRAINT_SCHEMA = DATABASE() AND TABLE_NAME = 'prov_runs'
               AND CONSTRAINT_NAME = 'ck_prov_runs_image' AND CONSTRAINT_TYPE = 'CHECK');
SET @sql := IF(@ck = 0,
  'ALTER TABLE `prov_runs` ADD CONSTRAINT `ck_prov_runs_image` CHECK ((`mode` = ''reimage'') = (`image_id` IS NOT NULL))',
  'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

ALTER TABLE `hardware_models`
  DROP COLUMN IF EXISTS `arch`;
