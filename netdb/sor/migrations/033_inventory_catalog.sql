-- 033_inventory_catalog.sql — broaden the inventory catalog without breaking
-- legacy hardware-model consumers.
--
-- Backfill policy (reviewed WP0 audit): product_types.id preserves
-- hardware_models.id. `printer` and `scanner` map to `peripheral`, `other`
-- maps to `other`, and every other legacy hw_type maps to `hardware`. The old
-- hw_type/make/model/model_id columns remain intact for the compatibility
-- window. Product-type provenance identifies this as a dashboard migration;
-- lookup_source/ref identify the legacy row, not an external lookup. No
-- product_barcodes rows are generated: product barcode data is only committed
-- after a later operator save (D3).
--
-- The nullable brand/model dedupe key is intentionally omitted. A MariaDB
-- UNIQUE key containing NULL deleted_at values does not enforce uniqueness
-- among live rows, and the plan makes this optional pending a valid policy.

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

CREATE TABLE IF NOT EXISTS `product_types` (
  `id`             INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK; legacy hardware model IDs are preserved during migration 033',
  `kind`           ENUM('hardware','peripheral','component','software','accessory','consumable','other') NOT NULL,
  `name`           VARCHAR(160) NOT NULL,
  `brand`          VARCHAR(128) NULL,
  `model`          VARCHAR(128) NULL,
  `category`       VARCHAR(128) NULL,
  `specs`          JSON NULL,
  `notes`          TEXT NULL,
  `image_url`      VARCHAR(1024) NULL,
  `lookup_source`  VARCHAR(64) NULL COMMENT 'external lookup provider or migration source identifier',
  `lookup_ref`     VARCHAR(255) NULL COMMENT 'external lookup reference or source-row reference',
  `source_id`      SMALLINT UNSIGNED NOT NULL COMMENT 'SoR provenance source',
  `asserted_kind`  ENUM('human','machine') NOT NULL DEFAULT 'human',
  `asserted_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `created_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  `deleted_at`     DATETIME NULL,
  PRIMARY KEY (`id`),
  KEY `ix_product_types_kind` (`kind`,`deleted_at`),
  KEY `ix_product_types_name` (`name`),
  KEY `fk_product_types_source` (`source_id`),
  CONSTRAINT `fk_product_types_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`) ON DELETE RESTRICT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Broad catalog of hardware, software, peripherals, and other products';

