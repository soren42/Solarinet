-- SolariNet base database schema (Architecture & Plan §10).
--
-- The contract boundary between the C server and the PHP dashboard. Current-
-- state tables are upserted; history tables are append-only and partitioned by
-- time for retention pruning. MariaDB / InnoDB. All time stored UTC.
--
-- This file is the canonical full baseline. db/migrations/001_baseline.sql is
-- the same content as a numbered migration; db/migrations/002_c2_capabilities.sql
-- (Phase 3 Handoff §5) extends it additively.

CREATE TABLE node (
  nodeId        BIGINT UNSIGNED PRIMARY KEY,   -- FNV-1a-64 (§5.5)
  role          ENUM('client','monitor','server') NOT NULL,
  hostFqdn      VARCHAR(255) NOT NULL,
  certCn        VARCHAR(255) NOT NULL,
  osName        VARCHAR(64),
  arch          VARCHAR(16),
  enrolledAt    DATETIME NOT NULL,
  lastSeenAt    DATETIME,
  configEpoch   BIGINT UNSIGNED NOT NULL DEFAULT 0,
  state         ENUM('up','degraded','down','unknown') NOT NULL DEFAULT 'unknown',
  INDEX(role), INDEX(lastSeenAt)
) ENGINE=InnoDB;

-- current host metrics (one row per client node, upserted)
CREATE TABLE hostCurrent (
  nodeId        BIGINT UNSIGNED PRIMARY KEY,
  sampledAt     DATETIME NOT NULL,
  cpuLoadMilli  JSON,                  -- per-core array
  ramUsedKb     BIGINT UNSIGNED, ramTotalKb BIGINT UNSIGNED,
  swapUsedKb    BIGINT UNSIGNED, swapTotalKb BIGINT UNSIGNED,
  disks         JSON, ifaces JSON, usbBuses JSON,
  CONSTRAINT FK_hc_node FOREIGN KEY(nodeId) REFERENCES node(nodeId) ON DELETE CASCADE
) ENGINE=InnoDB;

-- append-only host history (partitioned monthly for retention)
CREATE TABLE hostHistory (
  id            BIGINT UNSIGNED AUTO_INCREMENT,
  nodeId        BIGINT UNSIGNED NOT NULL,
  sampledAt     DATETIME NOT NULL,
  cpuAvgMilli   INT UNSIGNED, ramUsedKb BIGINT UNSIGNED,
  swapUsedKb    BIGINT UNSIGNED, diskMinFreePct SMALLINT,
  PRIMARY KEY(id, sampledAt), INDEX(nodeId, sampledAt)
) ENGINE=InnoDB
  PARTITION BY RANGE (TO_DAYS(sampledAt)) (
    PARTITION p_min VALUES LESS THAN (0),
    PARTITION p_max VALUES LESS THAN MAXVALUE
  );

-- watched processes (current), keyed per node+process
CREATE TABLE procCurrent (
  nodeId   BIGINT UNSIGNED NOT NULL, procName VARCHAR(128) NOT NULL,
  pid INT, runState CHAR(1), nFiles INT, nSockets INT, rssKb BIGINT UNSIGNED,
  sampledAt DATETIME NOT NULL,
  PRIMARY KEY(nodeId, procName)
) ENGINE=InnoDB;

-- probe targets & their current measured state (from monitors)
CREATE TABLE probeTarget (
  targetId   VARCHAR(128) PRIMARY KEY,    -- stable id, e.g. "tcp:hydrogen:443"
  host       VARCHAR(255), port SMALLINT UNSIGNED,
  proto      ENUM('icmp','tcp','udp') NOT NULL,
  replFactor TINYINT NOT NULL DEFAULT 2,
  label      VARCHAR(128)
) ENGINE=InnoDB;

CREATE TABLE probeCurrent (
  targetId    VARCHAR(128) NOT NULL,
  monitorNode BIGINT UNSIGNED NOT NULL,     -- which vantage reported
  outcome     ENUM('ok','timeout','refused','unreachable','dns_fail','tls_fail','proto_err'),
  rttMicros   INT UNSIGNED, jitterMicros INT UNSIGNED,
  lossPermille SMALLINT UNSIGNED, throughputKbps INT UNSIGNED,
  serviceMeta JSON, sampledAt DATETIME NOT NULL,
  PRIMARY KEY(targetId, monitorNode)
) ENGINE=InnoDB;

-- append-only probe history, partitioned monthly (same cols + id)
CREATE TABLE probeHistory (
  id BIGINT UNSIGNED AUTO_INCREMENT, targetId VARCHAR(128),
  monitorNode BIGINT UNSIGNED, outcome VARCHAR(16),
  rttMicros INT UNSIGNED, lossPermille SMALLINT UNSIGNED, sampledAt DATETIME NOT NULL,
  PRIMARY KEY(id, sampledAt), INDEX(targetId, sampledAt)
) ENGINE=InnoDB
  PARTITION BY RANGE(TO_DAYS(sampledAt)) (
    PARTITION p_min VALUES LESS THAN (0),
    PARTITION p_max VALUES LESS THAN MAXVALUE
  );

-- failover coordination: exactly one row (id=1)
CREATE TABLE serverLease (
  id          TINYINT PRIMARY KEY DEFAULT 1,
  leaseHolder BIGINT UNSIGNED,           -- server nodeId
  leaseEpoch  BIGINT UNSIGNED NOT NULL DEFAULT 0,
  expiresAt   DATETIME(3) NOT NULL,
  CHECK (id = 1)
) ENGINE=InnoDB;

-- alert rules & firing log
CREATE TABLE alertRule (
  ruleId   INT AUTO_INCREMENT PRIMARY KEY,
  scope    ENUM('host','probe') NOT NULL,
  metric   VARCHAR(64) NOT NULL,        -- e.g. "cpuAvgMilli","lossPermille"
  op       ENUM('gt','lt','eq','transition') NOT NULL,
  threshold DOUBLE, forSeconds INT, severity ENUM('info','warn','crit'),
  enabled  BOOLEAN NOT NULL DEFAULT TRUE
) ENGINE=InnoDB;

CREATE TABLE alertEvent (
  eventId  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  ruleId   INT, nodeId BIGINT UNSIGNED, targetId VARCHAR(128),
  firedAt  DATETIME NOT NULL, clearedAt DATETIME,
  severity ENUM('info','warn','crit'), detail VARCHAR(512),
  INDEX(firedAt), INDEX(nodeId)
) ENGINE=InnoDB;

-- per-node config & deploy convergence tracking
CREATE TABLE nodeConfig (
  nodeId      BIGINT UNSIGNED PRIMARY KEY,
  targetEpoch BIGINT UNSIGNED NOT NULL, appliedEpoch BIGINT UNSIGNED,
  configBlob  JSON, lastDirectiveAt DATETIME, lastResult VARCHAR(128)
) ENGINE=InnoDB;
