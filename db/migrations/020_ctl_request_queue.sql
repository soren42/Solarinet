-- Privileged control-verb request queue (Task #1 — PHP→host security boundary).
--
-- The dashboard (PHP) must be able to REQUEST a privileged control action
-- (SIGN / DEPLOY / RETIRE / DECOMMISSION / FLEET_*) without ever holding the
-- authority to execute one. The solariCtl AF_UNIX bridge refuses every
-- PRIVILEGED verb to the dashboard peer uid (SO_PEERCRED + the verb-class ACL
-- in src/server/solariCtl.c); instead PHP submits REQUEST_SUBMIT, the bridge
-- writes ONE pending ctlRequest row here, and the privileged solariServer
-- process (running as `solari`, the sole CA-key holder) claims and executes it
-- out of PHP's reach. This table is that hand-off queue and its audit trail.
--
-- It lives in the server's own monitoring DB (this schema), NOT in the SoR on
-- cesium: the C bridge (writer) and the in-process consumer both use the
-- server's serverDb handle, so co-locating the queue with its single consumer
-- avoids a cross-host connection. The SoR remains the CMDB source of truth; this
-- is operational state owned by the server.
--
-- Additive and idempotent (CREATE TABLE IF NOT EXISTS). MariaDB / InnoDB, UTC.
CREATE TABLE IF NOT EXISTS ctlRequest (
  id             BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  verb           VARCHAR(48)  NOT NULL,               -- inner privileged verb (NOT the REQUEST_SUBMIT wrapper)
  -- Capped at the ctl line-protocol buffer (CTL_REQ_CAP=4096) so a stored row can
  -- never exceed what the consumer replays. A wider TEXT column let an oversized
  -- row be silently truncated on claim and replayed as a valid-looking prefix
  -- (review F7); VARCHAR(4096) makes over-length inserts fail at the DB instead.
  argsWire       VARCHAR(4096) NOT NULL DEFAULT '',    -- inner verb args in ctl line-protocol form (k=v k=v, per-value pct-encoded); replayed verbatim
  requestedBy    VARCHAR(128) NOT NULL,                -- authenticated operator string from the PHP session (audit)
  peerUid        INT UNSIGNED NOT NULL,                -- SO_PEERCRED uid of the submitting process (audit)
  state          ENUM('pending','claimed','done','failed') NOT NULL DEFAULT 'pending',
  attempts       INT UNSIGNED NOT NULL DEFAULT 0,      -- incremented on each claim
  claimedBy      VARCHAR(64)  NULL,                    -- consumer that won the claim CAS
  result         MEDIUMTEXT   NULL,                    -- inner verb reply on success (state=done)
  error          VARCHAR(512) NULL,                    -- failure reason on state=failed
  idempotencyKey VARCHAR(128) NULL,                    -- optional client key; UNIQUE so a resubmit returns the existing request
  createdAt      DATETIME(6)  NOT NULL DEFAULT CURRENT_TIMESTAMP(6),  -- microsecond: stable FIFO under burst submit
  updatedAt      DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  claimedAt      DATETIME(6)  NULL,
  completedAt    DATETIME(6)  NULL,
  -- Idempotency is scoped to the REQUESTER (review F10): a global key let one
  -- caller pre-seed a predictable key so another's submit silently returned the
  -- first one's request id. A composite UNIQUE still permits many NULLs (unkeyed
  -- requests never collide) but now a key only collides within the same
  -- requestedBy, and the bridge returns that requester's own existing id.
  UNIQUE KEY uq_ctlRequest_idem (requestedBy, idempotencyKey),
  -- Consumer hot path: oldest pending row first.
  INDEX ix_ctlRequest_pending (state, id),
  INDEX ix_ctlRequest_created (createdAt)
) ENGINE=InnoDB;
