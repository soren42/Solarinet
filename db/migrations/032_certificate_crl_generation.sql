-- F-01: allocate monotonic CRL numbers and record the last atomically published
-- CRL per issuing CA. A failed generation may burn a number, never reuse one.
CREATE TABLE certificateCrlState (
  issuerFp       CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  nextNumber     BIGINT UNSIGNED NOT NULL DEFAULT 1,
  publishedNumber BIGINT UNSIGNED NULL,
  generatedAt    DATETIME(6) NULL,
  crlFp          CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
  nextUpdate     DATETIME(6) NULL,
  PRIMARY KEY (issuerFp),
  CHECK (nextNumber > 0)
) ENGINE=InnoDB;
