-- F-01: commit enrollment approval, node registration, and the issued public
-- certificate as one recoverable state change. The certificate is public
-- material; the CSR remains transient and is cleared in the same transaction.
ALTER TABLE enrollment
  ADD COLUMN nodeId BIGINT UNSIGNED NULL,
  ADD COLUMN issuedCertPem MEDIUMTEXT NULL;
