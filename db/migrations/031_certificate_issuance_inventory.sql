-- F-01: retain issuer-qualified identity for every issued leaf. Existing
-- revocation entries are issuance evidence and are copied before adding the
-- reference from the revocation ledger.
CREATE TABLE certificateIssuance (
  issuerFp       CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  serialHex      VARCHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  leafCertFp     CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  nodeId         BIGINT UNSIGNED NULL,
  enrollmentId   BIGINT UNSIGNED NULL,
  origin         ENUM('enrollment','operator','backfill') NOT NULL,
  issuedBy       VARCHAR(128) NOT NULL,
  issuedAt       DATETIME(6) NULL,
  recordedAt     DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (issuerFp, serialHex),
  UNIQUE KEY uq_certificateIssuance_leaf (leafCertFp),
  UNIQUE KEY uq_certificateIssuance_enrollment (enrollmentId),
  KEY ix_certificateIssuance_node (nodeId, issuedAt),
  CONSTRAINT fk_certificateIssuance_node
    FOREIGN KEY (nodeId) REFERENCES node(nodeId) ON DELETE RESTRICT,
  CONSTRAINT fk_certificateIssuance_enrollment
    FOREIGN KEY (enrollmentId) REFERENCES enrollment(enrId) ON DELETE RESTRICT
) ENGINE=InnoDB;

INSERT INTO certificateIssuance
  (issuerFp,serialHex,leafCertFp,nodeId,enrollmentId,origin,issuedBy,issuedAt)
SELECT issuerFp,serialHex,leafCertFp,nodeId,NULL,'backfill',revokedBy,NULL
FROM certificateRevocation;

CREATE TABLE certificateInventoryState (
  singletonId TINYINT UNSIGNED NOT NULL PRIMARY KEY,
  complete    BOOLEAN NOT NULL DEFAULT FALSE,
  completedAt DATETIME(6) NULL,
  completedBy VARCHAR(128) NULL,
  CHECK (singletonId = 1)
) ENGINE=InnoDB;

INSERT INTO certificateInventoryState (singletonId,complete) VALUES (1,FALSE);

ALTER TABLE certificateRevocation
  MODIFY COLUMN nodeId BIGINT UNSIGNED NULL,
  ADD COLUMN enrollmentId BIGINT UNSIGNED NULL AFTER nodeId,
  ADD CONSTRAINT fk_certificateRevocation_issuance
    FOREIGN KEY (issuerFp,serialHex)
    REFERENCES certificateIssuance(issuerFp,serialHex) ON DELETE RESTRICT,
  ADD CONSTRAINT fk_certificateRevocation_enrollment
    FOREIGN KEY (enrollmentId) REFERENCES enrollment(enrId) ON DELETE RESTRICT;

ALTER TABLE certificateRevocationChallenge
  MODIFY COLUMN nodeId BIGINT UNSIGNED NULL,
  ADD COLUMN enrollmentId BIGINT UNSIGNED NULL AFTER nodeId,
  ADD CONSTRAINT fk_certificateRevocationChallenge_enrollment
    FOREIGN KEY (enrollmentId) REFERENCES enrollment(enrId) ON DELETE RESTRICT;
