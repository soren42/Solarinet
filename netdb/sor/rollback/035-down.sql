-- Compensating rollback for 035_catalog_display_names.sql.
--
-- Outside migrations/ so tools/migrate.py cannot discover it. Restores the
-- 033 legacy-sync triggers and reverts backfilled display names to the bare
-- model string, only where the name still equals the 035-derived value.
-- Names an operator has since edited are left alone. After success, remove the
-- 035 migration-ledger row manually.

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

UPDATE `product_types`
   SET `name` = `model`
 WHERE `lookup_source` IN ('migration:033','legacy:hardware_models')
   AND `model` IS NOT NULL
   AND `name` <=> LEFT(CASE
         WHEN `brand` IS NULL OR `brand` = '' THEN `model`
         WHEN LEFT(`model`, CHAR_LENGTH(`brand`)) = `brand` THEN `model`
         WHEN RIGHT(`brand`, CHAR_LENGTH(`model`)) = `model` THEN `brand`
         ELSE CONCAT(`brand`, ' ', `model`)
       END, 160);

DROP TRIGGER IF EXISTS `trg_hw_models_catalog_ins`;
DROP TRIGGER IF EXISTS `trg_hw_models_catalog_upd`;

DELIMITER //
CREATE TRIGGER `trg_hw_models_catalog_ins` BEFORE INSERT ON `hardware_models`
FOR EACH ROW
BEGIN
  IF NEW.`product_type_id` IS NULL THEN
    INSERT INTO `product_types`
      (`kind`,`name`,`brand`,`model`,`notes`,`lookup_source`,`source_id`,`asserted_kind`,`deleted_at`)
    VALUES (CASE NEW.`hw_type`
              WHEN 'printer' THEN 'peripheral'
              WHEN 'scanner' THEN 'peripheral'
              WHEN 'other' THEN 'other'
              ELSE 'hardware'
            END,
            NEW.`model`, NEW.`make`, NEW.`model`, NEW.`notes`, 'legacy:hardware_models',
            NEW.`source_id`, NEW.`asserted_kind`, NEW.`deleted_at`);
    SET NEW.`product_type_id` = LAST_INSERT_ID();
  END IF;
END//
CREATE TRIGGER `trg_hw_models_catalog_upd` AFTER UPDATE ON `hardware_models`
FOR EACH ROW
BEGIN
  IF NOT (NEW.`make` <=> OLD.`make`) OR NOT (NEW.`model` <=> OLD.`model`)
     OR NOT (NEW.`hw_type` <=> OLD.`hw_type`) OR NOT (NEW.`notes` <=> OLD.`notes`)
     OR NOT (NEW.`deleted_at` <=> OLD.`deleted_at`) THEN
    UPDATE `product_types`
       SET `kind` = CASE NEW.`hw_type`
                      WHEN 'printer' THEN 'peripheral'
                      WHEN 'scanner' THEN 'peripheral'
                      WHEN 'other' THEN 'other'
                      ELSE 'hardware'
                    END,
           `name`=NEW.`model`, `brand`=NEW.`make`, `model`=NEW.`model`,
           `notes`=NEW.`notes`, `deleted_at`=NEW.`deleted_at`
     WHERE `id`=NEW.`product_type_id`
       AND `lookup_source` IN ('migration:033','legacy:hardware_models');
  END IF;
END//
DELIMITER ;
