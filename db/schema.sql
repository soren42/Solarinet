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
  fsReadonlyCount  TINYINT UNSIGNED   NOT NULL DEFAULT 0,
  blockDevMissing  TINYINT UNSIGNED   NOT NULL DEFAULT 0,
  smartFailCount   TINYINT UNSIGNED   NOT NULL DEFAULT 0,
  failedUnitCount  SMALLINT UNSIGNED  NOT NULL DEFAULT 0,
  dmesgCritCount   SMALLINT UNSIGNED  NOT NULL DEFAULT 0,
  fsReadonlyList   VARCHAR(256) NOT NULL DEFAULT '',
  smartFailList    VARCHAR(256) NOT NULL DEFAULT '',
  failedUnitList   VARCHAR(256) NOT NULL DEFAULT '',
  dmesgCritSample  VARCHAR(256) NOT NULL DEFAULT '',
  CONSTRAINT FK_hc_node FOREIGN KEY(nodeId) REFERENCES node(nodeId) ON DELETE CASCADE
) ENGINE=InnoDB;

-- append-only host history (partitioned monthly for retention)
CREATE TABLE hostHistory (
  id            BIGINT UNSIGNED AUTO_INCREMENT,
  nodeId        BIGINT UNSIGNED NOT NULL,
  sampledAt     DATETIME NOT NULL,
  cpuAvgMilli   INT UNSIGNED, ramUsedKb BIGINT UNSIGNED,
  swapUsedKb    BIGINT UNSIGNED, diskMinFreePct SMALLINT,
  netKbps       BIGINT UNSIGNED,             -- aggregate iface rx+tx throughput
  fsReadonlyCount TINYINT UNSIGNED NOT NULL DEFAULT 0,
  blockDevMissing TINYINT UNSIGNED NOT NULL DEFAULT 0,
  smartFailCount  TINYINT UNSIGNED NOT NULL DEFAULT 0,
  failedUnitCount SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  dmesgCritCount  SMALLINT UNSIGNED NOT NULL DEFAULT 0,
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
  enabled  BOOLEAN NOT NULL DEFAULT TRUE,
  UNIQUE KEY uq_alertrule_scope_metric (scope, metric)
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

-- Privileged control-verb request queue (Task #1 — PHP→host security boundary).
-- The dashboard (PHP) is refused every PRIVILEGED ctl verb at the socket
-- (SO_PEERCRED + verb-class ACL in src/server/solariCtl.c); it REQUEST_SUBMITs
-- one pending row here instead, and the privileged in-process consumer
-- (serverCtlQueuePoll, running as the sole CA-key holder) claims and executes it
-- out of PHP's reach. This is the hand-off queue and its audit trail. Kept in
-- sync with db/migrations/020_ctl_request_queue.sql.
CREATE TABLE ctlRequest (
  id             BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  verb           VARCHAR(48)  NOT NULL,               -- inner privileged verb
  -- Capped at the ctl replay buffer (CTL_REQ_CAP=4096) so a stored row can never
  -- exceed what the consumer replays; a wider TEXT column allowed silent
  -- truncation-on-claim and replay of a valid-looking prefix (review F7).
  argsWire       VARCHAR(4096) NOT NULL DEFAULT '',    -- inner args (ctl line-protocol, pct-encoded); replayed verbatim
  requestedBy    VARCHAR(128) NOT NULL,                -- authenticated operator from the PHP session (audit)
  peerUid        INT UNSIGNED NOT NULL,                -- SO_PEERCRED uid of the submitter (audit)
  state          ENUM('pending','claimed','done','failed') NOT NULL DEFAULT 'pending',
  attempts       INT UNSIGNED NOT NULL DEFAULT 0,      -- incremented on each claim
  claimedBy      VARCHAR(64)  NULL,                    -- consumer that won the claim CAS
  result         MEDIUMTEXT   NULL,                    -- inner verb reply on success
  error          VARCHAR(512) NULL,                    -- failure reason on state=failed
  idempotencyKey VARCHAR(128) NULL,                    -- optional client key; UNIQUE so a resubmit returns the existing request
  createdAt      DATETIME(6)  NOT NULL DEFAULT CURRENT_TIMESTAMP(6),  -- microsecond: stable FIFO under burst
  updatedAt      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  claimedAt      DATETIME(6)  NULL,
  completedAt    DATETIME(6)  NULL,
  -- Idempotency scoped to the requester (review F10): a global key let one caller
  -- pre-seed a predictable key so another's submit silently returned the first
  -- request. Composite still permits many NULLs; a key only collides within its
  -- own requestedBy.
  UNIQUE KEY uq_ctlRequest_idem (requestedBy, idempotencyKey),
  INDEX ix_ctlRequest_pending (state, id),             -- consumer hot path: oldest pending first
  INDEX ix_ctlRequest_created (createdAt)
) ENGINE=InnoDB;
