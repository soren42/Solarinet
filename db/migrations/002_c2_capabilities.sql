-- SolariNet C2 capabilities migration (Phase 3 Handoff §5).
--
-- Six new tables and two small column additions extend db/schema.sql. They are
-- additive — no existing table changes shape — so this is a forward migration,
-- not a rewrite.

-- network segments (first-class; referenced by node, gear, discovery)
CREATE TABLE segment (
  segId    VARCHAR(32) PRIMARY KEY,        -- "core","compute","storage","dmz","lab","iot"
  label    VARCHAR(64) NOT NULL,
  cidr     VARCHAR(64) NOT NULL,            -- e.g. "10.42.20.0/24"
  wireless BOOLEAN NOT NULL DEFAULT FALSE,
  notes    VARCHAR(255)
) ENGINE=InnoDB;

-- network gear (switches/APs/gateways) for the LAN-hierarchy topology
CREATE TABLE networkGear (
  gearId   VARCHAR(48) PRIMARY KEY,        -- "sw-core-01","gw","ap-lab-01"
  name     VARCHAR(96) NOT NULL,
  kind     ENUM('gateway','switch','ap','router','other') NOT NULL,
  model    VARCHAR(96), segId VARCHAR(32),
  ports    SMALLINT, uplinkGearId VARCHAR(48),  -- self-referential hierarchy
  wireless BOOLEAN NOT NULL DEFAULT FALSE, mgmtIp VARCHAR(64),
  lastSeenAt DATETIME,
  INDEX(segId), INDEX(uplinkGearId)
) ENGINE=InnoDB;

-- LLDP / inspection-derived adjacency: who connects where, at L2
CREATE TABLE lldpEdge (
  id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  nodeId      BIGINT UNSIGNED,                -- a SolariNet node, if the endpoint is one
  gearId      VARCHAR(48),                     -- the switch/AP side
  localIf     VARCHAR(48), peerPort VARCHAR(48),
  linkType    ENUM('wired','wireless') NOT NULL,
  speedMbps   INT, rssi SMALLINT, viaLldp BOOLEAN NOT NULL DEFAULT FALSE,
  observedAt  DATETIME NOT NULL,
  INDEX(nodeId), INDEX(gearId)
) ENGINE=InnoDB;

-- discovered-but-not-monitored candidates
CREATE TABLE discovered (
  discId    BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  host      VARCHAR(255), ip VARCHAR(64) NOT NULL,
  kind      ENUM('host','service','network','other') NOT NULL,
  via       ENUM('mdns','arp','scp_advert','portscan','lldp') NOT NULL,
  services  JSON,                            -- ["ssh:22","http:80"]
  segId     VARCHAR(32), arch VARCHAR(16),
  seenCount INT NOT NULL DEFAULT 1, firstSeenAt DATETIME, lastSeenAt DATETIME,
  status    ENUM('new','ignored','adopting','adopted') NOT NULL DEFAULT 'new',
  UNIQUE(ip, kind), INDEX(status), INDEX(segId)
) ENGINE=InnoDB;

-- pending enrollments (CSRs awaiting operator approval) — §15.1 made stateful
CREATE TABLE enrollment (
  enrId       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  host        VARCHAR(255), ip VARCHAR(64),
  role        ENUM('client','monitor','server') NOT NULL,
  certFp      VARCHAR(95),                    -- SHA-256 CSR fingerprint, colon-hex
  status      ENUM('token','pending','approved','rejected','expired') NOT NULL,
  requestedAt DATETIME, decidedAt DATETIME, decidedBy VARCHAR(64),
  csrPem      TEXT,                          -- held only until signed, then nulled
  INDEX(status)
) ENGINE=InnoDB;

-- build/artifact registry (per arch/os/channel) + convergence rollup
CREATE TABLE buildArtifact (
  buildId   BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  arch      VARCHAR(16) NOT NULL, os VARCHAR(16) NOT NULL,
  version   VARCHAR(32) NOT NULL,
  channel   ENUM('stable','beta','dev') NOT NULL DEFAULT 'stable',
  sha256    CHAR(64) NOT NULL, sizeBytes BIGINT UNSIGNED,
  artifactUri VARCHAR(255), publishedAt DATETIME,
  UNIQUE(arch, os, version, channel)
) ENGINE=InnoDB;

-- small additions to existing tables (additive)
ALTER TABLE node     ADD COLUMN segId VARCHAR(32) AFTER arch,
                       ADD COLUMN uplinkGearId VARCHAR(48),
                       MODIFY state ENUM('up','degraded','down','unknown','retired') NOT NULL DEFAULT 'unknown';
-- NOTE: the §10 baseline (db/schema.sql) already declares probeTarget.label,
-- while Handoff §5 lists `ADD COLUMN label` here. Applying both verbatim would
-- raise a duplicate-column error, so the label add is guarded with
-- IF NOT EXISTS (MariaDB 10.0.2+) to keep this migration faithful to §5 yet
-- idempotent over the baseline. segId is genuinely new.
ALTER TABLE probeTarget ADD COLUMN IF NOT EXISTS label VARCHAR(128),
                        ADD COLUMN segId VARCHAR(32);
