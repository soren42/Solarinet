-- Compensating rollback for 036_inventory_locations.sql. This only restores
-- the prior location schema when every location still matches the migration
-- baseline and no container/project metadata was added. It never deletes
-- operator data.
SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

DROP PROCEDURE IF EXISTS `_solarinet_rollback_m036_assert_reversible`;
DELIMITER //
CREATE PROCEDURE `_solarinet_rollback_m036_assert_reversible`()
BEGIN
  IF NOT EXISTS (SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='_solarinet_m036_location_baseline') THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='refusing rollback: 036 location baseline missing';
  END IF;
  IF (SELECT COUNT(*) FROM locations) <> (SELECT COUNT(*) FROM `_solarinet_m036_location_baseline`)
     OR EXISTS (
       SELECT 1 FROM locations l LEFT JOIN `_solarinet_m036_location_baseline` b ON b.id=l.id
        WHERE b.id IS NULL OR l.parent_id<>b.parent_id AND NOT (l.parent_id IS NULL AND b.parent_id IS NULL)
           OR NOT (l.name <=> b.name) OR NOT (l.code <=> b.code)
           OR NOT (l.notes <=> b.notes) OR NOT (l.deleted_at <=> b.deleted_at)
           OR NOT (l.loc_type <=> b.expected_type)
           OR NOT (l.updated_at <=> b.expected_updated_at)
     ) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='refusing rollback: location rows changed or added after 036';
  END IF;
  IF (SELECT COUNT(*) FROM container_types)<>0 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='refusing rollback: container_types contains post-migration data';
  END IF;
  IF EXISTS (SELECT 1 FROM locations WHERE container_type_id IS NOT NULL OR assigned_project_id IS NOT NULL) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='refusing rollback: location metadata contains post-migration data';
  END IF;
END//
DELIMITER ;
CALL `_solarinet_rollback_m036_assert_reversible`();
DROP PROCEDURE `_solarinet_rollback_m036_assert_reversible`;

DROP TRIGGER IF EXISTS trg_locations_nonzero_id;
DROP TRIGGER IF EXISTS trg_locations_level_ins;
DROP TRIGGER IF EXISTS trg_locations_level_upd;
ALTER TABLE locations
  DROP FOREIGN KEY fk_locations_container_type,
  DROP FOREIGN KEY fk_locations_assigned_project,
  DROP INDEX uq_locations_live_sibling,
  DROP INDEX ix_locations_parent,
  DROP INDEX ix_locations_type_live,
  DROP INDEX ix_locations_container_type,
  DROP INDEX ix_locations_project_live,
  DROP COLUMN parent_key,
  DROP COLUMN location_name_key,
  DROP COLUMN container_type_id,
  DROP COLUMN assigned_project_id,
  ADD KEY idx_locations_parent_name (parent_id,name),
  MODIFY loc_type ENUM('property','floor','room','section','storage_unit','site','building','rack','shelf','position','zone','cabinet','drawer','tray','bin','container') NOT NULL;
UPDATE locations l JOIN `_solarinet_m036_location_baseline` b ON b.id=l.id
   SET l.loc_type=b.original_type;
ALTER TABLE locations
  MODIFY loc_type ENUM('site','building','floor','room','rack','shelf','position','zone','cabinet','drawer','tray','bin','container') NOT NULL;
DROP TABLE container_types;
DROP TABLE `_solarinet_m036_location_baseline`;
