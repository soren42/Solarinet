-- ============================================================================
-- 024_provisioning_cdc.sql — publish provisioning taxonomy edits to the outbox
-- ============================================================================
-- Adds transactional CDC for the mutable tables introduced or used by
-- migration 021. `sources` is included because 021 registers `provisioner`
-- there. `inventory_snapshots` is append-only and immutable, so it is
-- intentionally not tracked. The existing emit daemon publishes these events
-- on the same sor.events contract as the core tables.
--
-- MariaDB CREATE OR REPLACE TRIGGER makes this migration safely re-runnable.
-- Apply on cesium only; replication carries trigger metadata to benzene.
-- Validate on a scratch SoR first; never apply tests to the live sor database.
-- ============================================================================

CREATE OR REPLACE TRIGGER trg_sources_ai
AFTER INSERT ON `sources` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('sources', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_sources_au
AFTER UPDATE ON `sources` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('sources', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_sources_ad
AFTER DELETE ON `sources` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('sources', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_functions_ai
AFTER INSERT ON `functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('functions', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_functions_au
AFTER UPDATE ON `functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('functions', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_functions_ad
AFTER DELETE ON `functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('functions', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_entity_functions_ai
AFTER INSERT ON `entity_functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('entity_functions', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_entity_functions_au
AFTER UPDATE ON `entity_functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('entity_functions', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_entity_functions_ad
AFTER DELETE ON `entity_functions` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('entity_functions', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_function_incompatibility_ai
AFTER INSERT ON `function_incompatibility` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('function_incompatibility', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_function_incompatibility_au
AFTER UPDATE ON `function_incompatibility` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('function_incompatibility', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_function_incompatibility_ad
AFTER DELETE ON `function_incompatibility` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('function_incompatibility', OLD.id, 'delete');
