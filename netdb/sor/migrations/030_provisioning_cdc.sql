-- ============================================================================
-- 030_provisioning_cdc.sql — publish provisioning images/stacks edits to the outbox
-- ============================================================================
-- Phase P3 of the Provisioning Images & Stacks feature
-- (docs/Provisioning_UI_Design.md). Adds transactional CDC (AFTER INSERT /
-- UPDATE / DELETE -> sor_outbox) to the nine tables created by 029:
--   prov_images, prov_software, prov_software_artifacts, prov_software_requires,
--   prov_software_conflicts, prov_stacks, prov_stack_members, prov_stack_deps,
--   prov_runs.
-- 029 deliberately attached none ("CDC: deliberately none ... unless the
-- deployed sorsync.conf optional_tables is updated in lockstep"); this
-- migration is that lockstep change, together with deploy/sorsync/sor_emitd.py
-- DEFAULT_OPTIONAL_TABLES and the sorsync.conf.example line.
--
-- !! DEPLOY ORDER HAZARD — READ BEFORE APPLYING !!
-- No sorsync applier consumes these tables, so no queue is bound to
-- `sor.<prov_table>.*`. sor_emitd publishes a table NOT listed in optional
-- tables with the AMQP `mandatory` flag; an unbound route is then UNROUTABLE,
-- the batch raises, the emit checkpoint does not advance, and EVERY table's
-- events (DNS, entities, ...) stall behind the first prov_* row.
--   1. FIRST deploy sor_emitd.py carrying the new DEFAULT_OPTIONAL_TABLES and
--      restart sor-emit.service. If the DEPLOYED sorsync.conf sets
--      `[amqp] optional_tables` explicitly, that key overrides the default:
--      add the nine prov_* tables to it too (see sorsync.conf.example).
--   2. Confirm in the sor-emit log that it reconnected with the new list.
--   3. ONLY THEN apply this migration.
-- Rolling back is the reverse: apply rollback/030-down.sql first (or leave the
-- optional list in place; extra optional names are harmless).
--
-- Unknown names are harmless to the other consumers: every applier binds an
-- explicit `sor.<table>.*` allow-list (sor_apply_dnsd/ad/pihole), so a
-- prov_* event is simply never delivered to them.
--
-- row_id: sor_outbox.row_id is BIGINT UNSIGNED, which holds both the INT
-- UNSIGNED ids of eight tables and prov_runs.id (BIGINT UNSIGNED) unchanged.
--
-- prov_runs: its BEFORE DELETE guard (029 trg_prov_runs_bd) always refuses, so
-- the AFTER DELETE trigger below can never fire today. It is created anyway so
-- all nine tables carry the same three triggers (one rule, one rollback list),
-- and a future retention purge that relaxes the guard publishes its deletes
-- without a new migration. A refused DELETE writes no outbox row (tested).
-- Trigger names trg_<table>_ai/_au/_ad do not collide with 029's
-- trg_prov_runs_bu / trg_prov_runs_bd.
--
-- Ordering vs 029: 029's seed rows were inserted before these triggers exist,
-- so the seed is not published. Consumers that need current state read the
-- tables; the outbox carries changes made after 030.
--
-- If 029 is rolled back (rollback/029-down.sql) its DROP TABLEs remove these
-- triggers with the tables. Re-applying 029 does NOT bring them back, and the
-- migration ledger still lists 030 as applied: re-apply this file by hand
-- (it is idempotent) after any 029 forward recovery.
--
-- MariaDB CREATE OR REPLACE TRIGGER makes this migration safely re-runnable.
-- Apply on cesium ONLY, as the DB root account: live cesium has binary
-- logging on, so creating triggers needs SUPER / TRIGGER-with-binlog privilege
-- (like 029's trigger and the migrate-SoR-as-root rule); replication carries
-- trigger metadata to benzene. Validate on a scratch SoR first; never apply
-- tests to the live sor database.
-- ============================================================================

CREATE OR REPLACE TRIGGER trg_prov_images_ai
AFTER INSERT ON `prov_images` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_images', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_prov_images_au
AFTER UPDATE ON `prov_images` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_images', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_prov_images_ad
AFTER DELETE ON `prov_images` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_images', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_prov_software_ai
AFTER INSERT ON `prov_software` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_prov_software_au
AFTER UPDATE ON `prov_software` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_prov_software_ad
AFTER DELETE ON `prov_software` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_prov_software_artifacts_ai
AFTER INSERT ON `prov_software_artifacts` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software_artifacts', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_prov_software_artifacts_au
AFTER UPDATE ON `prov_software_artifacts` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software_artifacts', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_prov_software_artifacts_ad
AFTER DELETE ON `prov_software_artifacts` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software_artifacts', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_prov_software_requires_ai
AFTER INSERT ON `prov_software_requires` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software_requires', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_prov_software_requires_au
AFTER UPDATE ON `prov_software_requires` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software_requires', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_prov_software_requires_ad
AFTER DELETE ON `prov_software_requires` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software_requires', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_prov_software_conflicts_ai
AFTER INSERT ON `prov_software_conflicts` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software_conflicts', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_prov_software_conflicts_au
AFTER UPDATE ON `prov_software_conflicts` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software_conflicts', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_prov_software_conflicts_ad
AFTER DELETE ON `prov_software_conflicts` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_software_conflicts', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_prov_stacks_ai
AFTER INSERT ON `prov_stacks` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_stacks', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_prov_stacks_au
AFTER UPDATE ON `prov_stacks` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_stacks', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_prov_stacks_ad
AFTER DELETE ON `prov_stacks` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_stacks', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_prov_stack_members_ai
AFTER INSERT ON `prov_stack_members` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_stack_members', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_prov_stack_members_au
AFTER UPDATE ON `prov_stack_members` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_stack_members', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_prov_stack_members_ad
AFTER DELETE ON `prov_stack_members` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_stack_members', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_prov_stack_deps_ai
AFTER INSERT ON `prov_stack_deps` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_stack_deps', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_prov_stack_deps_au
AFTER UPDATE ON `prov_stack_deps` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_stack_deps', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_prov_stack_deps_ad
AFTER DELETE ON `prov_stack_deps` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_stack_deps', OLD.id, 'delete');

CREATE OR REPLACE TRIGGER trg_prov_runs_ai
AFTER INSERT ON `prov_runs` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_runs', NEW.id, 'insert');

CREATE OR REPLACE TRIGGER trg_prov_runs_au
AFTER UPDATE ON `prov_runs` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_runs', NEW.id, 'update');

CREATE OR REPLACE TRIGGER trg_prov_runs_ad
AFTER DELETE ON `prov_runs` FOR EACH ROW
  INSERT INTO sor_outbox (table_name, row_id, action)
  VALUES ('prov_runs', OLD.id, 'delete');
