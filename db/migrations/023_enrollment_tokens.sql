-- Enrollment bearer credentials are stored only as SHA-256 hex digests.
-- The credential remains valid for certificate pickup after CSR submission and
-- is cleared only after the node acknowledges installation (or rejection).
ALTER TABLE enrollment
  ADD COLUMN enrollTokenHash CHAR(64) NULL,
  ADD COLUMN tokenExpiresAt DATETIME NULL,
  ADD COLUMN certAckAt DATETIME NULL,
  ADD UNIQUE INDEX uq_enrollment_token_hash (enrollTokenHash);
