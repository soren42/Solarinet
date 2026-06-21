-- SolariNet retention maintenance (Architecture & Plan §15; Phase 3 Handoff
-- §5 note: "A nightly job may purge retired rows older than the retention
-- window.").
--
-- Two responsibilities, both idempotent and safe to run from cron / a systemd
-- timer (see db/maintenance/README is not needed - this file is self-documenting):
--
--   1. Purge nodes left in state='retired' (set by a completed decommission,
--      Handoff §4.2/§7.3) whose lastSeenAt is older than the retention window.
--      Current-state child rows (hostCurrent, procCurrent) cascade via their
--      FKs; probeCurrent/probeHistory are keyed by targetId, not nodeId, so
--      they are pruned by the partition roller (roll_history.sql) on time.
--
--   2. Prune append-only history rows older than the retention window. The bulk
--      drop happens at the partition level (roll_history.sql, fast DDL); this
--      procedure is the row-level safety net for any history not yet rolled off
--      a partition boundary, and for installs that disabled partition rolling.
--
-- MariaDB / InnoDB. All times UTC. Retention defaults to 90 days; override by
-- passing a different value to the procedure.

DELIMITER //

-- Purge retired nodes (and, by FK cascade, their current-state rows) whose last
-- contact predates the retention window. The node row itself is kept until then
-- so the fleet's recent past is never silently rewritten (Handoff §5).
DROP PROCEDURE IF EXISTS solari_purge_retired //
CREATE PROCEDURE solari_purge_retired(IN retentionDays INT)
BEGIN
  IF retentionDays IS NULL OR retentionDays < 1 THEN
    SET retentionDays = 90;
  END IF;

  -- Child history rows for the doomed nodes (no FK on hostHistory.nodeId).
  DELETE FROM hostHistory
   WHERE nodeId IN (
     SELECT nodeId FROM node
      WHERE state = 'retired'
        AND lastSeenAt IS NOT NULL
        AND lastSeenAt < (UTC_TIMESTAMP() - INTERVAL retentionDays DAY)
   );

  -- nodeConfig has no ON DELETE CASCADE in the baseline; clear it explicitly.
  DELETE FROM nodeConfig
   WHERE nodeId IN (
     SELECT nodeId FROM node
      WHERE state = 'retired'
        AND lastSeenAt IS NOT NULL
        AND lastSeenAt < (UTC_TIMESTAMP() - INTERVAL retentionDays DAY)
   );

  -- The node row last: hostCurrent / procCurrent cascade via FK_hc_node etc.
  DELETE FROM node
   WHERE state = 'retired'
     AND lastSeenAt IS NOT NULL
     AND lastSeenAt < (UTC_TIMESTAMP() - INTERVAL retentionDays DAY);

  SELECT ROW_COUNT() AS purgedNodes, retentionDays AS retentionDays;
END //

-- Row-level history prune safety net (partition rolling does the bulk work).
DROP PROCEDURE IF EXISTS solari_purge_history //
CREATE PROCEDURE solari_purge_history(IN retentionDays INT)
BEGIN
  IF retentionDays IS NULL OR retentionDays < 1 THEN
    SET retentionDays = 90;
  END IF;

  DELETE FROM hostHistory
   WHERE sampledAt < (UTC_TIMESTAMP() - INTERVAL retentionDays DAY);
  DELETE FROM probeHistory
   WHERE sampledAt < (UTC_TIMESTAMP() - INTERVAL retentionDays DAY);

  -- Stale current-state rows from nodes that have gone away (defensive).
  DELETE FROM probeCurrent
   WHERE sampledAt < (UTC_TIMESTAMP() - INTERVAL retentionDays DAY);
END //

-- One-shot nightly entry point: purge retired nodes, then trim history.
DROP PROCEDURE IF EXISTS solari_retention_run //
CREATE PROCEDURE solari_retention_run(IN retentionDays INT)
BEGIN
  CALL solari_purge_retired(retentionDays);
  CALL solari_purge_history(retentionDays);
END //

DELIMITER ;

-- Run nightly, e.g. from cron / a systemd timer:
--   mysql solarinet -e "CALL solari_retention_run(90);"
-- CALL solari_retention_run(90);
