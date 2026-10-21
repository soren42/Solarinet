-- Persist the signing CA identity alongside the latest issued leaf. The CA
-- configured today may differ from the CA that issued an existing leaf.
ALTER TABLE node
  ADD COLUMN leafIssuerFp CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL;
