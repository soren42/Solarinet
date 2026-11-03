-- 036: normalize location levels and add live sibling uniqueness, container
-- metadata, project assignment, and parent/level invariants.
SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

-- Preflight all data-dependent constraints before changing the schema. 9104
-- is soft-deleted by D6: it maps like 9103, but contributes NULL to the live
-- name key and therefore does not claim the root sibling name.
DROP PROCEDURE IF EXISTS `_solarinet_m036_preflight`;
DELIMITER //
CREATE PROCEDURE `_solarinet_m036_preflight`()
BEGIN
  DECLARE msg VARCHAR(255);
  DECLARE start_id INT UNSIGNED;
  DECLARE walk_id INT UNSIGNED;
  DECLARE walk_parent INT UNSIGNED;
  DECLARE n INT DEFAULT 0;
  DECLARE finished INT DEFAULT 0;
  DECLARE locs CURSOR FOR SELECT id FROM locations;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET finished=1;
  IF EXISTS (
    SELECT 1 FROM locations l LEFT JOIN locations p ON p.id=l.parent_id
     WHERE l.parent_id IS NOT NULL AND p.id IS NULL
  ) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='036 preflight: orphan location parent';
  END IF;
  IF EXISTS (SELECT 1 FROM locations WHERE id=0) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='036 preflight: location id 0 conflicts with root sentinel';
  END IF;
  -- Walk every ancestor chain. The visited table makes arbitrary cycles,
  -- including cycles longer than two rows, a deterministic refusal.
  DROP TEMPORARY TABLE IF EXISTS `_m036_seen`;
  CREATE TEMPORARY TABLE `_m036_seen` (id INT UNSIGNED PRIMARY KEY) ENGINE=MEMORY;
  OPEN locs;
  location_loop: LOOP
    FETCH locs INTO start_id;
    IF finished THEN LEAVE location_loop; END IF;
    DELETE FROM `_m036_seen`;
    SET walk_id=start_id;
    SET n=0;
    chain_loop: WHILE walk_id IS NOT NULL DO
      IF EXISTS (SELECT 1 FROM `_m036_seen` WHERE id=walk_id) THEN
        CLOSE locs;
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='036 preflight: location parent cycle';
      END IF;
      INSERT INTO `_m036_seen` VALUES (walk_id);
      SET n=n+1;
      IF n>10000 THEN
        CLOSE locs;
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='036 preflight: location parent chain exceeds 10000';
      END IF;
      SET walk_parent=NULL;
      SELECT parent_id INTO walk_parent FROM locations WHERE id=walk_id;
      SET walk_id=walk_parent;
    END WHILE;
  END LOOP;
  CLOSE locs;
  DROP TEMPORARY TABLE `_m036_seen`;
  IF EXISTS (
    SELECT 1 FROM locations l
    WHERE (l.parent_id IS NULL AND CASE l.loc_type WHEN 'site' THEN 'property' WHEN 'building' THEN 'property' ELSE l.loc_type END <> 'property')
       OR (l.parent_id IS NOT NULL AND l.loc_type NOT IN ('rack','shelf','position','zone','cabinet','drawer','tray','bin','container') AND NOT (
         (CASE WHEN l.loc_type IN ('site','building') THEN 'property' ELSE l.loc_type END='floor'
            AND (SELECT p.loc_type FROM locations p WHERE p.id=l.parent_id) IN ('site','building')) OR
         (l.loc_type='room' AND (SELECT p.loc_type FROM locations p WHERE p.id=l.parent_id)='floor') OR
         (l.loc_type='section' AND (SELECT p.loc_type FROM locations p WHERE p.id=l.parent_id)='room')
       ))
  ) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='036 preflight: invalid location parent/level transition';
  END IF;
  SELECT GROUP_CONCAT(CONCAT(parent_id,':',name) ORDER BY parent_id,name SEPARATOR ',') INTO msg
    FROM (SELECT IFNULL(parent_id,0) parent_id,name FROM locations
           WHERE deleted_at IS NULL GROUP BY IFNULL(parent_id,0),name HAVING COUNT(*)>1) d;
  IF msg IS NOT NULL THEN
    SET msg=LEFT(CONCAT('036 preflight: live sibling duplicates ',msg),255);
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT=msg;
  END IF;
  SELECT GROUP_CONCAT(code ORDER BY code SEPARATOR ',') INTO msg
    FROM (SELECT code FROM locations WHERE code IS NOT NULL GROUP BY code HAVING COUNT(*)>1) d;
  IF msg IS NOT NULL THEN
    SET msg=LEFT(CONCAT('036 preflight: duplicate location codes ',msg),255);
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT=msg;
  END IF;
