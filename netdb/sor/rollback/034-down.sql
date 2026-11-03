-- Compensating rollback for 034_prov_images_os.sql.
--
-- Canonical operating_systems rows are deliberately retained: after forward
-- application they may be referenced by entities or other catalog data, and
-- deleting them would be destructive. This rollback removes only the image
-- link introduced by 034. DDL autocommits; remove the 034 migration-ledger row
-- manually after a successful rollback before forward recovery.

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

ALTER TABLE `prov_images`
  DROP FOREIGN KEY IF EXISTS `fk_prov_images_os`,
  DROP INDEX IF EXISTS `ix_prov_images_os`,
  DROP COLUMN IF EXISTS `os_id`;
