-- Compensating rollback for 033_inventory_catalog.sql.
--
-- This is deliberately outside migrations/ so tools/migrate.py cannot discover
-- it. Deactivate catalog writers and take/verify a backup before use. MariaDB
-- DDL autocommits. The preflight refuses if catalog rows differ from the
-- migration backfill, any commercial barcode exists, units/models were added
-- after migration without the preserved mapping, or licence fields were used.
-- It never deletes operator-created catalog data.
--
-- After success, remove the 033 migration-ledger row manually before any
-- forward recovery. A normal deploy should repair forward instead.

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

DROP PROCEDURE IF EXISTS `_solarinet_rollback_m033_assert_reversible`;
DELIMITER //
CREATE PROCEDURE `_solarinet_rollback_m033_assert_reversible`()
BEGIN
  IF (SELECT COUNT(*) FROM `product_barcodes`) <> 0 THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='refusing rollback: product_barcodes contains post-migration data';
  END IF;

  IF EXISTS (SELECT 1 FROM `barcodes` WHERE `subject_table`='product_types') THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='refusing rollback: barcodes contains product_types subjects';
  END IF;

  IF (SELECT COUNT(*) FROM `product_types`) <> (SELECT COUNT(*) FROM `hardware_models`)
     OR EXISTS (
       SELECT 1
         FROM `product_types` pt
         LEFT JOIN `hardware_models` hm ON hm.`id`=pt.`id`
        WHERE hm.`id` IS NULL
           OR NOT (pt.`kind` <=> CASE hm.`hw_type`
                 WHEN 'printer' THEN 'peripheral'
                 WHEN 'scanner' THEN 'peripheral'
                 WHEN 'other' THEN 'other'
                 ELSE 'hardware'
               END)
           OR NOT (pt.`name` <=> hm.`model`)
           OR NOT (pt.`brand` <=> hm.`make`)
           OR NOT (pt.`model` <=> hm.`model`)
           OR NOT (pt.`category` IS NULL)
           OR NOT (pt.`specs` IS NULL)
           OR NOT (pt.`notes` <=> hm.`notes`)
           OR NOT (pt.`image_url` IS NULL)
           OR NOT (pt.`lookup_source` <=> 'migration:033')
           OR NOT (pt.`lookup_ref` <=> CONCAT('hardware_models:',hm.`id`))
           OR NOT (pt.`source_id` <=> (SELECT `id` FROM `sources` WHERE `slug`='dashboard'))
           OR NOT (pt.`asserted_kind` <=> 'human')
           OR NOT (pt.`deleted_at` <=> hm.`deleted_at`)
           OR NOT (pt.`updated_at` <=> pt.`created_at`)
           OR NOT (pt.`asserted_at` <=> pt.`created_at`)
     ) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='refusing rollback: product_types contains new or changed catalog data';
  END IF;

  IF EXISTS (SELECT 1 FROM `hardware_models` WHERE `product_type_id` IS NULL OR `product_type_id`<>`id`)
     OR EXISTS (SELECT 1 FROM `hardware_units` hu
                 WHERE hu.`product_type_id` IS NULL OR hu.`product_type_id`<>hu.`model_id`) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='refusing rollback: hardware model/unit catalog links changed after migration';
  END IF;

  IF EXISTS (SELECT 1 FROM `hardware_units`
              WHERE `licence_seats` IS NOT NULL
                 OR `licence_expires_on` IS NOT NULL
                 OR `licence_reference` IS NOT NULL
                 OR `installed_on_unit_id` IS NOT NULL) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='refusing rollback: hardware_units contains post-migration licence or installation data';
  END IF;
END//
DELIMITER ;

CALL `_solarinet_rollback_m033_assert_reversible`();
DROP PROCEDURE `_solarinet_rollback_m033_assert_reversible`;

DROP TRIGGER IF EXISTS `trg_hw_units_installed_target_ins`;
DROP TRIGGER IF EXISTS `trg_hw_units_installed_target_upd`;
DROP TRIGGER IF EXISTS `trg_product_types_no_software_install_target`;
DROP TRIGGER IF EXISTS `trg_hw_models_catalog_ins`;
DROP TRIGGER IF EXISTS `trg_hw_models_catalog_upd`;

ALTER TABLE `hardware_units`
  DROP FOREIGN KEY IF EXISTS `fk_hw_units_installed_on_unit`,
  DROP FOREIGN KEY IF EXISTS `fk_hw_units_product_type`,
  DROP INDEX IF EXISTS `ix_hw_units_installed_on_unit`,
  DROP INDEX IF EXISTS `ix_hw_units_licence_expiry`,
  DROP INDEX IF EXISTS `ix_hw_units_product_type`,
  DROP COLUMN IF EXISTS `installed_on_unit_id`,
  DROP COLUMN IF EXISTS `licence_reference`,
  DROP COLUMN IF EXISTS `licence_expires_on`,
  DROP COLUMN IF EXISTS `licence_seats`,
  DROP COLUMN IF EXISTS `product_type_id`;

ALTER TABLE `hardware_models`
  DROP FOREIGN KEY IF EXISTS `fk_hw_models_product_type`,
  DROP INDEX IF EXISTS `uq_hw_models_product_type`,
  DROP COLUMN IF EXISTS `product_type_id`;

ALTER TABLE `barcodes`
  MODIFY COLUMN `subject_table` VARCHAR(64) NOT NULL
    COMMENT 'hardware_units|locations|projects|interfaces (polymorphic, mirrors external_refs; enforced in the API)';

DROP TABLE IF EXISTS `product_barcodes`;
DROP TABLE IF EXISTS `product_types`;
