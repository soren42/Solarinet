-- Transactional outbox for SoR change-data-capture. Triggers on tracked tables
-- append here in the SAME transaction as the change; sor_emitd publishes rows to
-- the MQ (sor.events) and advances a checkpoint. No FKs by design (write-cheap).
CREATE TABLE IF NOT EXISTS sor_outbox (
  id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  table_name VARCHAR(64) NOT NULL,
  row_id     BIGINT UNSIGNED NULL,
  action     ENUM('insert','update','delete') NOT NULL,
  changed_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS sor_sync_state (
  k VARCHAR(64) NOT NULL PRIMARY KEY,
  v BIGINT UNSIGNED NOT NULL DEFAULT 0,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
INSERT IGNORE INTO sor_sync_state (k,v) VALUES ('emit_checkpoint',0),('discovery_checkpoint',0);
