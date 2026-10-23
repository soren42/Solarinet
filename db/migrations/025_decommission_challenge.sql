-- Durable, single-use operator confirmation for node decommission.
-- Only the SHA-256 digest of the 64-bit CSPRNG token is persisted.
CREATE TABLE IF NOT EXISTS decommissionChallenge (
  nodeId       BIGINT UNSIGNED PRIMARY KEY,
  wipeScope    TINYINT UNSIGNED NOT NULL,
  requestedBy  VARCHAR(128) NOT NULL,
  tokenHash    CHAR(64) NOT NULL,
  state        ENUM('pending','consumed','cancelled') NOT NULL DEFAULT 'pending',
  issuedAt     DATETIME(6) NOT NULL,
  expiresAt    DATETIME(6) NOT NULL,
  consumedAt   DATETIME(6) NULL,
  INDEX ix_decommissionChallenge_expiry (state, expiresAt),
  CONSTRAINT fk_decommissionChallenge_node
    FOREIGN KEY (nodeId) REFERENCES node(nodeId) ON DELETE CASCADE
) ENGINE=InnoDB;
