-- F-10: a report receipt commits atomically with its current/history rows.
-- Reporter incarnations prevent seqNo reuse after a sender restart or wrap.
CREATE TABLE reportReceipt (
  nodeId BIGINT UNSIGNED NOT NULL,
  sessionId BINARY(16) NOT NULL,
  seqNo BIGINT UNSIGNED NOT NULL,
  msgType INT UNSIGNED NOT NULL,
  payloadHash BINARY(32) NOT NULL,
  receivedAt DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (nodeId, sessionId, seqNo, msgType),
  INDEX (receivedAt)
) ENGINE=InnoDB;
