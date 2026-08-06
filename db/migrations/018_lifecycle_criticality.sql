-- Entity lifecycle and criticality.  Additive and safe while an older server
-- binary is still writing alertEvent rows (the new alert columns are nullable
-- except for their backwards-compatible eventKind default).
ALTER TABLE asset
  ADD COLUMN IF NOT EXISTS lifecycle ENUM('active','decommissioned','deleted') NOT NULL DEFAULT 'active' AFTER monitorHost,
  ADD COLUMN IF NOT EXISTS criticality TINYINT NOT NULL DEFAULT 2 AFTER lifecycle;
ALTER TABLE node
  ADD COLUMN IF NOT EXISTS criticality TINYINT NOT NULL DEFAULT 2 AFTER state;
ALTER TABLE alertEvent
  ADD COLUMN IF NOT EXISTS eventKind ENUM('fired','cleared','audit') NOT NULL DEFAULT 'fired',
  ADD COLUMN IF NOT EXISTS openedEventId BIGINT UNSIGNED NULL,
  ADD COLUMN IF NOT EXISTS assetId BIGINT UNSIGNED NULL,
  ADD COLUMN IF NOT EXISTS effectiveTier TINYINT NULL,
  ADD COLUMN IF NOT EXISTS disposition ENUM('publish','suppress') NULL;
CREATE TABLE IF NOT EXISTS probeTargetTombstone (
  targetId VARCHAR(128) PRIMARY KEY,
  removedAt DATETIME NOT NULL,
  removedBy VARCHAR(64) NOT NULL,
  assetId BIGINT UNSIGNED NULL
) ENGINE=InnoDB;
ALTER TABLE probeTargetTombstone
  ADD COLUMN IF NOT EXISTS assetId BIGINT UNSIGNED NULL;
-- MariaDB supports IF NOT EXISTS for CREATE INDEX on supported deployment
-- versions; the migration remains idempotent there.
CREATE INDEX IF NOT EXISTS ix_asset_lifecycle ON asset (lifecycle);
CREATE INDEX IF NOT EXISTS ix_alert_event_asset ON alertEvent (assetId);
CREATE INDEX IF NOT EXISTS ix_alert_event_opened ON alertEvent (openedEventId);
