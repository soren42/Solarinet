-- Persist the issued leaf identity for audit and for a future transport API
-- that can expose the authenticated peer certificate DER to the server.
ALTER TABLE node
  ADD COLUMN leafCertFp CHAR(64) NULL,
  ADD COLUMN leafCertSerial VARCHAR(40) NULL;
