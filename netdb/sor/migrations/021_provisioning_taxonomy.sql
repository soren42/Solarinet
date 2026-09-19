-- ============================================================================
-- 021_provisioning_taxonomy.sql — function taxonomy + IPAM live-uniqueness fix
-- ============================================================================
-- Phase 0 of the SolariNet Provisioning & Lifecycle plan
-- (docs/SolariNet_Provisioning_Plan.html §5 taxonomy, §6 allocation, §10 model).
--
-- Purpose:
--   1. `functions` — the closed function-tag taxonomy (database/auth/web/
--      security/dns/monitoring/base). Each tag carries the home network it
--      allocates from, its criticality tier, and its package/config profile
--      handles. This replaces the free-text `entities.role` as the driver for
--      IP allocation, package bundles, DNS and config (§5).
--   2. `entity_functions` — ORDERED, multi-tag membership of an entity in the
--      taxonomy, with exactly one primary tag per entity enforced in the
--      schema, and a declared `tag_order` for deterministic bundle layering
--      (second-review major: the model lacked ordered membership + a
--      single-primary invariant).
--   3. `function_incompatibility` — symmetric "these two tags may not coexist"
--      pairs, stored canonically (lo < hi) so a pair is recorded once
--      (second-review major: incompatibility had no table).
--   4. `inventory_snapshots` — append-only, immutable record of a rendered
--      fleet/inventory state, keyed by content hash, so a run can pin the exact
--      inventory it executed against (§11 immutable inventory snapshot;
--      second-review major: no snapshot table existed).
--   5. ip_addresses live-uniqueness fix (SECOND-REVIEW BLOCKER #3): the existing
--      `uq_ip_addr (address, deleted_at)` does NOT enforce one-live-owner —
--      MariaDB treats every NULL `deleted_at` as distinct, so two live rows can
--      share an address. The allocator's atomicity guarantee is unenforceable
--      without a real constraint. Add the 015 `live_flag` generated-column trick
--      and a UNIQUE over (address, live_flag) so the DB — not application logic,
--      which seed/discovery/direct writers all bypass — rejects a duplicate live
--      address.
--   6. A `sources` row (`slug` = 'provisioner') so rows the provisioning
--      subsystem asserts carry their own provenance and appliers can tell them
--      apart from seed/discovery/human writes.
--
-- Conventions followed (netdb/sor/SCHEMA.md §6, mirrors 015 and 020):
--   * Provenance quintet on every mutable table: source_id, asserted_kind,
--     asserted_at, created_at/updated_at/deleted_at.
--     Exception, deliberate: `inventory_snapshots` is append-only (an audit log)
--     and carries no updated_at/deleted_at — same shape as `barcode_scans` (015)
--     and `rw_plan_versions` (020).
--   * Soft-delete-aware uniques via the 015 `live_flag` generated column: live
--     rows get 1, deleted rows get NULL, and a UNIQUE over (…, live_flag) binds
--     only live rows (a UNIQUE key permits many NULLs).
--   * ENUM values are only ever appended, never reordered.
--   * Every statement is idempotent and re-runnable: CREATE TABLE IF NOT EXISTS,
--     information_schema-guarded index/constraint adds via PREPARE/EXECUTE
--     (pattern copied from 020_rackwire.sql:240-253 / 014_inventory.sql).
--
-- No CDC triggers are added here. Per the 020 precedent, trigger creation is
-- owned by the generated deploy/sorsync/sql/02-triggers.sql; when the run-queue
-- (§11) needs inventory regeneration to react to taxonomy edits, these tables
-- get added to that generator's tracked-table list — not hand-written here, so
-- ownership stays single.
--
-- Target: MariaDB >= 10.7 (INET6, generated columns), InnoDB, utf8mb4.
-- Database: `sor` on cesium (10.1.0.200). Apply on cesium ONLY; replication
-- carries it to benzene. Never apply on benzene directly.
--
-- Apply (GATED — do not run against live sor without explicit go):
--   sudo mariadb sor < netdb/sor/migrations/021_provisioning_taxonomy.sql
--
-- Validate first against a scratch/stage schema — never point tests at live sor.
-- ============================================================================

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

-- ----------------------------------------------------------------------------
-- 0. PREFLIGHT — abort BEFORE any DDL if live data would fail the new uniques
-- ----------------------------------------------------------------------------
-- MariaDB DDL is not transactional: each CREATE/ALTER auto-commits. If the
-- uq_ip_addr_live / one-primary-IP unique adds (section 6) failed on live
-- cesium because duplicates already exist, the new tables + generated columns
-- would already be committed, leaving a half-applied schema. So check first and
-- force a clear error (the mariadb CLI aborts a batch on first error by
-- default) before touching anything. On a clean DB both counts are 0 → no-op.
SET @dup_addr := (SELECT COUNT(*) FROM (
    SELECT `address` FROM `ip_addresses` WHERE `deleted_at` IS NULL
    GROUP BY `address` HAVING COUNT(*) > 1) d);
SET @dup_prim := (SELECT COUNT(*) FROM (
    SELECT `entity_id` FROM `ip_addresses`
    WHERE `deleted_at` IS NULL AND `is_primary` = 1 AND `entity_id` IS NOT NULL
    GROUP BY `entity_id` HAVING COUNT(*) > 1) d);
-- Section 6d/6e add CHECK (is_primary IN (0,1)) and CHECK (is_primary=0 OR
-- deleted_at IS NOT NULL OR entity_id IS NOT NULL). A live row violating either
-- would make that ALTER fail AFTER earlier DDL committed, so check here too.
-- @bad_prim: any out-of-range is_primary. @orphan_prim: a live primary with no
-- owning entity — such a row must be given an owner (or soft-deleted) before the
-- entity-required CHECK can hold.
SET @bad_prim := (SELECT COUNT(*) FROM `ip_addresses`
    WHERE `is_primary` NOT IN (0,1));
SET @orphan_prim := (SELECT COUNT(*) FROM `ip_addresses`
    WHERE `deleted_at` IS NULL AND `is_primary` = 1 AND `entity_id` IS NULL);
-- Selecting a non-existent column surfaces the reason in the error text and
-- aborts the script; harmless DO 0 when clean.
SET @sql := IF(@dup_addr > 0,
  'SELECT `021 ABORT: live duplicate addresses exist -- dedupe ip_addresses before applying` FROM `ip_addresses`',
  IF(@dup_prim > 0,
     'SELECT `021 ABORT: some entity has >1 live primary address -- fix before applying` FROM `ip_addresses`',
  IF(@bad_prim > 0,
     'SELECT `021 ABORT: ip_addresses.is_primary has values outside {0,1} -- fix before applying` FROM `ip_addresses`',
  IF(@orphan_prim > 0,
     'SELECT `021 ABORT: live primary address with NULL entity_id exists -- assign an owner before applying` FROM `ip_addresses`',
     'DO 0'))));
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- ----------------------------------------------------------------------------
-- 1. sources — register the provisioning subsystem as a provenance target
-- ----------------------------------------------------------------------------
-- ON DUPLICATE KEY so a re-run is a no-op AND a previously soft-deleted row is
-- restored (INSERT IGNORE would leave a deleted 'provisioner' in place, and the
-- allocator only resolves live sources — see allocator._sourceId).
INSERT INTO `sources` (`slug`,`display_name`,`kind`,`endpoint`)
VALUES ('provisioner', 'SolariNet provisioning subsystem', 'sync',
        'https://dashboard.akoria.net/provisioning/')
ON DUPLICATE KEY UPDATE
  `deleted_at`   = NULL,
  `display_name` = VALUES(`display_name`),
  `kind`         = VALUES(`kind`),
  `endpoint`     = VALUES(`endpoint`);

-- ----------------------------------------------------------------------------
-- 2. functions — the closed function-tag taxonomy
-- ----------------------------------------------------------------------------
-- One live row per tag slug. The tag set is CLOSED and seeded below; the table
-- is generic so a new tag is a data change, not a schema change. `network_id`
-- is the home segment a host with this PRIMARY tag allocates its identity
-- address from; NULL for tags that apply to every host regardless of segment
-- (e.g. 'base'). `criticality` is the 5-tier scale (1 = highest, matches the
-- lifecycle/criticality subsystem). `package_bundle` / `config_profile` are the
-- handles the LSV/config subsystems resolve; NULL means "same as slug".
CREATE TABLE IF NOT EXISTS `functions` (
  `id`             INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `slug`           VARCHAR(48) NOT NULL COMMENT 'stable tag handle, e.g. database, auth, web, base; unique among live rows',
  `display_name`   VARCHAR(96) NOT NULL COMMENT 'operator-facing label',
  `network_id`     INT UNSIGNED NULL COMMENT 'FK: home segment a host with this primary tag allocates from; NULL = segment-agnostic (base)',
  `criticality`    TINYINT UNSIGNED NOT NULL DEFAULT 3 COMMENT '5-tier criticality (1=highest .. 5=lowest); mirrors the criticality subsystem',
  `package_bundle` VARCHAR(64) NULL COMMENT 'package-bundle handle the LSV subsystem resolves; NULL = same as slug',
  `config_profile` VARCHAR(64) NULL COMMENT 'config-profile handle the configure subsystem resolves; NULL = same as slug',
  `description`    VARCHAR(255) NULL COMMENT 'human note on what the tag means',
  `source_id`      SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`  ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`     DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`      TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_functions_slug_live` (`slug`,`live_flag`),
  KEY `ix_functions_network` (`network_id`),
  CONSTRAINT `ck_functions_criticality` CHECK (`criticality` BETWEEN 1 AND 5),
  CONSTRAINT `fk_functions_network` FOREIGN KEY (`network_id`) REFERENCES `networks`(`id`) ON DELETE SET NULL,
  CONSTRAINT `fk_functions_source`  FOREIGN KEY (`source_id`)  REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Closed function-tag taxonomy: drives IP allocation, package bundle, DNS, config, criticality (plan §5)';

-- ----------------------------------------------------------------------------
-- 3. entity_functions — ordered, multi-tag membership with a single primary
-- ----------------------------------------------------------------------------
-- An entity may carry several tags; `tag_order` gives the deterministic layering
-- order for package/config resolution (lower applies first), and a UNIQUE over
-- (entity_id, tag_order, live_flag) forbids two live tags claiming the same slot.
-- At most one tag is the primary — it decides the home network and identity
-- address — enforced by the `primary_flag` generated column: it holds the
-- entity_id for the one live primary row and NULL otherwise, and a UNIQUE over it
-- permits at most one live primary per entity (many NULLs allowed). "Exactly one
-- live primary per tagged entity" is a stronger invariant that the tag-write
-- resolver (Phase 1, not yet built) must uphold; the schema guarantees "at most
-- one". `is_primary` is constrained to {0,1} so an out-of-range value cannot slip
-- past the generated-column test.
CREATE TABLE IF NOT EXISTS `entity_functions` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `entity_id`     BIGINT UNSIGNED NOT NULL COMMENT 'FK: the entity carrying the tag',
  `function_id`   INT UNSIGNED NOT NULL COMMENT 'FK: the function tag',
  `is_primary`    TINYINT(1) NOT NULL DEFAULT 0 COMMENT '1 = the entity''s primary tag (drives home network + identity address)',
  `tag_order`     SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'deterministic layering order for bundle/config resolution (lower first)',
  `notes`         VARCHAR(255) NULL COMMENT 'free-form note',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`     TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  -- entity_id for the one live primary row, NULL otherwise → UNIQUE = at most one live primary/entity.
  `primary_flag`  BIGINT UNSIGNED AS (IF(`is_primary` = 1 AND `deleted_at` IS NULL, `entity_id`, NULL)) PERSISTENT
                  COMMENT 'holds entity_id for the single live primary tag; UNIQUE enforces one-primary-per-entity',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_entity_functions_live`    (`entity_id`,`function_id`,`live_flag`),
  UNIQUE KEY `uq_entity_functions_order`   (`entity_id`,`tag_order`,`live_flag`),
  UNIQUE KEY `uq_entity_functions_primary` (`primary_flag`),
  KEY `ix_entity_functions_function` (`function_id`),
  CONSTRAINT `ck_entity_functions_primary` CHECK (`is_primary` IN (0,1)),
  CONSTRAINT `fk_entity_functions_entity`   FOREIGN KEY (`entity_id`)   REFERENCES `entities`(`id`)  ON DELETE CASCADE,
  CONSTRAINT `fk_entity_functions_function` FOREIGN KEY (`function_id`) REFERENCES `functions`(`id`) ON DELETE RESTRICT,
  CONSTRAINT `fk_entity_functions_source`   FOREIGN KEY (`source_id`)   REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Ordered multi-tag membership of entities in the function taxonomy; one live primary tag per entity (plan §5/§10)';

-- ----------------------------------------------------------------------------
-- 4. function_incompatibility — symmetric "cannot coexist" pairs, canonical
-- ----------------------------------------------------------------------------
-- Stored canonically with lo < hi so each unordered pair is recorded exactly
-- once; the CHECK rejects a reversed or self pair, the UNIQUE rejects a live
-- duplicate. The taxonomy resolver reads this to reject an entity_functions
-- combination that pairs two incompatible tags.
CREATE TABLE IF NOT EXISTS `function_incompatibility` (
  `id`             INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `function_lo_id` INT UNSIGNED NOT NULL COMMENT 'FK: lower function id of the incompatible pair (canonical: lo < hi)',
  `function_hi_id` INT UNSIGNED NOT NULL COMMENT 'FK: higher function id of the incompatible pair',
  `reason`         VARCHAR(255) NULL COMMENT 'why the two tags may not coexist',
  `source_id`      SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`  ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`     DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`      TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_function_incompat_live` (`function_lo_id`,`function_hi_id`,`live_flag`),
  KEY `ix_function_incompat_hi` (`function_hi_id`),
  CONSTRAINT `ck_function_incompat_order` CHECK (`function_lo_id` < `function_hi_id`),
  CONSTRAINT `fk_function_incompat_lo`     FOREIGN KEY (`function_lo_id`) REFERENCES `functions`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_function_incompat_hi`     FOREIGN KEY (`function_hi_id`) REFERENCES `functions`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_function_incompat_source` FOREIGN KEY (`source_id`)      REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Symmetric incompatible function-tag pairs, stored canonically lo<hi (plan §5)';

-- ----------------------------------------------------------------------------
-- 5. inventory_snapshots — append-only immutable rendered-inventory records
-- ----------------------------------------------------------------------------
-- Backs §11's "immutable inventory snapshot": a run pins the exact inventory it
-- executed against by referencing a snapshot id. Append-only audit log — rows
-- are never edited or soft-deleted, so no updated_at/deleted_at (same shape as
-- barcode_scans/rw_plan_versions). Immutability is enforced operationally by
-- GRANTing runtime roles INSERT/SELECT only (no UPDATE/DELETE) — the house
-- pattern for append-only tables.
--
-- `content_hash` is a PERSISTENT generated column = SHA2(content,256): it cannot
-- disagree with `content`, so a run that pins a snapshot id can trust the hash
-- as a genuine fingerprint (the review flagged a hand-supplied hash as
-- untrustworthy). Indexed but NOT unique — the same inventory legitimately
-- re-snapshots at different times.
CREATE TABLE IF NOT EXISTS `inventory_snapshots` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK; the snapshot id a run pins',
  `content`       LONGTEXT NOT NULL COMMENT 'rendered inventory payload (JSON) at snapshot time',
  `content_hash`  CHAR(64) AS (SHA2(`content`, 256)) PERSISTENT COMMENT 'sha256 hex of content; generated so it cannot diverge',
  `entity_count`  INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'denormalized host count for the list endpoint (no blob read)',
  `notes`         VARCHAR(255) NULL COMMENT 'free-form note, e.g. what triggered the snapshot',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'machine' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  PRIMARY KEY (`id`),
  KEY `ix_inventory_snapshots_hash` (`content_hash`),
  KEY `ix_inventory_snapshots_created` (`created_at`),
  CONSTRAINT `ck_inventory_snapshots_json`   CHECK (JSON_VALID(`content`)),
  CONSTRAINT `fk_inventory_snapshots_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Append-only immutable snapshots of rendered inventory; a run pins one (plan §11)';

-- ----------------------------------------------------------------------------
-- 6. ip_addresses — enforce ONE LIVE OWNER per address (second-review blocker)
-- ----------------------------------------------------------------------------
-- The shipped `uq_ip_addr (address, deleted_at)` does not enforce live
-- uniqueness: MariaDB treats every NULL deleted_at as distinct, so two live rows
-- can hold the same address and the allocator's atomicity is unbacked. Add the
-- 015 live_flag generated column + a UNIQUE over (address, live_flag) so the
-- database rejects a duplicate live address regardless of which writer (seed,
-- discovery, allocator, or a direct INSERT) attempts it.
--
-- Guarded so a re-run is a no-op. If this ALTER fails on live sor because two
-- live rows already share an address, that is a real data defect to resolve
-- BEFORE the constraint can hold — that is the point of the constraint.

-- 6a. live_flag generated column (ADD COLUMN IF NOT EXISTS is idempotent).
ALTER TABLE `ip_addresses`
  ADD COLUMN IF NOT EXISTS `live_flag` TINYINT
      AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT
      COMMENT 'soft-delete-aware unique helper (added 021)'
      AFTER `deleted_at`;

-- 6b. UNIQUE(address, live_flag), guarded on information_schema so re-runs skip.
SET @has := (SELECT COUNT(*) FROM information_schema.STATISTICS
              WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ip_addresses'
                AND INDEX_NAME = 'uq_ip_addr_live');
SET @sql := IF(@has = 0,
  'ALTER TABLE `ip_addresses`
     ADD UNIQUE KEY `uq_ip_addr_live` (`address`,`live_flag`)',
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- 6c. fk_ip_entity: ON DELETE SET NULL -> ON DELETE RESTRICT.
--
-- This MUST run before 6d creates the primary_entity_flag generated column.
-- MariaDB forbids a generated column (and a CHECK) from referencing a column
-- whose FK carries a *mutating* referential action such as ON DELETE SET NULL —
-- the action could change entity_id outside DML and desync a stored value
-- (error 1901). Empirically a VIRTUAL column over such a column is accepted on
-- 11.8, but that leans on a version-specific allowance; converting the FK to a
-- non-mutating action (RESTRICT) first removes the dependency entirely, so 6d's
-- column and 6f's CHECK are unconditionally legal on every MariaDB >= 10.7.
--
-- RESTRICT is also the behaviour we want: an entity that still owns any
-- ip_addresses row (live or soft-deleted) cannot be hard-deleted out from under
-- it. Hard entity delete is not a normal SoR path (retirement is a soft-delete);
-- RESTRICT converts the residual orphan-a-live-primary hole into a loud FK error
-- instead of a silent SET NULL. The section-0 preflight (@orphan_prim) has
-- already refused to reach here if any live primary currently lacks an owner.
--
-- Idempotent AND self-healing, guarded on the current DELETE_RULE:
--   RESTRICT  -> both steps are no-ops (already converted).
--   SET NULL  -> DROP the FK, then re-ADD it RESTRICT.
--   NULL (FK absent, e.g. a prior run died between the DROP and the re-ADD)
--             -> skip the DROP, but still ADD the RESTRICT FK so a re-run heals
--                the half-applied state rather than leaving entity_id unbacked.
-- A same-named DROP+ADD cannot be combined into one ALTER — InnoDB rejects
-- reusing a live FK name in a single statement (errno 121) — so this is two
-- auto-committing ALTERs. The re-add references the same (entity_id ->
-- entities.id) shape schema.sql declares; a mid-step failure aborts the CLI
-- loudly and a re-run (guard now sees @rule IS NULL) completes the ADD.
SET @rule := (SELECT DELETE_RULE FROM information_schema.REFERENTIAL_CONSTRAINTS
               WHERE CONSTRAINT_SCHEMA = DATABASE()
                 AND TABLE_NAME = 'ip_addresses'
                 AND CONSTRAINT_NAME = 'fk_ip_entity');
-- DROP only when a non-RESTRICT FK is actually present (i.e. SET NULL). Skip the
-- DROP when @rule IS RESTRICT (done) or NULL (FK missing — nothing to drop).
SET @sql := IF(@rule IS NULL OR @rule = 'RESTRICT',
  'DO 0',
  'ALTER TABLE `ip_addresses` DROP FOREIGN KEY `fk_ip_entity`');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;
-- ADD whenever the FK is not already RESTRICT: after a SET NULL->drop above, OR
-- when @rule IS NULL (FK absent — heal it). Only skip when already RESTRICT.
SET @sql := IF(@rule = 'RESTRICT',
  'DO 0',
  'ALTER TABLE `ip_addresses`
     ADD CONSTRAINT `fk_ip_entity` FOREIGN KEY (`entity_id`)
         REFERENCES `entities` (`id`) ON DELETE RESTRICT');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- 6d. ONE LIVE PRIMARY (identity) ADDRESS PER ENTITY. is_primary=1 rows render
-- into DNS (v_dns_hosts A+PTR); two live primaries for one entity produce a
-- duplicate/ambiguous A record and let the allocator hand an entity a second
-- identity address. `primary_entity_flag` holds the entity_id for the one live
-- primary-with-owner row and NULL otherwise; UNIQUE over it permits at most one
-- (many NULLs — non-primary rows, and any primary that lacks an entity_id, don't
-- collide). It therefore enforces one-primary-per-entity for every row that HAS
-- an owner; section 6f's CHECK guarantees a live primary always HAS an owner, so
-- the two together make "one live identity IP per entity, always owned" a DB
-- invariant (the allocator's _reconcileEntity is defence in depth, not the only
-- guard).
-- Same generated-column trick as entity_functions.primary_flag, but VIRTUAL not
-- PERSISTENT. fk_ip_entity is now ON DELETE RESTRICT (6c above), so referencing
-- entity_id from a generated column is unconditionally legal; VIRTUAL is kept
-- because rebuilding it PERSISTENT would buy nothing and InnoDB still supports a
-- UNIQUE index over a VIRTUAL column.
ALTER TABLE `ip_addresses`
  ADD COLUMN IF NOT EXISTS `primary_entity_flag` BIGINT UNSIGNED
      AS (IF(`is_primary` = 1 AND `deleted_at` IS NULL, `entity_id`, NULL)) VIRTUAL
      COMMENT 'entity_id for the single live primary address; UNIQUE = one identity IP/entity (added 021)'
      AFTER `live_flag`;

SET @has := (SELECT COUNT(*) FROM information_schema.STATISTICS
              WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ip_addresses'
                AND INDEX_NAME = 'uq_ip_addr_primary_entity');
SET @sql := IF(@has = 0,
  'ALTER TABLE `ip_addresses`
     ADD UNIQUE KEY `uq_ip_addr_primary_entity` (`primary_entity_flag`)',
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- 6e. is_primary integrity CHECK, guarded on information_schema.CHECK_CONSTRAINTS
-- so a re-run skips (ADD CONSTRAINT is not idempotent on its own). is_primary is
-- constrained to strictly {0,1}, so the primary_entity_flag generated column
-- (which tests is_primary = 1) cannot be fooled by an out-of-range value —
-- mirroring entity_functions.ck_entity_functions_primary.
SET @has := (SELECT COUNT(*) FROM information_schema.CHECK_CONSTRAINTS
              WHERE CONSTRAINT_SCHEMA = DATABASE()
                AND TABLE_NAME = 'ip_addresses'
                AND CONSTRAINT_NAME = 'ck_ip_addr_is_primary');
SET @sql := IF(@has = 0,
  'ALTER TABLE `ip_addresses`
     ADD CONSTRAINT `ck_ip_addr_is_primary` CHECK (`is_primary` IN (0,1))',
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- 6f. LIVE PRIMARY ⇒ entity_id IS NOT NULL, enforced declaratively. Legal now
-- that 6c made fk_ip_entity ON DELETE RESTRICT (a non-mutating action), so a
-- CHECK over entity_id no longer trips error 1901. The section-0 preflight
-- (@orphan_prim) already refused to reach here if a live orphan primary exists,
-- so this ADD cannot fail on existing data.

-- A row is legal when it is not a live primary (is_primary=0 OR soft-deleted) or
-- it names an owning entity.
SET @has := (SELECT COUNT(*) FROM information_schema.CHECK_CONSTRAINTS
              WHERE CONSTRAINT_SCHEMA = DATABASE()
                AND TABLE_NAME = 'ip_addresses'
                AND CONSTRAINT_NAME = 'ck_ip_addr_primary_has_entity');
SET @sql := IF(@has = 0,
  'ALTER TABLE `ip_addresses`
     ADD CONSTRAINT `ck_ip_addr_primary_has_entity`
         CHECK (`is_primary` = 0 OR `deleted_at` IS NOT NULL OR `entity_id` IS NOT NULL)',
  'DO 0');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- ----------------------------------------------------------------------------
-- 7. Seed the closed function-tag set
-- ----------------------------------------------------------------------------
-- Idempotent: INSERT ... ON DUPLICATE KEY UPDATE against the live-slug unique.
-- source_id resolves to the provisioner row created in section 1. network_id is
-- left NULL here (segment wiring is a data step done once UniFi import lands,
-- the Phase-0 prerequisite) except where the mapping is unambiguous today.
-- Criticality: security/auth/dns/database = tier 1-2 (fleet depends on them),
-- web = 3, monitoring = 3, base = 4.
INSERT INTO `functions` (`slug`,`display_name`,`criticality`,`description`,`source_id`,`asserted_kind`)
SELECT t.slug, t.display_name, t.criticality, t.description,
       (SELECT id FROM sources WHERE slug='provisioner'), 'human'
FROM (
  SELECT 'base'       AS slug, 'Base host'          AS display_name, 4 AS criticality, 'Common baseline applied to every host regardless of segment' AS description
  UNION ALL SELECT 'database',   'Database server',   2, 'MariaDB / SoR data-tier hosts'
  UNION ALL SELECT 'auth',       'Directory / auth',  1, 'Samba AD / Keycloak identity hosts'
  UNION ALL SELECT 'web',        'Web / dashboard',   3, 'HTTP application and dashboard hosts'
  UNION ALL SELECT 'security',   'Security / CA',     1, 'CA, secrets, IDS and security-plane hosts'
  UNION ALL SELECT 'dns',        'DNS server',        1, 'Authoritative / resolver DNS hosts'
  UNION ALL SELECT 'monitoring', 'Monitoring',        3, 'solariMonitor collectors and probes'
) AS t
ON DUPLICATE KEY UPDATE
  `display_name` = VALUES(`display_name`),
  `criticality`  = VALUES(`criticality`),
  `description`  = VALUES(`description`);

-- ============================================================================
-- End 021_provisioning_taxonomy.sql
-- ============================================================================
