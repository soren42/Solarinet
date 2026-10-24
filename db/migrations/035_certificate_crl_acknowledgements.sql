-- Record a node reporter's authenticated observation of the currently
-- published CRL. The key is node+issuer; report retries refresh the timestamp.
CREATE TABLE certificateCrlAcknowledgement (
  nodeId          BIGINT UNSIGNED NOT NULL,
  issuerFp        CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  crlNumber       BIGINT UNSIGNED NOT NULL,
  crlFp           CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  acknowledgedAt  DATETIME(6) NOT NULL,
  PRIMARY KEY (nodeId, issuerFp),
  KEY ix_certificateCrlAck_generation (issuerFp, crlNumber, crlFp),
  CONSTRAINT fk_certificateCrlAck_node FOREIGN KEY (nodeId) REFERENCES node (nodeId),
  CONSTRAINT chk_certificateCrlAck_number CHECK (crlNumber > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
