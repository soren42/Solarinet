-- 012_alert_ack_and_opie.sql
--
-- (1) Alert acknowledgement + lifecycle. Operators can ACK an alert; the alerts
--     view retires an alert to History once it is acked, OR 60 min after it
--     clears if left un-acked. Ack state lives on alertEvent (the C engine only
--     INSERTs fire/clear rows; ack is a dashboard-layer concept, so no C change).
-- (2) Opie — the AI on-call SA. One opieReport per incident: a root-cause +
--     recommendations writeup produced by an Opus-4.8 investigation, shown in the
--     dashboard with an iMessage summary pushed at trigger time.
-- Additive + idempotent.

ALTER TABLE alertEvent
  ADD COLUMN IF NOT EXISTS ackedAt DATETIME NULL,
  ADD COLUMN IF NOT EXISTS ackedBy VARCHAR(64) NULL;

CREATE TABLE IF NOT EXISTS opieReport (
  reportId     INT AUTO_INCREMENT PRIMARY KEY,
  incidentKey  VARCHAR(160) NOT NULL,                 -- dedup key: one investigation per incident
  triggerKind  ENUM('crit','warn-storm') NOT NULL,
  firstEventId BIGINT UNSIGNED NULL,                  -- alertEvent that tripped it
  nodeId       BIGINT UNSIGNED NULL,
  hostFqdn     VARCHAR(255) NULL,
  severity     ENUM('info','warn','crit') NULL,
  status       ENUM('investigating','done','failed') NOT NULL DEFAULT 'investigating',
  summary      VARCHAR(512) NULL,                     -- the 2-3 line iMessage summary
  analysis     MEDIUMTEXT NULL,                       -- full markdown RCA + recommendations
  model        VARCHAR(48) NULL,
  durationSec  INT NULL,
  startedAt    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  finishedAt   DATETIME NULL,
  UNIQUE KEY uq_incident (incidentKey),
  KEY idx_started (startedAt),
  KEY idx_node (nodeId)
);
