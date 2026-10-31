-- ============================================================================
-- 031_provisioning_arch_refresh.sql — hardware model CPU arch + refresh runs
-- ============================================================================
-- Cross-lab review P5 of the Provisioning Images & Stacks feature
-- (docs/Provisioning_UI_Design.md §3 item 1, §4 P5). Two schema changes:
--
--   1. `hardware_models.arch` — nullable CPU architecture of a product, using
--      exactly the prov_images.arch vocabulary (x86_64, arm64, armhf, riscv64,
--      i386). NULL = unknown. It is the FIRST arch source for a deploy target
--      (agent-reported monitoring `node.arch` is the fallback). A reimage is
--      refused while the target arch is unknown or differs from the image arch
--      (API and prov-stage.py both fail closed). Apply-stacks-only keeps using
--      Ansible fact detection and does not read this column.
--      ENUM values are only ever appended, in lockstep with prov_images.arch.
--
--   2. `prov_runs` gains mode `refresh` (IMAGE_REFRESH now records a run with a
--      terminal status instead of only "started"). A refresh run targets an
--      image, not a machine, so `entity_id` becomes NULL-able and the 029 CHECK
--      `ck_prov_runs_image` ((mode='reimage') = (image_id IS NOT NULL)) is
--      replaced by `ck_prov_runs_shape`:
--        reimage  entity + image
--        apply    entity, no image
--        refresh  image, no entity
--      The 029 audit triggers (trg_prov_runs_bu/_bd) compare with <=> so a NULL
--      entity_id is still immutable; the 030 CDC triggers publish by row id and
--      are unaffected.
--
-- CDC: hardware_models carries no CDC triggers (023 does not cover it), so
-- adding a column needs no trigger change; prov_runs keeps 030's triggers.
-- No trigger is created or replaced here.
--
-- Idempotent: MODIFY COLUMN re-states the same definition; ADD COLUMN /
-- DROP CONSTRAINT use IF [NOT] EXISTS; the new CHECK is added only when absent
-- (information_schema guard, like 014).
--
-- Target: MariaDB >= 10.7. Database: `sor` on cesium (10.1.0.200) ONLY;
-- replication carries it to benzene. Apply through tools/migrate.py as the DB
-- root account (the standing migrate-SoR-as-root rule; ALTER on a table that
-- carries triggers on a binlogged primary). Manual rollback:
-- netdb/sor/rollback/031-down.sql. Validate on a scratch schema first — never
-- point tests at live sor.
--
-- Deploy order: apply 031 BEFORE activating the release that carries the P5
-- helpers/dashboard (they read hardware_models.arch and write mode 'refresh').
-- The P4 release keeps working on a 031 schema (new column unread; existing
-- modes unchanged).
-- ============================================================================

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

-- ----------------------------------------------------------------------------
-- 1. hardware_models.arch
-- ----------------------------------------------------------------------------
ALTER TABLE `hardware_models`
  ADD COLUMN IF NOT EXISTS `arch` ENUM('x86_64','arm64','armhf','riscv64','i386') NULL DEFAULT NULL
    COMMENT 'CPU architecture (prov_images.arch vocabulary); NULL = unknown. First arch source for reimage targets'
    AFTER `hw_type`;

-- ----------------------------------------------------------------------------
-- 2. prov_runs: mode 'refresh', NULL-able entity, shape CHECK
-- ----------------------------------------------------------------------------
ALTER TABLE `prov_runs`
  DROP CONSTRAINT IF EXISTS `ck_prov_runs_image`;

ALTER TABLE `prov_runs`
  MODIFY COLUMN `mode` ENUM('reimage','apply','refresh') NOT NULL
    COMMENT 'reimage = wipe + install + stacks at first boot; apply = Ansible over SSH, no reinstall; refresh = IMAGE_REFRESH of one image',
  MODIFY COLUMN `entity_id` BIGINT UNSIGNED NULL
    COMMENT 'FK: target machine (reimage/apply); NULL for refresh';

SET @ck := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
             WHERE CONSTRAINT_SCHEMA = DATABASE() AND TABLE_NAME = 'prov_runs'
               AND CONSTRAINT_NAME = 'ck_prov_runs_shape' AND CONSTRAINT_TYPE = 'CHECK');
SET @sql := IF(@ck = 0,
  'ALTER TABLE `prov_runs` ADD CONSTRAINT `ck_prov_runs_shape` CHECK (
     (`mode` = ''reimage'' AND `entity_id` IS NOT NULL AND `image_id` IS NOT NULL)
  OR (`mode` = ''apply''   AND `entity_id` IS NOT NULL AND `image_id` IS NULL)
  OR (`mode` = ''refresh'' AND `entity_id` IS NULL     AND `image_id` IS NOT NULL))',
  'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;
