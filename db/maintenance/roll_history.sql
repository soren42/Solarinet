-- SolariNet history-partition maintenance (Architecture & Plan §10/§15:
-- "history tables are append-only and partitioned by time for retention
-- pruning"; §15 task "roll history partitions").
--
-- hostHistory and probeHistory ship from db/schema.sql as:
--   PARTITION BY RANGE (TO_DAYS(sampledAt)) (
--     PARTITION p_min VALUES LESS THAN (0),
--     PARTITION p_max VALUES LESS THAN MAXVALUE )
--
-- This file replaces the catch-all p_max with monthly partitions and provides a
-- roller that (a) pre-creates monthly partitions ahead of "now" so writes never
-- fall into a soon-to-be-dropped catch-all, and (b) drops monthly partitions
-- older than the retention window with fast DDL (DROP PARTITION, not row DELETE).
--
-- Monthly partition naming: pYYYYMM, with boundary TO_DAYS(first day of the
-- *next* month) so "VALUES LESS THAN" cleanly contains the named month.
--
-- MariaDB / InnoDB. Safe and idempotent: adding an existing partition or
-- dropping an absent one is detected and skipped. Run from the same nightly job
-- as retention.sql.

DELIMITER //

-- Reorganize a table's trailing catch-all (p_max) into [monthN .. p_max] once,
-- so subsequent monthly adds can REORGANIZE p_max repeatedly. Idempotent: if the
-- requested month partition already exists, do nothing.
DROP PROCEDURE IF EXISTS solari_add_month_partition //
CREATE PROCEDURE solari_add_month_partition(IN tbl VARCHAR(64), IN monthStart DATE)
BEGIN
  DECLARE pname VARCHAR(16);
  DECLARE nextMonth DATE;
  DECLARE boundary INT;
  DECLARE existing INT DEFAULT 0;

  SET pname = CONCAT('p', DATE_FORMAT(monthStart, '%Y%m'));
  SET nextMonth = monthStart + INTERVAL 1 MONTH;
  SET boundary = TO_DAYS(nextMonth);

  SELECT COUNT(*) INTO existing
    FROM information_schema.PARTITIONS
   WHERE TABLE_SCHEMA = DATABASE()
     AND TABLE_NAME = tbl
     AND PARTITION_NAME = pname;

  IF existing = 0 THEN
    -- Split the catch-all: new month partition lands before p_max, which keeps
    -- absorbing everything beyond it.
    SET @ddl = CONCAT(
      'ALTER TABLE `', tbl, '` REORGANIZE PARTITION p_max INTO (',
      'PARTITION `', pname, '` VALUES LESS THAN (', boundary, '), ',
      'PARTITION p_max VALUES LESS THAN MAXVALUE)');
    PREPARE stmt FROM @ddl;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
  END IF;
END //

-- Drop one named monthly partition if it exists (fast retention prune).
DROP PROCEDURE IF EXISTS solari_drop_partition //
CREATE PROCEDURE solari_drop_partition(IN tbl VARCHAR(64), IN pname VARCHAR(16))
BEGIN
  DECLARE existing INT DEFAULT 0;
  SELECT COUNT(*) INTO existing
    FROM information_schema.PARTITIONS
   WHERE TABLE_SCHEMA = DATABASE()
     AND TABLE_NAME = tbl
     AND PARTITION_NAME = pname;
  IF existing > 0 THEN
    SET @ddl = CONCAT('ALTER TABLE `', tbl, '` DROP PARTITION `', pname, '`');
    PREPARE stmt FROM @ddl;
    EXECUTE stmt;
    DEALLOCATE PREPARE stmt;
  END IF;
END //

-- Drop every monthly partition (pYYYYMM) on a table whose month ends before the
-- retention cutoff. p_min and p_max are never touched.
DROP PROCEDURE IF EXISTS solari_drop_old_partitions //
CREATE PROCEDURE solari_drop_old_partitions(IN tbl VARCHAR(64), IN retentionDays INT)
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE pname VARCHAR(64);
  DECLARE bnd BIGINT;
  DECLARE cutoffDays INT;
  -- Cursor over named monthly partitions and their high-water boundary
  -- (PARTITION_DESCRIPTION is TO_DAYS(first-of-next-month) for our ranges).
  DECLARE cur CURSOR FOR
    SELECT PARTITION_NAME, CAST(PARTITION_DESCRIPTION AS UNSIGNED)
      FROM information_schema.PARTITIONS
     WHERE TABLE_SCHEMA = DATABASE()
       AND TABLE_NAME = tbl
       AND PARTITION_NAME LIKE 'p2%'        -- pYYYYMM only (not p_min/p_max)
       AND PARTITION_DESCRIPTION <> 'MAXVALUE';
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

  IF retentionDays IS NULL OR retentionDays < 1 THEN
    SET retentionDays = 90;
  END IF;
  SET cutoffDays = TO_DAYS(UTC_DATE()) - retentionDays;

  -- A partition is droppable when its entire range is below the cutoff
  -- (boundary <= cutoff), i.e. every row it holds has aged out.
  OPEN cur;
  drop_loop: LOOP
    FETCH cur INTO pname, bnd;
    IF done = 1 THEN
      LEAVE drop_loop;
    END IF;
    IF bnd <= cutoffDays THEN
      CALL solari_drop_partition(tbl, pname);
    END IF;
  END LOOP;
  CLOSE cur;
END //

-- Nightly roller: ensure partitions exist for [this month .. monthsAhead], then
-- drop monthly partitions older than retentionDays, for both history tables.
DROP PROCEDURE IF EXISTS solari_roll_partitions //
CREATE PROCEDURE solari_roll_partitions(IN retentionDays INT, IN monthsAhead INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  DECLARE m DATE;
  DECLARE firstOfMonth DATE;

  IF retentionDays IS NULL OR retentionDays < 1 THEN SET retentionDays = 90; END IF;
  IF monthsAhead   IS NULL OR monthsAhead   < 1 THEN SET monthsAhead   = 2;  END IF;

  SET firstOfMonth = DATE_FORMAT(UTC_DATE(), '%Y-%m-01');

  -- Pre-create current month + the next monthsAhead months.
  SET i = 0;
  WHILE i <= monthsAhead DO
    SET m = firstOfMonth + INTERVAL i MONTH;
    CALL solari_add_month_partition('hostHistory',  m);
    CALL solari_add_month_partition('probeHistory', m);
    SET i = i + 1;
  END WHILE;

  -- Drop months that have aged out.
  CALL solari_drop_old_partitions('hostHistory',  retentionDays);
  CALL solari_drop_old_partitions('probeHistory', retentionDays);
END //

DELIMITER ;

-- Run nightly (before or after retention.sql), e.g.:
--   mysql solarinet -e "CALL solari_roll_partitions(90, 2);"
-- CALL solari_roll_partitions(90, 2);
