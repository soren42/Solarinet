-- Compensating rollback for 037_inventory_items_closeout.sql. It refuses
-- while any unit exists only as a catalog product type (model_id NULL): the
-- prior schema cannot represent it, and this rollback never deletes operator
-- data. Lock timestamps lose their sub-second part (values are truncated).
SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

DROP PROCEDURE IF EXISTS `_solarinet_rollback_m037_assert_reversible`;
DELIMITER //
CREATE PROCEDURE `_solarinet_rollback_m037_assert_reversible`()
BEGIN
  IF EXISTS (SELECT 1 FROM hardware_units WHERE model_id IS NULL) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='refusing rollback: hardware units without a legacy model_id exist';
  END IF;
END//
DELIMITER ;
CALL `_solarinet_rollback_m037_assert_reversible`();
DROP PROCEDURE `_solarinet_rollback_m037_assert_reversible`;

-- Accepted precision loss: updated_at is optimistic-lock metadata only; the
-- fractional seconds are dropped (rounded) on rollback, no business data is lost.
ALTER TABLE container_types
  MODIFY `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP;
ALTER TABLE product_types
  MODIFY `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP;
ALTER TABLE locations
  MODIFY `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification';
ALTER TABLE locations
  DROP INDEX `uq_locations_live_code`,
  DROP COLUMN `location_code_key`;
ALTER TABLE hardware_units
  DROP INDEX `uq_hw_units_type_serial`,
  MODIFY `model_id` INT UNSIGNED NOT NULL COMMENT 'FK: which product this unit is';

-- Restore the 033 trigger definitions verbatim.
DROP TRIGGER IF EXISTS `trg_hw_units_installed_target_ins`;
DROP TRIGGER IF EXISTS `trg_hw_units_installed_target_upd`;
DELIMITER //
CREATE TRIGGER `trg_hw_units_installed_target_ins` BEFORE INSERT ON `hardware_units`
FOR EACH ROW
BEGIN
  -- Compatibility window: legacy writers set only model_id.
  IF NEW.`product_type_id` IS NULL THEN
    SET NEW.`product_type_id` = (SELECT `product_type_id` FROM `hardware_models` WHERE `id`=NEW.`model_id`);
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
  -- A model change without an explicit catalog change follows the new model.
  IF NEW.`product_type_id` IS NULL
     OR (NOT (NEW.`model_id` <=> OLD.`model_id`)
         AND NEW.`product_type_id` <=> OLD.`product_type_id`) THEN
    SET NEW.`product_type_id` = (SELECT `product_type_id` FROM `hardware_models` WHERE `id`=NEW.`model_id`);
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
