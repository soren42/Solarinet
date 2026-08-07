-- ============================================================================
-- 020_rackwire.sql — RackWire connection planner: plan storage + power model
-- ============================================================================
-- Implements CONTRACT-RW v1.0 §1 (see RackWire/CONTRACT-RW.md).
--
-- Purpose:
--   1. `rw_plans` / `rw_plan_versions` — server-side persistence for RackWire
--      plans (D4). The browser app keeps its localStorage path for file:// and
--      offline use; SoR is authoritative when reachable.
--   2. `power_circuits` + ENUM extensions — power modelling built ON the
--      existing SoR shape rather than a parallel domain (D5): power inlets and
--      outlets are `interfaces` rows (`if_kind` = power_inlet/power_outlet),
--      power cords are `interconnects` rows (`link_kind` = 'power'), and the
--      branch circuits they land on are the new `power_circuits` tree hanging
--      off `locations`. No new edge tables.
--   3. `rw_model_groups` — port geometry (the HANDOFF §6 `groups` array) and
--      device-level electrical extras keyed to `hardware_models`, so the
--      device library can be served from the SoR instead of a static array (D8).
--   4. A `sources` row (`slug` = 'rackwire') so committed cables/locations carry
--      RackWire provenance and the commit path can refuse to touch rows it does
--      not own (D6, §4).
--
-- Conventions followed (netdb/sor/SCHEMA.md §6):
--   * Provenance quintet on every new table: source_id, asserted_kind,
--     asserted_at, created_at/updated_at/deleted_at.
--     Exception, deliberate: `rw_plan_versions` is append-only (CONTRACT-RW §1.2)
--     and therefore carries no updated_at/deleted_at — same shape as
--     `barcode_scans` in 015. Version-count cap (25) is enforced in the API.
--   * Soft-delete-aware uniques via the 015 `live_flag` generated-column trick:
--     live rows get 1, deleted rows get NULL, and a UNIQUE key over (…, live_flag)
--     binds only live rows (multiple NULLs are permitted in a unique key).
--   * ENUM values are only ever APPENDED — never inserted or reordered — so
--     existing rows keep their stored ordinal.
--   * Every statement is idempotent and re-runnable: CREATE TABLE IF NOT EXISTS,
--     ADD COLUMN IF NOT EXISTS, information_schema-guarded FK and ENUM changes
--     via PREPARE/EXECUTE (pattern copied from 014_inventory.sql:86-99).
--
-- No CDC triggers are added on the new tables — the sorsync bypass is intended.
-- Writes this migration enables against `interfaces` DO fire the existing CDC
-- triggers; that is acceptable, the appliers diff before acting.
--
-- Target: MariaDB >= 10.7 (validated on 12.3 dialect), InnoDB, utf8mb4.
-- Database: `sor` on cesium (10.1.0.200). cesium replicates to benzene — apply
-- on cesium ONLY and let replication carry it; never apply on benzene directly.
--
-- Apply:
--   sudo mariadb sor < netdb/sor/migrations/020_rackwire.sql
--
-- Validate first against a scratch/stage schema — never point tests at live `sor`.
-- ============================================================================

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

-- ----------------------------------------------------------------------------
-- 1. sources — register RackWire as a provenance target
-- ----------------------------------------------------------------------------
-- CONTRACT-RW §1.1: API code resolves this with
--   SELECT id FROM sources WHERE slug='rackwire'
-- INSERT IGNORE per the binding spec, so re-running is a no-op.
INSERT IGNORE INTO `sources` (`slug`,`display_name`,`kind`,`endpoint`)
VALUES ('rackwire', 'RackWire connection planner', 'human', 'https://dashboard.akoria.net/rackwire/');

