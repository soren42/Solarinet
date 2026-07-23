-- 014_inventory.sql — Inventory management, built ON the existing SoR CMDB.
--
-- Extends hardware_units / hardware_models / locations / users rather than
-- duplicating them, and adds receipts, projects, and project_allocations.
-- Requirements (operator): components, purchase dates, receipt scans, serials,
-- granular storage (numbered drawers/trays), and projects that reserve inventory
-- (no double-commit) + a physical tray you "just grab" when bringing a project
-- online. FKs + triggers throughout. Runs against the `sor` DB (cesium).
-- Idempotent where the engine allows (MariaDB IF NOT EXISTS).

-- === locations: numbered drawers / trays / bins ============================
ALTER TABLE locations
  MODIFY loc_type ENUM('site','building','floor','room','rack','shelf','position',
                       'zone','cabinet','drawer','tray','bin','container') NOT NULL;
ALTER TABLE locations ADD COLUMN IF NOT EXISTS code VARCHAR(32) NULL AFTER name;  -- drawer/tray number

-- === hardware_units: inventory attributes ==================================
ALTER TABLE hardware_units
  ADD COLUMN IF NOT EXISTS quantity        INT NOT NULL DEFAULT 1,
  ADD COLUMN IF NOT EXISTS unit_cost_cents INT NULL,
  ADD COLUMN IF NOT EXISTS stock_status    ENUM('in_stock','allocated','deployed','consumed','retired')
                                           NOT NULL DEFAULT 'in_stock',
  ADD COLUMN IF NOT EXISTS receipt_id      INT UNSIGNED NULL;

-- === receipts (purchase record + scan) =====================================
CREATE TABLE IF NOT EXISTS receipts (
  id             INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  vendor         VARCHAR(128) NULL,
  purchased_on   DATE NULL,
  subtotal_cents INT NULL,
  currency       CHAR(3) NOT NULL DEFAULT 'USD',
  order_ref      VARCHAR(96) NULL,
  file_path      VARCHAR(512) NULL,          -- server path to the uploaded scan
  file_mime      VARCHAR(64) NULL,
  notes          TEXT NULL,
  source_id      INT NULL,
  asserted_kind  VARCHAR(32) NULL, asserted_at DATETIME NULL,
  created_at     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  deleted_at     DATETIME NULL,
  KEY idx_receipt_purchased (purchased_on)
);

-- === projects ==============================================================
CREATE TABLE IF NOT EXISTS projects (
  id               INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name             VARCHAR(160) NOT NULL,
  code             VARCHAR(32) NULL,
  status           ENUM('planning','active','staged','on_hold','complete','archived')
                   NOT NULL DEFAULT 'planning',
  tray_location_id INT UNSIGNED NULL,         -- the numbered tray for this project
  owner_user_id    BIGINT UNSIGNED NULL,
  description      TEXT NULL,
  started_on       DATE NULL, target_on DATE NULL, completed_on DATE NULL,
  source_id        INT NULL,
  asserted_kind    VARCHAR(32) NULL, asserted_at DATETIME NULL,
  created_at       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  deleted_at       DATETIME NULL,
  UNIQUE KEY uq_project_code (code)
);

-- === project_allocations: component <-> project reservation ================
-- An allocation references EITHER a specific owned unit (committed/installed) OR
-- a model to acquire (needed, feeds the shopping list). 'committed'/'installed'
-- are exclusive (a unit can be committed to at most one project — enforced below).
CREATE TABLE IF NOT EXISTS project_allocations (
  id                INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  project_id        INT UNSIGNED NOT NULL,
  hardware_unit_id  INT UNSIGNED NULL,
  hardware_model_id INT UNSIGNED NULL,
  allocation        ENUM('needed','committed','installed','returned') NOT NULL DEFAULT 'needed',
  quantity          INT NOT NULL DEFAULT 1,
  notes             TEXT NULL,
  source_id         INT NULL,
  created_at        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  deleted_at        DATETIME NULL,
  KEY idx_alloc_unit  (hardware_unit_id, allocation),
  KEY idx_alloc_proj  (project_id)
  -- invariant "hardware_unit_id OR hardware_model_id must be set" is enforced in the API
);

-- === foreign keys (added after all tables exist) ===========================
-- Guarded so re-running the migration doesn't error on duplicate constraints.
SET @fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
            WHERE CONSTRAINT_SCHEMA=DATABASE() AND CONSTRAINT_NAME='fk_hwunit_receipt');
