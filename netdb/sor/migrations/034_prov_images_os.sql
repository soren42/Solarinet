-- 034_prov_images_os.sql — make the OS catalogue the canonical distro identity.
--
-- prov_images.distro/release_name remain provisioner handles used in fetch
-- URLs, paths and installer arguments. os_id is the catalogue identity used
-- by SoR entities. Restrict hard deletion of a referenced OS; the nullable
-- column supports migration/rollback and unknown future image rows.

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

ALTER TABLE `prov_images`
  ADD COLUMN IF NOT EXISTS `os_id` INT UNSIGNED NULL
    COMMENT 'canonical operating_systems identity; distro/release_name remain pipeline handles'
    AFTER `id`;

SET @idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
              WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='prov_images'
                AND INDEX_NAME='ix_prov_images_os');
SET @sql := IF(@idx=0,
  'ALTER TABLE `prov_images` ADD KEY `ix_prov_images_os` (`os_id`)', 'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
             WHERE CONSTRAINT_SCHEMA=DATABASE() AND TABLE_NAME='prov_images'
               AND CONSTRAINT_NAME='fk_prov_images_os' AND CONSTRAINT_TYPE='FOREIGN KEY');
SET @sql := IF(@fk=0,
  'ALTER TABLE `prov_images` ADD CONSTRAINT `fk_prov_images_os` FOREIGN KEY (`os_id`) REFERENCES `operating_systems` (`id`) ON DELETE RESTRICT', 'DO 0');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

-- A deterministic canonical key is derived from the image's human-facing
-- release label where it is the catalogue's version vocabulary (Tumbleweed),
-- otherwise from the pipeline's release_name (e.g. 24.04, 13, bookworm).
-- Known distro handles are explicit; unknown handles stop the migration.
DROP TEMPORARY TABLE IF EXISTS `_m034_image_os`;
CREATE TEMPORARY TABLE `_m034_image_os` (
  `image_id` INT UNSIGNED NOT NULL PRIMARY KEY,
  `os_name` VARCHAR(128) NOT NULL,
  `os_version` VARCHAR(64) NOT NULL
) ENGINE=InnoDB;

INSERT INTO `_m034_image_os` (`image_id`,`os_name`,`os_version`)
SELECT i.`id`,
       CASE i.`distro`
         WHEN 'opensuse' THEN IF(i.`release_name`='tumbleweed','openSUSE','openSUSE Leap')
         WHEN 'debian' THEN 'Debian'
         WHEN 'ubuntu' THEN 'Ubuntu'
         WHEN 'raspios' THEN 'Raspberry Pi OS'
         WHEN 'fedora' THEN 'Fedora'
         ELSE CONCAT('!unknown:', i.`distro`)
       END,
       -- Versions use the os-release VERSION_ID vocabulary (Pi OS bookworm
       -- reports 12), so catalogue rows match what hosts observe.
       CASE
         WHEN i.`distro`='opensuse' AND i.`release_label`='Tumbleweed'
           AND i.`release_name`='tumbleweed' THEN 'Tumbleweed'
         WHEN i.`distro`='raspios' AND i.`release_name`='bookworm' THEN '12'
         WHEN i.`distro`='raspios' AND i.`release_name`='trixie' THEN '13'
         ELSE i.`release_name`
       END
  FROM `prov_images` i;

DROP PROCEDURE IF EXISTS `_solarinet_m034_assert_map`;
DELIMITER //
CREATE PROCEDURE `_solarinet_m034_assert_map`()
BEGIN
  IF NOT EXISTS (SELECT 1 FROM `sources` WHERE `slug`='manual' AND `deleted_at` IS NULL) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='034 requires the live manual provenance source';
  END IF;
  IF EXISTS (SELECT 1 FROM `_m034_image_os` WHERE `os_name` LIKE '!unknown:%') THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='034 ambiguous/unrecognized prov_images distro handle; add an explicit OS mapping';
  END IF;
  IF EXISTS (
    SELECT m.`image_id`
      FROM `_m034_image_os` m
      JOIN `operating_systems` os
        ON os.`name`=m.`os_name` AND os.`version`=m.`os_version`
     GROUP BY m.`image_id`
    HAVING COUNT(*)>1
  ) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='034 ambiguous operating_systems match for prov_images row';
  END IF;
  IF EXISTS (
    SELECT 1 FROM `_m034_image_os` m
    JOIN `operating_systems` os
      ON os.`name`=m.`os_name` AND os.`version`=m.`os_version`
     WHERE os.`deleted_at` IS NOT NULL
  ) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='034 matching operating_systems row is soft-deleted; resolve ambiguity before migration';
  END IF;
END//
DELIMITER ;
CALL `_solarinet_m034_assert_map`();
DROP PROCEDURE `_solarinet_m034_assert_map`;

-- Insert only absent catalogue identities. These are canonical Linux rows,
-- explicitly human-asserted by the existing manual source.
INSERT INTO `operating_systems`
  (`family`,`name`,`version`,`source_id`,`asserted_kind`)
SELECT 'linux', m.`os_name`, m.`os_version`, src.`id`, 'human'
  FROM (SELECT DISTINCT `os_name`,`os_version` FROM `_m034_image_os`) m
  JOIN `sources` src ON src.`slug`='manual' AND src.`deleted_at` IS NULL
 WHERE NOT EXISTS (
   SELECT 1 FROM `operating_systems` os
    WHERE os.`name`=m.`os_name` AND os.`version`=m.`os_version`
      AND os.`deleted_at` IS NULL
 );

UPDATE `prov_images` i
JOIN `_m034_image_os` m ON m.`image_id`=i.`id`
JOIN `operating_systems` os
  ON os.`name`=m.`os_name` AND os.`version`=m.`os_version`
 AND os.`deleted_at` IS NULL
   SET i.`os_id`=os.`id`
 WHERE i.`os_id` IS NULL;

DROP PROCEDURE IF EXISTS `_solarinet_m034_assert_complete`;
DELIMITER //
CREATE PROCEDURE `_solarinet_m034_assert_complete`()
BEGIN
  IF EXISTS (SELECT 1 FROM `prov_images` WHERE `os_id` IS NULL) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT='034 left one or more prov_images without an operating-system mapping';
  END IF;
END//
DELIMITER ;
CALL `_solarinet_m034_assert_complete`();
DROP PROCEDURE `_solarinet_m034_assert_complete`;

-- Every image now has a catalogue identity; keep it that way so a
-- successful reimage can always record what was installed.
ALTER TABLE `prov_images` MODIFY COLUMN `os_id` INT UNSIGNED NOT NULL
  COMMENT 'canonical operating_systems identity; distro/release_name remain pipeline handles';

DROP TEMPORARY TABLE IF EXISTS `_m034_image_os`;
