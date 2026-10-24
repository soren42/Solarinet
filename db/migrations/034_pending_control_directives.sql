-- Persist outbound control directives until the matching node result arrives.
-- A node/correlation pair is unique while pending so a restarted server cannot
-- silently reuse a correlation ID that still has an outstanding directive.
CREATE TABLE pendingControlDirective (
  nodeId         BIGINT UNSIGNED NOT NULL,
  correlationId  INT UNSIGNED NOT NULL,
  verb           TINYINT UNSIGNED NOT NULL,
  targetEpoch    BIGINT UNSIGNED NOT NULL DEFAULT 0,
  issuedAt       DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (nodeId, correlationId),
  KEY ix_pending_control_issued (issuedAt)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
