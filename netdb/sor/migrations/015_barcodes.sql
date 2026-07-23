-- 015_barcodes.sql — Physical barcode / asset-tag layer on the inventory SoR.
--
-- One new polymorphic table `barcodes` (mirrors external_refs: subject_table +
-- subject_id, no FK possible on the subject) plus an append-only audit log
-- `barcode_scans`. One scan resolves to ONE subject; a hardware unit may own
-- several codes (its AKO-#### asset tag, factory serial, and UPC).
-- Associations deliberately REUSE existing structures — asset→bin via
-- hardware_units.location_id + the locations parent_id tree, bin→project via
-- projects.tray_location_id subtree rollup + project_allocations, future
-- ports via interfaces/interconnects — so NO new edge tables here.
-- Runs against the `sor` DB (cesium). Idempotent (MariaDB IF NOT EXISTS).

-- === barcodes: every printable/scannable code, one row per code =============
CREATE TABLE IF NOT EXISTS barcodes (
  id               BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY COMMENT 'surrogate PK',
  code             VARCHAR(128) NOT NULL COMMENT 'scanned payload: AKO-0042, LOC-0007, PRJ-0003, factory serial, UPC…',
  code_type        ENUM('asset_tag','serial','upc','location','project','port','custom')
                   NOT NULL DEFAULT 'custom',
  symbology        ENUM('qr','code128','both') NOT NULL DEFAULT 'both',
  subject_table    VARCHAR(64) NOT NULL COMMENT 'hardware_units|locations|projects|interfaces (polymorphic, mirrors external_refs; enforced in the API)',
  subject_id       BIGINT UNSIGNED NOT NULL COMMENT 'PK value in subject_table (no FK possible)',
  label_printed_at DATETIME NULL COMMENT 'when a physical label was last generated+confirmed printed',
  notes            VARCHAR(255) NULL,
  source_id        SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  asserted_kind    ENUM('human','machine') NOT NULL DEFAULT 'human',
  asserted_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  created_at       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  deleted_at       DATETIME NULL COMMENT 'soft delete; NULL = live',
  -- 1 when live, NULL when deleted → UNIQUE below binds only live rows
  -- (multiple NULLs are permitted in a unique key, so retired codes may repeat).
  live_flag        TINYINT AS (IF(deleted_at IS NULL, 1, NULL)) PERSISTENT,
  UNIQUE KEY uq_barcodes_code_live (code, live_flag),
  KEY ix_barcodes_subject (subject_table, subject_id),
  KEY ix_barcodes_code (code),
  CONSTRAINT fk_barcodes_source FOREIGN KEY (source_id) REFERENCES sources (id)
) COMMENT='Physical barcode/QR codes bound to SoR subjects (units, locations, projects, interfaces)';

-- === barcode_scans: append-only audit of scan-driven actions ================
-- No updated_at/deleted_at by design: rows are never edited or removed. The
-- raw code is snapshotted so history survives a barcode soft-delete.
CREATE TABLE IF NOT EXISTS barcode_scans (
  id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  code          VARCHAR(128) NOT NULL COMMENT 'raw payload as scanned',
  barcode_id    BIGINT UNSIGNED NULL COMMENT 'resolved barcode row, if any',
  subject_table VARCHAR(64) NULL COMMENT 'resolved subject snapshot',
  subject_id    BIGINT UNSIGNED NULL,
  action        ENUM('lookup','create','receive','move','associate','allocate','print','delete')
                NOT NULL,
  operator      VARCHAR(64) NOT NULL COMMENT 'authenticated operator (Operator::requireOperator())',
  detail        TEXT NULL COMMENT 'JSON context for history/undo, e.g. {"fromLocationId":3,"toLocationId":9}',
  source_id     SMALLINT UNSIGNED NOT NULL,
  created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY ix_bscans_code (code),
  KEY ix_bscans_subject (subject_table, subject_id),
  KEY ix_bscans_ts (created_at),
  CONSTRAINT fk_bscans_barcode FOREIGN KEY (barcode_id) REFERENCES barcodes (id) ON DELETE SET NULL,
  CONSTRAINT fk_bscans_source  FOREIGN KEY (source_id)  REFERENCES sources (id)
) COMMENT='Append-only audit log: one row per scan-driven action';
