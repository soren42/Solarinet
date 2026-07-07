-- 010_host_health.sql - additive host-health signal columns + seed alert rules.
--
-- Host-local health signals (fs-readonly, missing block device, SMART, failed
-- systemd units, critical dmesg lines) per docs/design/HOST_HEALTH_CONTRACT.md
-- sections 1 and 3. All-additive, NULL/0-default so existing hostCurrent rows
-- stay valid for older clients that don't yet send health TLVs.

ALTER TABLE hostCurrent
  ADD COLUMN IF NOT EXISTS fsReadonlyCount  TINYINT UNSIGNED   NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS blockDevMissing  TINYINT UNSIGNED   NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS smartFailCount   TINYINT UNSIGNED   NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS failedUnitCount  SMALLINT UNSIGNED  NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS dmesgCritCount   SMALLINT UNSIGNED  NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS fsReadonlyList   VARCHAR(256) NOT NULL DEFAULT '',
  ADD COLUMN IF NOT EXISTS smartFailList    VARCHAR(256) NOT NULL DEFAULT '',
  ADD COLUMN IF NOT EXISTS failedUnitList   VARCHAR(256) NOT NULL DEFAULT '',
  ADD COLUMN IF NOT EXISTS dmesgCritSample  VARCHAR(256) NOT NULL DEFAULT '';

ALTER TABLE hostHistory
  ADD COLUMN IF NOT EXISTS fsReadonlyCount TINYINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS blockDevMissing TINYINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS smartFailCount  TINYINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS failedUnitCount SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS dmesgCritCount  SMALLINT UNSIGNED NOT NULL DEFAULT 0;

ALTER TABLE alertRule
  ADD UNIQUE KEY IF NOT EXISTS uq_alertrule_scope_metric (scope, metric);

-- Seed the 5 host-scope alert rules from the host-health contract. Immediate
-- sustain (forSeconds = 0) means a single breach fires right away.
INSERT INTO alertRule (scope, metric, op, threshold, forSeconds, severity, enabled) VALUES
  ('host', 'health.fsReadonly',      'gt', 0, 0, 'crit', TRUE),
  ('host', 'health.blockDevMissing', 'gt', 0, 0, 'crit', TRUE),
  ('host', 'health.smartFail',       'gt', 0, 0, 'crit', TRUE),
  ('host', 'health.failedUnits',     'gt', 0, 0, 'warn', TRUE),
  ('host', 'health.dmesgCrit',       'gt', 0, 0, 'warn', TRUE)
ON DUPLICATE KEY UPDATE
  threshold = VALUES(threshold),
  op = VALUES(op),
  forSeconds = VALUES(forSeconds),
  severity = VALUES(severity),
  enabled = VALUES(enabled);