END//
DELIMITER ;
CALL `_solarinet_m036_preflight`();
DROP PROCEDURE `_solarinet_m036_preflight`;

-- Rollback guard snapshot: preserve enough information to prove every
-- pre-existing location is untouched before restoring its original enum.
DROP TABLE IF EXISTS `_solarinet_m036_location_baseline`;
CREATE TABLE `_solarinet_m036_location_baseline` (
  id INT UNSIGNED NOT NULL PRIMARY KEY,
  original_type VARCHAR(32) NOT NULL,
  expected_type VARCHAR(32) NOT NULL,
  parent_id INT UNSIGNED NULL,
  name VARCHAR(128) NOT NULL,
  code VARCHAR(32) NULL,
  notes TEXT NULL,
  deleted_at DATETIME NULL,
  expected_updated_at DATETIME NOT NULL
) ENGINE=InnoDB;
INSERT INTO `_solarinet_m036_location_baseline`
  SELECT id,loc_type,CASE WHEN loc_type IN ('site','building') THEN 'property'
         WHEN loc_type IN ('floor','room') THEN loc_type ELSE 'storage_unit' END,
         parent_id,name,code,notes,deleted_at,updated_at FROM locations;

ALTER TABLE locations MODIFY loc_type ENUM('site','building','property','floor','room','section','rack','shelf','position','zone','cabinet','drawer','tray','bin','container','storage_unit') NOT NULL;
UPDATE locations SET loc_type=CASE
  WHEN loc_type IN ('site','building') THEN 'property'
  WHEN loc_type IN ('floor','room') THEN loc_type
  ELSE 'storage_unit' END;
ALTER TABLE locations MODIFY loc_type ENUM('property','floor','room','section','storage_unit') NOT NULL;
UPDATE `_solarinet_m036_location_baseline` b JOIN locations l ON l.id=b.id
   SET b.expected_updated_at=l.updated_at;

