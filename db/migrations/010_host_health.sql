-- 010_host_health.sql - additive host-health signal columns + seed alert rules.
--
-- Host-local health signals (fs-readonly, missing block device, SMART, failed
-- systemd units, critical dmesg lines) per docs/design/HOST_HEALTH_CONTRACT.md
-- sections 1 and 3. All-additive, NULL/0-default so existing hostCurrent rows
-- stay valid for older clients that don't yet send health TLVs.

ALTER TABLE hostCurrent
  ADD COLUMN fsReadonlyCount  TINYINT   NOT NULL DEFAULT 0,
  ADD COLUMN blockDevMissing  TINYINT   NOT NULL DEFAULT 0,
  ADD COLUMN smartFailCount   TINYINT   NOT NULL DEFAULT 0,
  ADD COLUMN failedUnitCount  SMALLINT  NOT NULL DEFAULT 0,
  ADD COLUMN dmesgCritCount   SMALLINT  NOT NULL DEFAULT 0,
  ADD COLUMN fsReadonlyList   VARCHAR(256) NOT NULL DEFAULT '',
  ADD COLUMN smartFailList    VARCHAR(256) NOT NULL DEFAULT '',
  ADD COLUMN failedUnitList   VARCHAR(256) NOT NULL DEFAULT '',
  ADD COLUMN dmesgCritSample  VARCHAR(256) NOT NULL DEFAULT '';

ALTER TABLE hostHistory
  ADD COLUMN fsReadonlyCount TINYINT NOT NULL DEFAULT 0,
  ADD COLUMN blockDevMissing TINYINT NOT NULL DEFAULT 0,
  ADD COLUMN smartFailCount  TINYINT NOT NULL DEFAULT 0,
  ADD COLUMN failedUnitCount SMALLINT NOT NULL DEFAULT 0,
  ADD COLUMN dmesgCritCount  SMALLINT NOT NULL DEFAULT 0;

-- Seed the 5 host-scope alert rules from the host-health contract. Immediate
-- sustain (forSeconds = 0) means a single breach fires right away.
INSERT INTO alertRule (scope, metric, op, threshold, forSeconds, severity, enabled) VALUES
  ('host', 'health.fsReadonly',      'gt', 0, 0, 'crit', TRUE),
  ('host', 'health.blockDevMissing', 'gt', 0, 0, 'crit', TRUE),
  ('host', 'health.smartFail',       'gt', 0, 0, 'crit', TRUE),
  ('host', 'health.failedUnits',     'gt', 0, 0, 'warn', TRUE),
  ('host', 'health.dmesgCrit',       'gt', 0, 0, 'warn', TRUE);
