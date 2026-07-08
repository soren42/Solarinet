-- 014_maintenance_windows.sql — planned maintenance / outage windows.
--
-- A host (or the whole fleet) under an active window is expected to be down:
-- the alert-bridge suppresses its notifications AND the dead-man's-switch, and
-- Opie skips investigating it. So powering benzene down for an eGPU install
-- doesn't crit-page you. Lives in the solarinet DB (what the bridge/opied read).
-- Additive + idempotent.

CREATE TABLE IF NOT EXISTS maintenanceWindow (
  windowId   INT AUTO_INCREMENT PRIMARY KEY,
  scope      ENUM('node','all') NOT NULL DEFAULT 'node',
  nodeId     BIGINT UNSIGNED NULL,        -- host under maintenance (NULL when scope='all')
  hostFqdn   VARCHAR(255) NULL,           -- denormalized for display/matching
  ipAddr     VARCHAR(45) NULL,            -- for matching probe-target alerts (non-node hosts like benzene)
  reason     VARCHAR(255) NULL,
  startsAt   DATETIME NOT NULL,
  endsAt     DATETIME NOT NULL,
  status     ENUM('scheduled','active','completed','cancelled') NOT NULL DEFAULT 'scheduled',
  createdBy  VARCHAR(64) NULL,
  createdAt  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updatedAt  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  KEY idx_window_time (startsAt, endsAt),
  KEY idx_window_node (nodeId),
  KEY idx_window_host (hostFqdn)
);

-- A window is IN EFFECT for a node right now when it is not cancelled/completed,
-- NOW() is within [startsAt, endsAt], and it either targets that node (by nodeId
-- or hostFqdn) or the whole fleet (scope='all'). The bridge/opied use this shape.
