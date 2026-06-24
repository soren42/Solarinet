-- 004_assets_pools.sql — monitored systems (assets) + functional pools.
--
-- A discovered/adopted system is a first-class entity that owns MANY probe
-- targets (per-service) plus an optional host heartbeat, carries operator
-- metadata (name/class/tags/notes), and belongs to a functional pool that drives
-- the top-level dashboard cards. Assets are operator configuration (like
-- alertRule / globalConfig), owned by the dashboard tier. Additive; re-runnable.

CREATE TABLE IF NOT EXISTS pool (
  poolId      INT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  name        VARCHAR(64) NOT NULL UNIQUE,
  description VARCHAR(255),
  color       VARCHAR(16),                       -- UI accent, e.g. "#35e0d0"
  createdAt   DATETIME NOT NULL
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS asset (
  assetId     BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  ip          VARCHAR(64)  NOT NULL UNIQUE,       -- stable key from discovery
  host        VARCHAR(255),                       -- fqdn / reverse-DNS
  displayName VARCHAR(128),                       -- operator-assigned name
  class       VARCHAR(32) NOT NULL DEFAULT 'host',-- server|appliance|network|iot|host|other
  poolId      INT UNSIGNED,                       -- functional pool (nullable)
  tags        JSON,                               -- ["edge","prod"]
  notes       VARCHAR(512),
  monitorHost TINYINT(1) NOT NULL DEFAULT 1,      -- ICMP heartbeat enabled
  nodeId      BIGINT UNSIGNED,                    -- linked agent (future); nullable
  createdAt   DATETIME NOT NULL,
  updatedAt   DATETIME NOT NULL,
  INDEX(poolId), INDEX(class),
  CONSTRAINT fk_asset_pool FOREIGN KEY (poolId) REFERENCES pool(poolId) ON DELETE SET NULL
) ENGINE=InnoDB;

-- Probe targets roll up to an owning asset (one system -> many service targets).
ALTER TABLE probeTarget ADD COLUMN IF NOT EXISTS assetId BIGINT UNSIGNED;
ALTER TABLE probeTarget ADD INDEX IF NOT EXISTS idx_probeTarget_asset (assetId);

-- Default home for newly adopted systems.
INSERT IGNORE INTO pool (poolId, name, description, color, createdAt)
  VALUES (1, 'Unassigned', 'Systems not yet assigned to a pool', '#7c8aa0', UTC_TIMESTAMP());