-- ----------------------------------------------------------------------------
-- 2. rw_plans — one row per named RackWire plan (current state)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `rw_plans` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `name`          VARCHAR(160) NOT NULL COMMENT 'operator-facing plan name; unique among live rows',
  `slug`          VARCHAR(96)  NOT NULL COMMENT 'url-safe stable handle derived from name; unique among live rows',
  `plan_json`     LONGTEXT NOT NULL COMMENT 'RackWire.getPlan() payload verbatim: {devices,cables,racks,notes,codeMap}',
  `thumbnail`     LONGTEXT NULL COMMENT 'optional data: URI preview of the map view, for the plan picker',
  `device_count`  INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'denormalized from plan_json for the list endpoint (no blob read)',
  `cable_count`   INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'denormalized from plan_json for the list endpoint (no blob read)',
  `updated_by`    VARCHAR(64) NULL COMMENT 'authenticated operator that last wrote this plan (Operator::requireOperator())',
  `notes`         TEXT NULL COMMENT 'free-form operator notes',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row (normally rackwire)',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  -- 1 when live, NULL when deleted → the UNIQUE keys below bind only live rows.
  `live_flag`     TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_rw_plans_name_live` (`name`,`live_flag`),
  UNIQUE KEY `uq_rw_plans_slug_live` (`slug`,`live_flag`),
  KEY `ix_rw_plans_updated` (`updated_at`),
  CONSTRAINT `ck_rw_plans_json`   CHECK (JSON_VALID(`plan_json`)),
  CONSTRAINT `fk_rw_plans_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='RackWire plans: current state of each named cabling plan (D4)';

-- ----------------------------------------------------------------------------
-- 3. rw_plan_versions — append-only snapshots of a plan
-- ----------------------------------------------------------------------------
-- Backs RackWire.saveVersion()/versions()/restoreVersion(). Append-only by
-- design: no updated_at, no deleted_at, rows are never edited. The 25-snapshot
-- cap from HANDOFF §8 is enforced in the API (prune oldest on insert), not here,
-- so an operator can lift it without a schema change.
CREATE TABLE IF NOT EXISTS `rw_plan_versions` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK; also the snapshot id the host API returns',
  `plan_id`       BIGINT UNSIGNED NOT NULL COMMENT 'FK: the plan this snapshot belongs to',
  `version_name`  VARCHAR(160) NULL COMMENT 'operator label passed to saveVersion(name); NULL = auto-named by timestamp',
  `plan_json`     LONGTEXT NOT NULL COMMENT 'full plan payload at snapshot time (not a diff)',
  `created_by`    VARCHAR(64) NULL COMMENT 'authenticated operator that took the snapshot',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  PRIMARY KEY (`id`),
  KEY `ix_rw_plan_versions_plan` (`plan_id`,`created_at`),
  CONSTRAINT `ck_rw_plan_versions_json` CHECK (JSON_VALID(`plan_json`)),
  CONSTRAINT `fk_rw_plan_versions_plan`   FOREIGN KEY (`plan_id`)   REFERENCES `rw_plans`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_rw_plan_versions_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Append-only RackWire plan snapshots; cap enforced in the API, not the schema';

