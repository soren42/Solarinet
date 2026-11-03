-- 035_catalog_display_names.sql — readable catalog names; legacy sync stops
-- clobbering operator edits.
--
-- 033 backfilled product_types.name from hardware_models.model alone, so the
-- catalog shows "400" and "5" instead of "Raspberry Pi 400". The display name
-- is now brand + model, without repeating a word the two already share:
--   no brand                 -> model           (ILI9481)
--   model starts with brand  -> model           (Eufy / Eufybase -> Eufybase)
--   brand ends with model    -> brand           (Google Nest / Nest -> Google Nest)
--   otherwise                -> brand + ' ' + model
--
-- The 033 compatibility trigger on hardware_models UPDATE also rewrote kind,
-- name, brand, model and notes unconditionally, so a seed.py or provisioning
-- edit of a model reverted any operator change made through the catalog API.
-- It now moves a catalog field only while that field still holds the value
-- the legacy row would have derived (i.e. nobody has edited it).
--
-- Rollback: netdb/sor/rollback/035-down.sql.

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

-- Backfill: only rows still carrying the 033-derived name (name = model).
UPDATE `product_types`
   SET `name` = LEFT(CASE
         WHEN `brand` IS NULL OR `brand` = '' THEN `model`
         WHEN LEFT(`model`, CHAR_LENGTH(`brand`)) = `brand` THEN `model`
         WHEN RIGHT(`brand`, CHAR_LENGTH(`model`)) = `model` THEN `brand`
         ELSE CONCAT(`brand`, ' ', `model`)
       END, 160)
 WHERE `lookup_source` IN ('migration:033','legacy:hardware_models')
   AND `model` IS NOT NULL
   AND `name` <=> `model`;

DROP TRIGGER IF EXISTS `trg_hw_models_catalog_ins`;
DROP TRIGGER IF EXISTS `trg_hw_models_catalog_upd`;

DELIMITER //
-- Compatibility window: the legacy model API still writes hardware_models
-- directly. Give each new model its catalog row.
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
            LEFT(CASE
              WHEN NEW.`make` IS NULL OR NEW.`make` = '' THEN NEW.`model`
              WHEN LEFT(NEW.`model`, CHAR_LENGTH(NEW.`make`)) = NEW.`make` THEN NEW.`model`
              WHEN RIGHT(NEW.`make`, CHAR_LENGTH(NEW.`model`)) = NEW.`model` THEN NEW.`make`
              ELSE CONCAT(NEW.`make`, ' ', NEW.`model`)
            END, 160),
            NEW.`make`, NEW.`model`, NEW.`notes`, 'legacy:hardware_models',
            NEW.`source_id`, NEW.`asserted_kind`, NEW.`deleted_at`);
    SET NEW.`product_type_id` = LAST_INSERT_ID();
  END IF;
END//
-- Mirror legacy edits onto catalog rows the migration or the insert trigger
-- created, field by field, only where the catalog still holds the value the
-- OLD legacy row implied. Operator edits win.
CREATE TRIGGER `trg_hw_models_catalog_upd` AFTER UPDATE ON `hardware_models`
FOR EACH ROW
BEGIN
  DECLARE old_kind, new_kind VARCHAR(16);
  DECLARE old_name, new_name VARCHAR(160);
  IF NOT (NEW.`make` <=> OLD.`make`) OR NOT (NEW.`model` <=> OLD.`model`)
     OR NOT (NEW.`hw_type` <=> OLD.`hw_type`) OR NOT (NEW.`notes` <=> OLD.`notes`)
     OR NOT (NEW.`deleted_at` <=> OLD.`deleted_at`) THEN
    SET old_kind = CASE OLD.`hw_type` WHEN 'printer' THEN 'peripheral' WHEN 'scanner' THEN 'peripheral'
                                      WHEN 'other' THEN 'other' ELSE 'hardware' END;
    SET new_kind = CASE NEW.`hw_type` WHEN 'printer' THEN 'peripheral' WHEN 'scanner' THEN 'peripheral'
                                      WHEN 'other' THEN 'other' ELSE 'hardware' END;
    SET old_name = LEFT(CASE
          WHEN OLD.`make` IS NULL OR OLD.`make` = '' THEN OLD.`model`
          WHEN LEFT(OLD.`model`, CHAR_LENGTH(OLD.`make`)) = OLD.`make` THEN OLD.`model`
          WHEN RIGHT(OLD.`make`, CHAR_LENGTH(OLD.`model`)) = OLD.`model` THEN OLD.`make`
          ELSE CONCAT(OLD.`make`, ' ', OLD.`model`)
        END, 160);
    SET new_name = LEFT(CASE
          WHEN NEW.`make` IS NULL OR NEW.`make` = '' THEN NEW.`model`
          WHEN LEFT(NEW.`model`, CHAR_LENGTH(NEW.`make`)) = NEW.`make` THEN NEW.`model`
          WHEN RIGHT(NEW.`make`, CHAR_LENGTH(NEW.`model`)) = NEW.`model` THEN NEW.`make`
          ELSE CONCAT(NEW.`make`, ' ', NEW.`model`)
        END, 160);
    -- Each IF reads only its own column, so MariaDB's left-to-right SET
    -- evaluation cannot leak one assignment into another's test.
    UPDATE `product_types`
       SET `kind`  = IF(`kind` <=> old_kind, new_kind, `kind`),
           `name`  = IF(`name` <=> old_name OR `name` <=> OLD.`model`, new_name, `name`),
           `brand` = IF(`brand` <=> OLD.`make`, NEW.`make`, `brand`),
           `model` = IF(`model` <=> OLD.`model`, NEW.`model`, `model`),
           `notes` = IF(`notes` <=> OLD.`notes`, NEW.`notes`, `notes`),
           `deleted_at` = NEW.`deleted_at`
     WHERE `id`=NEW.`product_type_id`
       AND `lookup_source` IN ('migration:033','legacy:hardware_models');
  END IF;
END//
DELIMITER ;