SET @sql := IF(@fk=0,'ALTER TABLE hardware_units ADD CONSTRAINT fk_hwunit_receipt FOREIGN KEY (receipt_id) REFERENCES receipts(id) ON DELETE SET NULL','DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
            WHERE CONSTRAINT_SCHEMA=DATABASE() AND CONSTRAINT_NAME='fk_project_tray');
SET @sql := IF(@fk=0,'ALTER TABLE projects ADD CONSTRAINT fk_project_tray FOREIGN KEY (tray_location_id) REFERENCES locations(id) ON DELETE SET NULL, ADD CONSTRAINT fk_project_owner FOREIGN KEY (owner_user_id) REFERENCES users(id) ON DELETE SET NULL','DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
            WHERE CONSTRAINT_SCHEMA=DATABASE() AND CONSTRAINT_NAME='fk_alloc_project');
SET @sql := IF(@fk=0,'ALTER TABLE project_allocations ADD CONSTRAINT fk_alloc_project FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE, ADD CONSTRAINT fk_alloc_unit FOREIGN KEY (hardware_unit_id) REFERENCES hardware_units(id) ON DELETE SET NULL, ADD CONSTRAINT fk_alloc_model FOREIGN KEY (hardware_model_id) REFERENCES hardware_models(id) ON DELETE SET NULL','DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- === shopping list: what a project still needs but doesn't own ==============
CREATE OR REPLACE VIEW v_project_shopping AS
  SELECT pa.project_id, p.name AS project, pa.hardware_model_id,
         hm.make, hm.model, hm.hw_type, SUM(pa.quantity) AS qty_needed
  FROM project_allocations pa
  JOIN projects p ON p.id=pa.project_id AND p.deleted_at IS NULL
  LEFT JOIN hardware_models hm ON hm.id=pa.hardware_model_id
  WHERE pa.allocation='needed' AND pa.hardware_unit_id IS NULL AND pa.deleted_at IS NULL
  GROUP BY pa.project_id, p.name, pa.hardware_model_id, hm.make, hm.model, hm.hw_type;

-- === triggers: double-commit guard, tray auto-move, audit ==================
DROP TRIGGER IF EXISTS trg_alloc_no_double_commit_ins;
DROP TRIGGER IF EXISTS trg_alloc_no_double_commit_upd;
DROP TRIGGER IF EXISTS trg_alloc_tray_move;
DELIMITER $$

CREATE TRIGGER trg_alloc_no_double_commit_ins BEFORE INSERT ON project_allocations
FOR EACH ROW BEGIN
  IF NEW.allocation IN ('committed','installed') AND NEW.hardware_unit_id IS NOT NULL THEN
    IF EXISTS (SELECT 1 FROM project_allocations pa
                WHERE pa.hardware_unit_id = NEW.hardware_unit_id
                  AND pa.allocation IN ('committed','installed')
                  AND pa.deleted_at IS NULL AND pa.project_id <> NEW.project_id) THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'hardware_unit already committed to another project';
    END IF;
  END IF;
END$$

CREATE TRIGGER trg_alloc_no_double_commit_upd BEFORE UPDATE ON project_allocations
FOR EACH ROW BEGIN
  IF NEW.allocation IN ('committed','installed') AND NEW.hardware_unit_id IS NOT NULL THEN
    IF EXISTS (SELECT 1 FROM project_allocations pa
                WHERE pa.hardware_unit_id = NEW.hardware_unit_id
                  AND pa.allocation IN ('committed','installed')
                  AND pa.deleted_at IS NULL AND pa.id <> NEW.id
                  AND pa.project_id <> NEW.project_id) THEN
      SIGNAL SQLSTATE '45000'
        SET MESSAGE_TEXT = 'hardware_unit already committed to another project';
    END IF;
  END IF;
END$$

-- Committing/installing a unit moves it to the project's numbered tray and flips
-- its stock_status — the "grab the tray" workflow, enforced in the DB.
CREATE TRIGGER trg_alloc_tray_move AFTER INSERT ON project_allocations
FOR EACH ROW BEGIN
  IF NEW.allocation IN ('committed','installed') AND NEW.hardware_unit_id IS NOT NULL THEN
    UPDATE hardware_units hu
      JOIN projects p ON p.id = NEW.project_id
      SET hu.location_id  = COALESCE(p.tray_location_id, hu.location_id),
          hu.stock_status = IF(NEW.allocation='installed','deployed','allocated')
    WHERE hu.id = NEW.hardware_unit_id;
  END IF;
END$$

DELIMITER ;
