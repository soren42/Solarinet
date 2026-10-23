-- 028_entity_unit_association.sql — enforce explicit SoR entity/unit links
--
-- Retain entities.hardware_unit_id for compatibility, and mirror its current
-- live associations into a relation table whose keys enforce one-to-one
-- cardinality. Triggers keep the registry synchronized with all writers.

DROP PROCEDURE IF EXISTS `_solarinet_m028_assert_no_duplicate_units`;
DELIMITER //
CREATE PROCEDURE `_solarinet_m028_assert_no_duplicate_units`()
BEGIN
  IF EXISTS (
    SELECT hardware_unit_id FROM entities
     WHERE hardware_unit_id IS NOT NULL AND deleted_at IS NULL
     GROUP BY hardware_unit_id HAVING COUNT(*) > 1
  ) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'entities contains duplicate live hardware_unit_id associations';
  END IF;
END//
DELIMITER ;
CALL `_solarinet_m028_assert_no_duplicate_units`();
DROP PROCEDURE `_solarinet_m028_assert_no_duplicate_units`;

CREATE TABLE IF NOT EXISTS `entity_hardware_unit_links` (
  `hardware_unit_id` INT UNSIGNED NOT NULL,
  `entity_id` BIGINT UNSIGNED NOT NULL,
  `linked_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`hardware_unit_id`),
  UNIQUE KEY `uq_entity_hardware_unit_entity` (`entity_id`),
  CONSTRAINT `fk_entity_hardware_unit_link_unit` FOREIGN KEY (`hardware_unit_id`) REFERENCES `hardware_units` (`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_entity_hardware_unit_link_entity` FOREIGN KEY (`entity_id`) REFERENCES `entities` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Current one-to-one association of physical units to SoR entities';

-- Backfill once, and make reapplication harmless. Conflicting historical
-- associations are rejected by the unique keys, never silently reassigned.
INSERT INTO `entity_hardware_unit_links` (`hardware_unit_id`, `entity_id`)
SELECT e.hardware_unit_id, e.id
  FROM entities e
  LEFT JOIN entity_hardware_unit_links l ON l.hardware_unit_id=e.hardware_unit_id
 WHERE e.hardware_unit_id IS NOT NULL AND e.deleted_at IS NULL
   AND l.hardware_unit_id IS NULL;

DELIMITER //
CREATE OR REPLACE TRIGGER `trg_entities_hu_link_ai`
AFTER INSERT ON `entities` FOR EACH ROW
BEGIN
  IF NEW.hardware_unit_id IS NOT NULL AND NEW.deleted_at IS NULL THEN
    IF EXISTS (
      SELECT 1 FROM entity_hardware_unit_links
       WHERE hardware_unit_id=NEW.hardware_unit_id AND entity_id<>NEW.id
    ) THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'physical unit is already linked to another live entity';
    END IF;
    INSERT INTO entity_hardware_unit_links (hardware_unit_id, entity_id)
    VALUES (NEW.hardware_unit_id, NEW.id);
  END IF;
END//

CREATE OR REPLACE TRIGGER `trg_entities_hu_link_au`
AFTER UPDATE ON `entities` FOR EACH ROW
BEGIN
  IF NOT (OLD.hardware_unit_id <=> NEW.hardware_unit_id)
     OR ((OLD.deleted_at IS NULL) <> (NEW.deleted_at IS NULL)) THEN
    IF NEW.hardware_unit_id IS NOT NULL AND NEW.deleted_at IS NULL
       AND EXISTS (
         SELECT 1 FROM entity_hardware_unit_links
          WHERE hardware_unit_id=NEW.hardware_unit_id AND entity_id<>NEW.id
       ) THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'physical unit is already linked to another live entity';
    END IF;
    DELETE FROM entity_hardware_unit_links WHERE entity_id=OLD.id;
    IF NEW.hardware_unit_id IS NOT NULL AND NEW.deleted_at IS NULL THEN
      INSERT INTO entity_hardware_unit_links (hardware_unit_id, entity_id)
      VALUES (NEW.hardware_unit_id, NEW.id);
    END IF;
  END IF;
END//
DELIMITER ;

DROP PROCEDURE IF EXISTS `_solarinet_m028_assert_registry_consistent`;
DELIMITER //
CREATE PROCEDURE `_solarinet_m028_assert_registry_consistent`()
BEGIN
  IF EXISTS (
    SELECT 1 FROM entities e
    LEFT JOIN entity_hardware_unit_links l
      ON l.hardware_unit_id=e.hardware_unit_id AND l.entity_id=e.id
   WHERE e.hardware_unit_id IS NOT NULL AND e.deleted_at IS NULL
     AND l.entity_id IS NULL
  ) OR EXISTS (
    SELECT 1 FROM entity_hardware_unit_links l
    LEFT JOIN entities e ON e.id=l.entity_id
   WHERE e.id IS NULL OR e.deleted_at IS NOT NULL OR e.hardware_unit_id<>l.hardware_unit_id
  ) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'entity_hardware_unit_links is inconsistent with live entities';
  END IF;
END//
DELIMITER ;
CALL `_solarinet_m028_assert_registry_consistent`();
DROP PROCEDURE `_solarinet_m028_assert_registry_consistent`;
