-- 037: Inventory I1 close-out. Items may be created directly on a catalog
-- product type (hardware_units.model_id becomes optional), live location
-- codes become globally unique, and optimistic-lock timestamps gain
-- microsecond precision so two edits in the same second still conflict.
SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

-- Preflight every data-dependent constraint before changing the schema.
DROP PROCEDURE IF EXISTS `_solarinet_m037_preflight`;
DELIMITER //
CREATE PROCEDURE `_solarinet_m037_preflight`()
BEGIN
  DECLARE msg VARCHAR(255);
  -- 033 derived product_type_id for every unit; a unit without one cannot
  -- satisfy the product-type requirement the triggers now enforce.
  IF EXISTS (
    SELECT 1 FROM hardware_units hu
      LEFT JOIN hardware_models hm ON hm.id=hu.model_id
     WHERE hu.product_type_id IS NULL AND hm.product_type_id IS NULL
  ) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='037 preflight: hardware unit without a product type';
  END IF;
  IF EXISTS (
    SELECT 1 FROM hardware_units hu JOIN hardware_models hm ON hm.id=hu.model_id
     WHERE hu.product_type_id IS NOT NULL AND NOT (hu.product_type_id <=> hm.product_type_id)
  ) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='037 preflight: hardware unit model and product type disagree';
  END IF;
  SELECT GROUP_CONCAT(code ORDER BY code SEPARATOR ',') INTO msg
    FROM (SELECT code FROM locations
           WHERE deleted_at IS NULL AND code IS NOT NULL AND code<>''
           GROUP BY code HAVING COUNT(*)>1) d;
  IF msg IS NOT NULL THEN
    SET msg=LEFT(CONCAT('037 preflight: duplicate live location codes ',msg),255);
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT=msg;
  END IF;
  -- Serial uniqueness moves from (model, serial) to (product type, serial).
  -- Product types and models are 1:1, so this only refuses inconsistent data.
  IF EXISTS (
    SELECT 1 FROM hardware_units hu
      LEFT JOIN hardware_models hm ON hm.id=hu.model_id
     WHERE hu.serial IS NOT NULL
     GROUP BY COALESCE(hu.product_type_id, hm.product_type_id), hu.serial
    HAVING COUNT(*)>1
  ) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='037 preflight: duplicate serial within a product type';
  END IF;
END//
DELIMITER ;
CALL `_solarinet_m037_preflight`();
DROP PROCEDURE `_solarinet_m037_preflight`;

-- Fill any product type 033 left NULL; a backfill is not an operator edit.
UPDATE hardware_units hu JOIN hardware_models hm ON hm.id=hu.model_id
   SET hu.product_type_id=hm.product_type_id, hu.updated_at=hu.updated_at
 WHERE hu.product_type_id IS NULL;

ALTER TABLE hardware_units
  MODIFY `model_id` INT UNSIGNED NULL COMMENT 'FK: legacy hardware model; NULL for catalog-only product types',
  ADD UNIQUE KEY `uq_hw_units_type_serial` (`product_type_id`,`serial`);

-- Live location codes are unique across the whole tree. Empty codes and
-- soft-deleted rows contribute NULL, which a unique key admits repeatedly.
ALTER TABLE locations
  ADD COLUMN `location_code_key` VARCHAR(32) GENERATED ALWAYS AS (IF(`deleted_at` IS NULL, NULLIF(`code`,''), NULL)) PERSISTENT,
  ADD UNIQUE KEY `uq_locations_live_code` (`location_code_key`);

-- Optimistic-lock tokens: microsecond precision. Existing values keep their
-- whole-second value (.000000), so outstanding tokens remain comparable.
ALTER TABLE locations
  MODIFY `updated_at` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6) COMMENT 'last row modification';
ALTER TABLE product_types
  MODIFY `updated_at` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6);
ALTER TABLE container_types
  MODIFY `updated_at` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6);

-- Units are identified by product type. model_id derives product_type_id; a
-- product type alone fills model_id from its live legacy model, if any; both
-- given must agree; neither is an error.
DROP TRIGGER IF EXISTS `trg_hw_units_installed_target_ins`;
DROP TRIGGER IF EXISTS `trg_hw_units_installed_target_upd`;
DELIMITER //
CREATE TRIGGER `trg_hw_units_installed_target_ins` BEFORE INSERT ON `hardware_units`
FOR EACH ROW
BEGIN
  DECLARE model_type INT UNSIGNED DEFAULT NULL;
  IF NEW.`model_id` IS NOT NULL THEN
    SET model_type = (SELECT `product_type_id` FROM `hardware_models` WHERE `id`=NEW.`model_id`);
    IF NEW.`product_type_id` IS NULL THEN
      SET NEW.`product_type_id` = model_type;
    ELSEIF NOT (NEW.`product_type_id` <=> model_type) THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='model_id and product_type_id disagree';
    END IF;
  ELSEIF NEW.`product_type_id` IS NOT NULL THEN
    SET NEW.`model_id` = (SELECT `id` FROM `hardware_models`
                           WHERE `product_type_id`=NEW.`product_type_id` AND `deleted_at` IS NULL LIMIT 1);
  END IF;
  IF NEW.`product_type_id` IS NULL THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='hardware unit product_type_id is required';
  END IF;
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
  DECLARE model_type INT UNSIGNED DEFAULT NULL;
  IF NEW.`product_type_id` IS NOT NULL
     AND NOT (NEW.`product_type_id` <=> OLD.`product_type_id`)
     AND NEW.`model_id` <=> OLD.`model_id` THEN
    -- A catalog change without an explicit model change follows the catalog.
    SET NEW.`model_id` = (SELECT `id` FROM `hardware_models`
                           WHERE `product_type_id`=NEW.`product_type_id` AND `deleted_at` IS NULL LIMIT 1);
  ELSEIF NEW.`model_id` IS NOT NULL
     AND (NEW.`product_type_id` IS NULL
          OR (NOT (NEW.`model_id` <=> OLD.`model_id`)
              AND NEW.`product_type_id` <=> OLD.`product_type_id`)) THEN
    -- A model change without an explicit catalog change follows the model.
    SET NEW.`product_type_id` = (SELECT `product_type_id` FROM `hardware_models` WHERE `id`=NEW.`model_id`);
  END IF;
  IF NEW.`product_type_id` IS NULL THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='hardware unit product_type_id is required';
  END IF;
  IF NEW.`model_id` IS NOT NULL THEN
    SET model_type = (SELECT `product_type_id` FROM `hardware_models` WHERE `id`=NEW.`model_id`);
    IF NOT (NEW.`product_type_id` <=> model_type) THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='model_id and product_type_id disagree';
    END IF;
  END IF;
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
DELIMITER ;
