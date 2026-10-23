-- F-01 first implementation slice: durable, operator-confirmed revocation
-- records bound to the active leaf's exact issuer, serial, and DER fingerprint.
CREATE TABLE certificateRevocation (
  issuerFp       CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  serialHex      VARCHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  leafCertFp     CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  nodeId         BIGINT UNSIGNED NOT NULL,
  reason         ENUM('unspecified','keyCompromise','affiliationChanged',
                      'superseded','cessationOfOperation') NOT NULL,
  revokedBy      VARCHAR(128) NOT NULL,
  revokedAt      DATETIME(6) NOT NULL,
  PRIMARY KEY (issuerFp, serialHex),
  UNIQUE KEY uq_certificateRevocation_leaf (leafCertFp),
  KEY ix_certificateRevocation_node (nodeId, revokedAt),
  CONSTRAINT fk_certificateRevocation_node
    FOREIGN KEY (nodeId) REFERENCES node(nodeId) ON DELETE RESTRICT
) ENGINE=InnoDB;

-- A short-lived, single-use challenge. Only SHA-256(token) is persisted.
CREATE TABLE certificateRevocationChallenge (
  issuerFp       CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  serialHex      VARCHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  leafCertFp     CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  nodeId         BIGINT UNSIGNED NOT NULL,
  reason         ENUM('unspecified','keyCompromise','affiliationChanged',
                      'superseded','cessationOfOperation') NOT NULL,
  requestedBy    VARCHAR(128) NOT NULL,
  tokenHash      CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  state          ENUM('pending','consumed','cancelled') NOT NULL DEFAULT 'pending',
  issuedAt       DATETIME(6) NOT NULL,
  expiresAt      DATETIME(6) NOT NULL,
  consumedAt     DATETIME(6) NULL,
  PRIMARY KEY (issuerFp, serialHex),
  KEY ix_certificateRevocationChallenge_expiry (state, expiresAt),
  CONSTRAINT fk_certificateRevocationChallenge_node
    FOREIGN KEY (nodeId) REFERENCES node(nodeId) ON DELETE RESTRICT
) ENGINE=InnoDB;