CREATE TABLE IF NOT EXISTS container_types (
  id INT UNSIGNED NOT NULL AUTO_INCREMENT,
  name VARCHAR(64) NOT NULL,
  name_key VARCHAR(64) GENERATED ALWAYS AS (IF(deleted_at IS NULL,name,NULL)) PERSISTENT,
  source_id SMALLINT UNSIGNED NOT NULL,
  asserted_kind ENUM('human','machine') NOT NULL DEFAULT 'human',
  asserted_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  deleted_at DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (id), UNIQUE KEY uq_container_types_live_name (name_key),
  KEY ix_container_types_source (source_id),
  CONSTRAINT fk_container_types_source FOREIGN KEY (source_id) REFERENCES sources(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Editable types for reusable storage-unit locations';

SET @old_parent_unique := (SELECT COUNT(*) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='locations' AND INDEX_NAME='uq_locations_parent_name');
SET @sql := IF(@old_parent_unique>0,'ALTER TABLE locations DROP INDEX uq_locations_parent_name','DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;
ALTER TABLE locations
  DROP INDEX idx_locations_parent_name,
  ADD COLUMN parent_key INT UNSIGNED GENERATED ALWAYS AS (IFNULL(parent_id,0)) PERSISTENT,
  ADD COLUMN location_name_key VARCHAR(128) GENERATED ALWAYS AS (IF(deleted_at IS NULL,name,NULL)) PERSISTENT,
  ADD COLUMN container_type_id INT UNSIGNED NULL,
  ADD COLUMN assigned_project_id INT UNSIGNED NULL,
  ADD UNIQUE KEY uq_locations_live_sibling (parent_key,location_name_key),
  ADD KEY ix_locations_parent (parent_id),
  ADD KEY ix_locations_type_live (loc_type,deleted_at),
  ADD KEY ix_locations_container_type (container_type_id),
  ADD KEY ix_locations_project_live (assigned_project_id,deleted_at),
  ADD CONSTRAINT fk_locations_container_type FOREIGN KEY (container_type_id) REFERENCES container_types(id) ON DELETE RESTRICT,
  ADD CONSTRAINT fk_locations_assigned_project FOREIGN KEY (assigned_project_id) REFERENCES projects(id) ON DELETE SET NULL;

DROP TRIGGER IF EXISTS trg_locations_nonzero_id;
DROP TRIGGER IF EXISTS trg_locations_level_ins;
DROP TRIGGER IF EXISTS trg_locations_level_upd;
DELIMITER //
CREATE TRIGGER trg_locations_nonzero_id AFTER INSERT ON locations FOR EACH ROW
BEGIN
  IF NEW.id=0 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='location id 0 is reserved for the root parent key'; END IF;
END//
CREATE TRIGGER trg_locations_level_ins BEFORE INSERT ON locations FOR EACH ROW
BEGIN
  DECLARE parent_type VARCHAR(32);
  IF NEW.parent_id IS NULL THEN
    IF NEW.loc_type <> 'property' THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='location root must be property'; END IF;
  ELSE
    SELECT loc_type INTO parent_type FROM locations WHERE id=NEW.parent_id;
    IF NEW.loc_type='storage_unit' THEN SET parent_type=parent_type;
    ELSEIF NOT ((parent_type='property' AND NEW.loc_type='floor') OR (parent_type='floor' AND NEW.loc_type='room') OR (parent_type='room' AND NEW.loc_type='section')) THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='invalid location parent/level transition';
    END IF;
  END IF;
END//
CREATE TRIGGER trg_locations_level_upd BEFORE UPDATE ON locations FOR EACH ROW
BEGIN
  DECLARE parent_type VARCHAR(32);
  DECLARE child_bad INT DEFAULT 0;
  DECLARE cursor_id INT UNSIGNED;
  DECLARE next_parent INT UNSIGNED;
  DECLARE steps INT DEFAULT 0;
  IF NEW.parent_id IS NULL THEN
    IF NEW.loc_type <> 'property' THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='location root must be property'; END IF;
  ELSE
    SET cursor_id=NEW.parent_id;
    ancestor_walk: WHILE cursor_id IS NOT NULL DO
      IF cursor_id=NEW.id THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='location parent cycle'; END IF;
      SET steps=steps+1;
      IF steps>10000 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='location parent cycle or excessive depth'; END IF;
      SET parent_type=NULL;
      SELECT parent_id,loc_type INTO next_parent,parent_type FROM locations WHERE id=cursor_id;
      IF parent_type IS NULL THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='location parent does not exist'; END IF;
      SET cursor_id=next_parent;
    END WHILE;
    SELECT loc_type INTO parent_type FROM locations WHERE id=NEW.parent_id;
    IF NEW.loc_type<>'storage_unit' AND NOT ((parent_type='property' AND NEW.loc_type='floor') OR (parent_type='floor' AND NEW.loc_type='room') OR (parent_type='room' AND NEW.loc_type='section')) THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='invalid location parent/level transition';
    END IF;
  END IF;
  SELECT COUNT(*) INTO child_bad FROM locations c WHERE c.parent_id=OLD.id AND NOT (
    c.loc_type='storage_unit' OR
    (NEW.loc_type='property' AND c.loc_type='floor') OR
    (NEW.loc_type='floor' AND c.loc_type='room') OR
    (NEW.loc_type='room' AND c.loc_type='section')
  );
  IF child_bad>0 THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='location update invalidates child level'; END IF;
END//
DELIMITER ;
