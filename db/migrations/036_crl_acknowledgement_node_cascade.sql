-- CRL acknowledgement rows are disposable per-node status. Retired node
-- retention deletes the node after its retention window, so remove the
-- acknowledgement with it instead of blocking the maintenance job.
ALTER TABLE certificateCrlAcknowledgement
  DROP FOREIGN KEY fk_certificateCrlAck_node;

ALTER TABLE certificateCrlAcknowledgement
  ADD CONSTRAINT fk_certificateCrlAck_node
    FOREIGN KEY (nodeId) REFERENCES node (nodeId) ON DELETE CASCADE;