-- ----------------------------------------------------------------------------
-- 4. power_circuits — branch circuits / PDU rails, as a tree off locations
-- ----------------------------------------------------------------------------
-- The electrical peer of `locations`: a self-referencing tree so a wall circuit
-- can parent a UPS output rail which parents a PDU bank. RackWire's AC loading
-- analysis (80% derate) sums the draw of every interface whose circuit_id lands
-- on a node of this tree.
CREATE TABLE IF NOT EXISTS `power_circuits` (
  `id`                INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `parent_circuit_id` INT UNSIGNED NULL COMMENT 'FK: upstream circuit (wall > UPS rail > PDU bank); NULL = utility feed',
  `location_id`       INT UNSIGNED NULL COMMENT 'FK: where the circuit lands (room/rack); NULL = unplaced',
  `name`              VARCHAR(128) NOT NULL COMMENT 'operator label, panel-qualified, e.g. "MAIN-12", "UPS-A/bank1"; unique among live rows',
  `breaker_label`     VARCHAR(64) NULL COMMENT 'breaker/fuse designation as printed on the panel',
  `volts`             DECIMAL(6,2) NULL COMMENT 'nominal circuit voltage, e.g. 120.00, 240.00',
  `amps`              DECIMAL(6,2) NULL COMMENT 'breaker rating in amps; usable continuous load is 80% of this (NEC derate)',
  `phase`             ENUM('single','split','three','dc') NOT NULL DEFAULT 'single' COMMENT 'supply topology',
  `notes`             TEXT NULL COMMENT 'free-form operator notes',
  `source_id`         SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`     ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`        DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`         TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_power_circuits_name_live` (`name`,`live_flag`),
  KEY `ix_power_circuits_parent`   (`parent_circuit_id`),
  KEY `ix_power_circuits_location` (`location_id`),
  CONSTRAINT `fk_power_circuits_parent`   FOREIGN KEY (`parent_circuit_id`) REFERENCES `power_circuits`(`id`) ON DELETE RESTRICT,
  CONSTRAINT `fk_power_circuits_location` FOREIGN KEY (`location_id`)       REFERENCES `locations`(`id`)      ON DELETE SET NULL,
  CONSTRAINT `fk_power_circuits_source`   FOREIGN KEY (`source_id`)         REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='AC/DC branch circuits and PDU/UPS rails as a self-referencing tree (D5)';

-- ----------------------------------------------------------------------------
-- 5. rw_model_groups — port geometry + electrical extras per hardware model
-- ----------------------------------------------------------------------------
-- One live row per hardware_model. `groups_json` is the HANDOFF §6 `groups`
-- array verbatim (k/label/glyph/count/rows/role/gbps/watts/amps/volts/poe/
-- poeW/poeDrawW/batt/domain/conn) — stored as-is so the library round-trips
-- without a translation layer. `device_json` carries the device-level extras
-- that hardware_models has no column for (drawW, budgetW, poeBudgetW, eff,
-- isInternet, isBattery, ru, circuitA, circuitV, kind, note).
CREATE TABLE IF NOT EXISTS `rw_model_groups` (
  `id`                INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `hardware_model_id` INT UNSIGNED NOT NULL COMMENT 'FK: the product this geometry describes',
  `library_id`        VARCHAR(64) NULL COMMENT 'RW.DEVICES entry id (e.g. udm-pro-max) so the client merges by id, server wins',
  `groups_json`       LONGTEXT NOT NULL COMMENT 'HANDOFF §6 groups[] verbatim; RW.layout() input',
  `device_json`       LONGTEXT NULL COMMENT 'device-level library extras as a JSON object (drawW, budgetW, poeBudgetW, eff, isInternet, isBattery, …)',
  `notes`             TEXT NULL COMMENT 'free-form operator notes',
  `source_id`         SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`     ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`        DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`         TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_rw_model_groups_model_live`   (`hardware_model_id`,`live_flag`),
  UNIQUE KEY `uq_rw_model_groups_library_live` (`library_id`,`live_flag`),
  CONSTRAINT `ck_rw_model_groups_groups` CHECK (JSON_VALID(`groups_json`)),
  CONSTRAINT `ck_rw_model_groups_device` CHECK (`device_json` IS NULL OR JSON_VALID(`device_json`)),
  CONSTRAINT `fk_rw_model_groups_model`  FOREIGN KEY (`hardware_model_id`) REFERENCES `hardware_models`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_rw_model_groups_source` FOREIGN KEY (`source_id`)         REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='RackWire port geometry + electrical extras per hardware model (D8); one live row per model';

-- ----------------------------------------------------------------------------
-- 6. ENUM extension: interconnects.link_kind += 'power'
-- ----------------------------------------------------------------------------
-- Power cords become ordinary interconnects. Guarded on the literal "'power'"
-- appearing in COLUMN_TYPE, so a re-run is a no-op and the (expensive on a big
-- table) ALTER never repeats. Appended last — existing ordinals are untouched.
SET @has := (SELECT COUNT(*) FROM information_schema.COLUMNS
              WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'interconnects'
                AND COLUMN_NAME = 'link_kind' AND LOCATE("'power'", COLUMN_TYPE) > 0);
SET @sql := IF(@has = 0,
  "ALTER TABLE `interconnects` MODIFY `link_kind`
     ENUM('copper','fiber','moca','wireless','virtual','lldp_adjacency','logical','power')
     NOT NULL DEFAULT 'copper'
     COMMENT 'physical medium, or discovered/logical adjacency; power = AC/DC cord (RackWire)'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- ----------------------------------------------------------------------------
-- 7. ENUM extension: interfaces.if_kind += 'power_inlet','power_outlet'
-- ----------------------------------------------------------------------------
-- A device's C14 inlet and a PDU's C13 outlet are both `interfaces` rows, which
-- makes a power cord an interconnect between two of them with no new edge table.
SET @has := (SELECT COUNT(*) FROM information_schema.COLUMNS
              WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'interfaces'
                AND COLUMN_NAME = 'if_kind' AND LOCATE("'power_inlet'", COLUMN_TYPE) > 0
                                              AND LOCATE("'power_outlet'", COLUMN_TYPE) > 0);
SET @sql := IF(@has = 0,
  "ALTER TABLE `interfaces` MODIFY `if_kind`
     ENUM('physical','wifi','vlan','bridge','bond','loopback','tunnel','virtual','switch_port','other',
          'power_inlet','power_outlet')
     NOT NULL DEFAULT 'physical'
     COMMENT 'physical NIC vs logical construct; power_inlet/power_outlet are electrical ports (RackWire)'",
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- ----------------------------------------------------------------------------
-- 8. interfaces: circuit binding + electrical ratings
-- ----------------------------------------------------------------------------
-- Nullable and additive: purely network interfaces leave all three NULL.
--   circuit_id   — which power_circuits node this port draws from / feeds
--   watts_rated  — plate rating for an inlet, output rating for an outlet
--   poe_class    — what an ethernet port delivers (or draws, on a PD)
ALTER TABLE `interfaces`
  ADD COLUMN IF NOT EXISTS `circuit_id`  INT UNSIGNED NULL
      COMMENT 'FK: power circuit this port draws from (inlet) or feeds (outlet); NULL for non-power ports'
      AFTER `vlan_id`,
  ADD COLUMN IF NOT EXISTS `watts_rated` DECIMAL(8,2) NULL
      COMMENT 'rated draw (inlet) or rated output (outlet) in watts; NULL = unknown/not applicable'
      AFTER `circuit_id`,
  ADD COLUMN IF NOT EXISTS `poe_class`   ENUM('af','at','bt3','bt4','passive24','passive48') NULL
      COMMENT 'PoE standard this port delivers or draws; NULL = no PoE'
      AFTER `watts_rated`;

-- Index + FK added separately so a re-run does not trip on a duplicate name.
SET @has := (SELECT COUNT(*) FROM information_schema.STATISTICS
              WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'interfaces'
                AND INDEX_NAME = 'ix_interfaces_circuit');
SET @sql := IF(@has = 0,
  'ALTER TABLE `interfaces` ADD KEY `ix_interfaces_circuit` (`circuit_id`)',
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

SET @fk := (SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
             WHERE CONSTRAINT_SCHEMA = DATABASE() AND CONSTRAINT_NAME = 'fk_interfaces_circuit');
SET @sql := IF(@fk = 0,
  'ALTER TABLE `interfaces` ADD CONSTRAINT `fk_interfaces_circuit` FOREIGN KEY (`circuit_id`) REFERENCES `power_circuits`(`id`) ON DELETE SET NULL',
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- ============================================================================
-- End 020_rackwire.sql
-- ============================================================================