CREATE TABLE IF NOT EXISTS `product_barcodes` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `product_type_id` INT UNSIGNED NOT NULL,
  `code`           VARCHAR(128) NOT NULL,
  `code_type`      ENUM('gtin','sku','other') NOT NULL DEFAULT 'other',
  `source_id`      SMALLINT UNSIGNED NOT NULL,
  `asserted_kind`  ENUM('human','machine') NOT NULL DEFAULT 'human',
  `asserted_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `created_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  `deleted_at`     DATETIME NULL,
  `live_flag`      TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_product_barcodes_code_live` (`code`,`live_flag`),
  KEY `ix_product_barcodes_product_deleted` (`product_type_id`,`deleted_at`),
  KEY `ix_product_barcodes_code` (`code`),
  KEY `fk_product_barcodes_source` (`source_id`),
  CONSTRAINT `fk_product_barcodes_product` FOREIGN KEY (`product_type_id`) REFERENCES `product_types` (`id`) ON DELETE RESTRICT,
  CONSTRAINT `fk_product_barcodes_source` FOREIGN KEY (`source_id`) REFERENCES `sources` (`id`) ON DELETE RESTRICT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Commercial product codes, distinct from SolariNet system barcodes';

ALTER TABLE `barcodes`
  MODIFY COLUMN `subject_table` VARCHAR(64) NOT NULL
    COMMENT 'hardware_units|locations|projects|interfaces|product_types (polymorphic; enforced in the API)';

ALTER TABLE `hardware_models`
  ADD COLUMN IF NOT EXISTS `product_type_id` INT UNSIGNED NULL
    COMMENT 'FK to the broader product catalog; legacy make/model/hw_type remain during transition'
    AFTER `arch`;

ALTER TABLE `hardware_units`
  ADD COLUMN IF NOT EXISTS `product_type_id` INT UNSIGNED NULL
    COMMENT 'broader product catalog link; legacy model_id remains during transition'
    AFTER `model_id`,
  ADD COLUMN IF NOT EXISTS `licence_seats` INT UNSIGNED NULL
    COMMENT 'licensed seat count; software instances only'
    AFTER `receipt_id`,
  ADD COLUMN IF NOT EXISTS `licence_expires_on` DATE NULL
    COMMENT 'licence expiry date; software instances only'
    AFTER `licence_seats`,
  ADD COLUMN IF NOT EXISTS `licence_reference` VARCHAR(1024) NULL
    COMMENT 'pointer to licence material; never store a key or token here'
    AFTER `licence_expires_on`,
  ADD COLUMN IF NOT EXISTS `installed_on_unit_id` INT UNSIGNED NULL
    COMMENT 'software installation target hardware unit; never a person'
    AFTER `licence_reference`;

-- Idempotent information-schema guards keep a partial/manual re-run from
-- replacing constraints. Product/model links are deliberately restrictive.
SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
              WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_models'
                AND INDEX_NAME='uq_hw_models_product_type');
SET @sql := IF(@idx=0,
  'ALTER TABLE `hardware_models` ADD UNIQUE KEY `uq_hw_models_product_type` (`product_type_id`)', 'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
              WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_units'
                AND INDEX_NAME='ix_hw_units_product_type');
SET @sql := IF(@idx=0,
  'ALTER TABLE `hardware_units` ADD KEY `ix_hw_units_product_type` (`product_type_id`)', 'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
              WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_units'
                AND INDEX_NAME='ix_hw_units_installed_on_unit');
SET @sql := IF(@idx=0,
  'ALTER TABLE `hardware_units` ADD KEY `ix_hw_units_installed_on_unit` (`installed_on_unit_id`)', 'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
              WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='hardware_units'
                AND INDEX_NAME='ix_hw_units_licence_expiry');
SET @sql := IF(@idx=0,
  'ALTER TABLE `hardware_units` ADD KEY `ix_hw_units_licence_expiry` (`licence_expires_on`,`deleted_at`)', 'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
             WHERE CONSTRAINT_SCHEMA=DATABASE() AND TABLE_NAME='hardware_models'
               AND CONSTRAINT_NAME='fk_hw_models_product_type' AND CONSTRAINT_TYPE='FOREIGN KEY');
SET @sql := IF(@fk=0,
  'ALTER TABLE `hardware_models` ADD CONSTRAINT `fk_hw_models_product_type` FOREIGN KEY (`product_type_id`) REFERENCES `product_types` (`id`) ON DELETE RESTRICT', 'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
             WHERE CONSTRAINT_SCHEMA=DATABASE() AND TABLE_NAME='hardware_units'
               AND CONSTRAINT_NAME='fk_hw_units_product_type' AND CONSTRAINT_TYPE='FOREIGN KEY');
SET @sql := IF(@fk=0,
  'ALTER TABLE `hardware_units` ADD CONSTRAINT `fk_hw_units_product_type` FOREIGN KEY (`product_type_id`) REFERENCES `product_types` (`id`) ON DELETE RESTRICT', 'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
             WHERE CONSTRAINT_SCHEMA=DATABASE() AND TABLE_NAME='hardware_units'
               AND CONSTRAINT_NAME='fk_hw_units_installed_on_unit' AND CONSTRAINT_TYPE='FOREIGN KEY');
SET @sql := IF(@fk=0,
  'ALTER TABLE `hardware_units` ADD CONSTRAINT `fk_hw_units_installed_on_unit` FOREIGN KEY (`installed_on_unit_id`) REFERENCES `hardware_units` (`id`) ON DELETE SET NULL', 'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

-- Migration-created product types carry a trace back to their source row.
-- The source itself is the existing dashboard provenance registry entry.
INSERT IGNORE INTO `product_types`
  (`id`,`kind`,`name`,`brand`,`model`,`notes`,`lookup_source`,`lookup_ref`,
   `source_id`,`asserted_kind`,`deleted_at`)
SELECT hm.`id`,
       CASE hm.`hw_type`
         WHEN 'printer' THEN 'peripheral'
         WHEN 'scanner' THEN 'peripheral'
         WHEN 'other' THEN 'other'
         ELSE 'hardware'
       END,
       hm.`model`, hm.`make`, hm.`model`, hm.`notes`, 'migration:033',
       CONCAT('hardware_models:', hm.`id`),
       src.`id`, 'human', hm.`deleted_at`
  FROM `hardware_models` hm
  JOIN `sources` src ON src.`slug`='dashboard';

UPDATE `hardware_models`
   SET `product_type_id`=`id`
 WHERE `product_type_id` IS NULL;

UPDATE `hardware_units` hu
  JOIN `hardware_models` hm ON hm.`id`=hu.`model_id`
   SET hu.`product_type_id`=hm.`product_type_id`
 WHERE hu.`product_type_id` IS NULL;

-- Keep the next catalog allocation above every legacy model ID, including
-- sparse high IDs. No catalog rows are allocated for product barcodes here.
SET @next_product_type_id := (
  SELECT GREATEST(COALESCE((SELECT MAX(`id`) FROM `hardware_models`),0),
                  COALESCE((SELECT MAX(`id`) FROM `product_types`),0))+1
);
SET @sql := CONCAT('ALTER TABLE `product_types` AUTO_INCREMENT=', @next_product_type_id);
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

-- A software installation may only point at another, non-software unit. The
-- self check belongs in triggers because the inserted AUTO_INCREMENT id is
-- not stable in a table CHECK on all supported MariaDB versions.
DROP TRIGGER IF EXISTS `trg_hw_units_installed_target_ins`;
DROP TRIGGER IF EXISTS `trg_hw_units_installed_target_upd`;
DROP TRIGGER IF EXISTS `trg_product_types_no_software_install_target`;
DELIMITER //
CREATE TRIGGER `trg_hw_units_installed_target_ins` BEFORE INSERT ON `hardware_units`
FOR EACH ROW
BEGIN
  IF NEW.`installed_on_unit_id` IS NOT NULL THEN
    IF NEW.`id`=NEW.`installed_on_unit_id` THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='installed_on_unit_id cannot reference the same unit';
    END IF;
    IF NOT EXISTS (
      SELECT 1 FROM `hardware_units` target
      JOIN `product_types` product ON product.`id`=target.`product_type_id`
      WHERE target.`id`=NEW.`installed_on_unit_id`
        AND product.`kind`<>'software'
    ) THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='installed_on_unit_id must reference a non-software unit';
    END IF;
  END IF;
END//
CREATE TRIGGER `trg_hw_units_installed_target_upd` BEFORE UPDATE ON `hardware_units`
FOR EACH ROW
BEGIN
  IF NEW.`installed_on_unit_id` IS NOT NULL THEN
    IF NEW.`id`=NEW.`installed_on_unit_id` THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='installed_on_unit_id cannot reference the same unit';
    END IF;
    IF NOT EXISTS (
      SELECT 1 FROM `hardware_units` target
      JOIN `product_types` product ON product.`id`=target.`product_type_id`
      WHERE target.`id`=NEW.`installed_on_unit_id`
        AND product.`kind`<>'software'
    ) THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='installed_on_unit_id must reference a non-software unit';
    END IF;
  END IF;
END//
CREATE TRIGGER `trg_product_types_no_software_install_target` BEFORE UPDATE ON `product_types`
FOR EACH ROW
BEGIN
  IF NEW.`kind`='software' AND OLD.`kind`<>'software' AND EXISTS (
    SELECT 1
      FROM `hardware_units` target
      JOIN `hardware_units` installed ON installed.`installed_on_unit_id`=target.`id`
     WHERE target.`product_type_id`=OLD.`id`
  ) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='product type cannot become software while installed units target it';
  END IF;
END//
DELIMITER ;
