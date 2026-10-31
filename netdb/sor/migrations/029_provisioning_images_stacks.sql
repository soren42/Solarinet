-- ============================================================================
-- 029_provisioning_images_stacks.sql — OS image warehouse + software stacks
-- ============================================================================
-- Phase P1 of the Provisioning Images & Stacks feature
-- (docs/Provisioning_UI_Design.md §3 items 3–6, §4 P1).
--
-- Purpose:
--   1. `prov_images` — one row per real distro × release × arch combination the
--      provisioning host can install, with how it boots (iPXE / Pi-native
--      netboot / write-to-media), where it is fetched from, the fetched version
--      and sha256, the unattended-install template, and the refresh policy.
--   2. `prov_stacks` — named, architecture-independent software stacks (Core,
--      Web, AI, …). `locked` marks a stack that is always included (Core).
--   3. `prov_stack_items` — the LOGICAL contents of a stack, one of six item
--      types (pkg, repo, artifact, script, build, role) with a per-family name
--      map, applicability filters and a per-type JSON `spec`.
--   4. `prov_stack_artifacts` — per-arch download URL + sha256 for `artifact`
--      items.
--   5. `prov_stack_deps` — stack → depends-on-stack edges (AI → Core).
--   6. `prov_runs` — audit log of every reimage / apply-stacks run.
--
-- The generator (deploy/fleet/stackgen.py) reads these tables and renders the
-- Ansible vars (`solari_stack_*`) for one target family + arch. It owns the
-- rules the schema cannot express: dependency closure, cycle rejection, Core
-- first, and hard errors for unmapped packages or missing per-arch artifacts.
--
-- ITEM TYPES (prov_stack_items.item_type; ENUM order is fixed, append-only)
--   pkg      distro-native package; real name per family. `repo_ref` may name a
--            repo item that must be applied before it.
--   repo     third-party zypper/apt/dnf repo added before packages. spec =
--            per-family {url, key_url, fingerprint[, suite, components]}.
--   artifact upstream release binary/tarball; `version_pin` required; per-arch
--            URL + sha256 in prov_stack_artifacts (arch 'any' = all arches).
--   script   install script: spec.body (inline, stored here) XOR spec.url +
--            spec.sha256 (remote, pinned — an unpinned curl|sh is rejected by
--            CHECK). interpreter (default /bin/sh), run_as root|admin, env
--            (non-secret), idempotency guard creates and/or check
--            (+ optional version_check regex compared with version_pin).
--   build    source build: spec.source {git} at `version_pin` ref, or
--            {tarball, sha256}; system cmake|make|autotools|meson|custom;
--            configure_args, build_deps (logical pkg names resolved through
--            the same family map), prefix, commands (custom), same guard.
--   role     existing Ansible role named by `ansible_role`.
--
-- WHY A JSON `spec` (not side tables): the per-type fields are heterogeneous,
-- mostly 1:1 with the item, and are edited as one form (P3). Six side tables
-- would add joins and rollback surface for no query that needs them. The
-- fields that ARE queried or safety-critical stay real columns (item_type,
-- family names, applicability, version_pin, ansible_role, repo_ref, ordering),
-- the shape of spec is CHECKed where it guards safety (script pinning, build
-- source present, spec present/absent per type), and deploy/fleet/stackgen.py
-- validates the rest strictly before anything renders.
--
-- PER-FAMILY NAME CONVENTION (name_suse / name_debian / name_fedora)
--   * pkg:              the real package name on that family. NULL = no
--                       package there (render is a hard error). '=' rejected.
--   * every other type: '=' = supported on that family. NULL = unsupported
--                       (hard error). Any other value is rejected by CHECK.
--   APPLICABILITY (applies_families / applies_arches, SET, NULL = all) is the
--   deliberate opt-out: an item not applicable to the target is SKIPPED (and
--   reported), whereas an applicable item with a NULL family name is an error.
--
-- ITEM ORDER within a stack: `item_order` when set, else the type default
--   repo 100 → pkg 200 → artifact 300 → build 400 → script 500 → role 600;
--   ties by type, then `sort_order`, then logical_name.

-- RELATIONSHIP TO `functions` (021): unchanged. A function's `package_bundle`
-- handle MAY name a prov_stacks.slug (future link: tag → default stacks).
-- Nothing enforces or reads that link yet.
--
-- `prov_runs` AUDIT POSTURE: append-only in the sense that a run row is never
-- deleted and its request columns (mode, target, image, stacks, requester,
-- dry-run, requested_at) are immutable; only the lifecycle columns (status,
-- started_at, finished_at, detail) advance, and a terminal status is final.
-- Enforced by BEFORE UPDATE / BEFORE DELETE triggers, not just grants. Like
-- inventory_snapshots it carries no updated_at/deleted_at.
--
-- CDC: deliberately none. No sorsync applier consumes these tables, and a new
-- outbox table name would be a MANDATORY publish in sor_emitd (unroutable →
-- the emit checkpoint stalls for every table) unless the deployed
-- sorsync.conf `optional_tables` is updated in lockstep. Stacks are pulled by
-- the generator on demand, like RackWire's dedicated-API tables (§7).
--
-- SEED: in this migration (sections 7–8). Seed-once semantics: each seed row is
-- inserted only when no row (live OR soft-deleted) with that natural key exists,
-- so a re-run never clobbers operator edits nor resurrects a deliberate delete.
-- `core` reproduces group_vars/all.yml solari_core_packages through
-- solari_pkg_map exactly; tests/deploy/test_stackgen.py and
-- tests/netdb/test_prov_images_stacks.sh hold it to that. Images are seeded only
-- for paths the netboot tree / fetch-images.sh / pi builders really carry;
-- no sha256 or version is invented (NULL until the first refresh).
--
-- Conventions followed (netdb/sor/SCHEMA.md §6/§7, mirrors 021):
--   * Provenance quintet on every mutable table; live_flag generated-column
--     soft-delete uniques; ENUM values only ever appended.
--   * Idempotent: CREATE TABLE IF NOT EXISTS, CREATE OR REPLACE TRIGGER, guarded
--     seeds.
--
-- Target: MariaDB >= 10.7. Database: `sor` on cesium (10.1.0.200) ONLY;
-- replication carries it to benzene. Apply through tools/migrate.py.
-- Manual rollback: netdb/sor/rollback/029-down.sql.
-- Validate on a scratch schema first — never point tests at live sor.
-- ============================================================================

SET NAMES utf8mb4;
SET sql_mode = 'STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';

-- ----------------------------------------------------------------------------
-- 1. prov_images — installable distro × release × arch rows
-- ----------------------------------------------------------------------------
-- `release_name` (not `release`: RELEASE is a reserved word in MariaDB) is the
-- machine handle ('12', 'tumbleweed', '15.6'); `release_label` is the operator
-- label ('12 (bookworm)'). `version` / `sha256` / `fetched_at` describe the
-- artifact last fetched by IMAGE_REFRESH (P3); NULL = never fetched.
-- `netboot_verified` = an operator has watched a real machine install from it.
CREATE TABLE IF NOT EXISTS `prov_images` (
  `id`               INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `distro`           VARCHAR(32)  NOT NULL COMMENT 'distro handle as used by fetch-images.sh: opensuse, debian, ubuntu, fedora, raspios',
  `family`           ENUM('suse','debian','fedora') NOT NULL COMMENT 'package family; selects prov_stack_items.name_<family>',
  `release_name`     VARCHAR(48)  NOT NULL COMMENT 'release handle, e.g. 12, 13, tumbleweed, 15.6, 24.04, bookworm',
  `release_label`    VARCHAR(96)  NULL COMMENT 'operator-facing release label, e.g. 12 (bookworm)',
  `arch`             ENUM('x86_64','arm64','armhf','riscv64','i386') NOT NULL COMMENT 'CPU architecture',
  `boot_method`      ENUM('ipxe','pi_netboot','media') NOT NULL COMMENT 'ipxe = netboot via benzene iPXE; pi_netboot = Pi-native network boot; media = write image to card/disk',
  `source_url`       VARCHAR(512) NULL COMMENT 'upstream location of the kernel+initrd directory or image',
  `checksum_url`     VARCHAR(512) NULL COMMENT 'upstream checksum list (e.g. SHA256SUMS); NULL = none known',
  `version`          VARCHAR(64)  NULL COMMENT 'upstream version/build id last fetched; NULL = never fetched',
  `sha256`           CHAR(64)     NULL COMMENT 'sha256 of the fetched artifact; NULL = never fetched/verified',
  `fetched_at`       DATETIME     NULL COMMENT 'when the artifact was last fetched; NULL = never',
  `install_template` VARCHAR(128) NULL COMMENT 'unattended-install template under deploy/fleet/netboot/configs/; NULL = image-based',
  `refresh_policy`   ENUM('manual','weekly','point_release') NOT NULL DEFAULT 'manual' COMMENT 'when IMAGE_REFRESH should re-fetch',
  `netboot_verified` TINYINT(1)   NOT NULL DEFAULT 0 COMMENT '1 = a real install from this row has been observed end to end',
  `notes`            VARCHAR(255) NULL COMMENT 'free-form operator note',
  `source_id`        SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`    ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`       DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`        TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_images_live` (`distro`,`release_name`,`arch`,`live_flag`),
  KEY `ix_prov_images_family_arch` (`family`,`arch`),
  KEY `ix_prov_images_source` (`source_id`),
  CONSTRAINT `ck_prov_images_verified` CHECK (`netboot_verified` IN (0,1)),
  CONSTRAINT `ck_prov_images_sha256`   CHECK (`sha256` IS NULL OR `sha256` REGEXP '^[0-9a-f]{64}$'),
  CONSTRAINT `ck_prov_images_pi_arch`  CHECK (`boot_method` <> 'pi_netboot' OR `arch` IN ('arm64','armhf')),
  CONSTRAINT `ck_prov_images_slug`     CHECK (`distro` REGEXP '^[a-z0-9][a-z0-9-]*$'),
  CONSTRAINT `fk_prov_images_source`   FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Installable OS images: real distro x release x arch rows with boot method and fetch state';

-- ----------------------------------------------------------------------------
-- 2. prov_stacks — named architecture-independent software stacks
-- ----------------------------------------------------------------------------
-- `locked` = always included in every run and ordered before every unlocked
-- stack (the generator treats each unlocked stack as implicitly depending on
-- every locked one). `sort_order` is the stable tie-break inside the
-- dependency order (lower first, then slug).
CREATE TABLE IF NOT EXISTS `prov_stacks` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `slug`          VARCHAR(48)  NOT NULL COMMENT 'stable handle, e.g. core, web, ai; unique among live rows',
  `name`          VARCHAR(96)  NOT NULL COMMENT 'operator-facing name',
  `description`   VARCHAR(255) NULL COMMENT 'what the stack provides',
  `locked`        TINYINT(1)   NOT NULL DEFAULT 0 COMMENT '1 = always included and ordered first (Core)',
  `sort_order`    SMALLINT UNSIGNED NOT NULL DEFAULT 100 COMMENT 'stable tie-break within dependency order (lower first)',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`     TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_stacks_slug_live` (`slug`,`live_flag`),
  KEY `ix_prov_stacks_source` (`source_id`),
  CONSTRAINT `ck_prov_stacks_locked` CHECK (`locked` IN (0,1)),
  CONSTRAINT `ck_prov_stacks_slug`   CHECK (`slug` REGEXP '^[a-z0-9][a-z0-9-]*$'),
  CONSTRAINT `fk_prov_stacks_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Named arch-independent software stacks; locked = always included (Core)';

-- ----------------------------------------------------------------------------
-- 3. prov_stack_items — logical stack contents (six item types)
-- ----------------------------------------------------------------------------
-- See the header for types, the name_<family> convention, applicability and
-- ordering. CHECKs make the safety-relevant rules declarative.
CREATE TABLE IF NOT EXISTS `prov_stack_items` (
  `id`               INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `stack_id`         INT UNSIGNED NOT NULL COMMENT 'FK: owning stack',
  `logical_name`     VARCHAR(96)  NOT NULL COMMENT 'arch/distro-independent item name, e.g. netcat, docker-ce repo',
  `item_type`        ENUM('pkg','repo','artifact','script','build','role') NOT NULL DEFAULT 'pkg' COMMENT 'pkg | repo | artifact | script | build | role (see migration header)',
  `name_suse`        VARCHAR(128) NULL COMMENT 'pkg: real openSUSE name; other types: = when supported; NULL = unsupported',
  `name_debian`      VARCHAR(128) NULL COMMENT 'pkg: real Debian/Ubuntu name; other types: = when supported; NULL = unsupported',
  `name_fedora`      VARCHAR(128) NULL COMMENT 'pkg: real Fedora name; other types: = when supported; NULL = unsupported',
  `applies_families` SET('suse','debian','fedora') NULL COMMENT 'families this item applies to; NULL = all; non-applicable targets skip it',
  `applies_arches`   SET('x86_64','arm64','armhf','riscv64','i386') NULL COMMENT 'arches this item applies to; NULL = all; non-applicable targets skip it',
  `ansible_role`     VARCHAR(64)  NULL COMMENT 'role items only: Ansible role under deploy/fleet/ansible/roles',
  `repo_ref`         VARCHAR(96)  NULL COMMENT 'pkg items only: logical_name of a repo item that must be applied first',
  `source_url`       VARCHAR(512) NULL COMMENT 'informational project/release home',
  `version_pin`      VARCHAR(64)  NULL COMMENT 'pinned version / git ref; required for artifact and build; compared by script/build version_check',
  `spec`             LONGTEXT     NULL COMMENT 'per-type JSON object (repo/script/build required, artifact optional, pkg/role none); validated by stackgen.py',
  `item_order`       SMALLINT UNSIGNED NULL COMMENT 'explicit position within the stack; NULL = type default (repo 100 .. role 600)',
  `sort_order`       SMALLINT UNSIGNED NOT NULL DEFAULT 100 COMMENT 'tie-break within the same position (lower first, then logical_name)',
  `notes`            VARCHAR(255) NULL COMMENT 'free-form operator note',
  `source_id`        SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`    ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`       DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`        TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_stack_items_live` (`stack_id`,`logical_name`,`live_flag`),
  KEY `ix_prov_stack_items_type` (`item_type`),
  KEY `ix_prov_stack_items_source` (`source_id`),
  CONSTRAINT `ck_prov_stack_items_role`     CHECK ((`item_type` = 'role') = (`ansible_role` IS NOT NULL)),
  CONSTRAINT `ck_prov_stack_items_repo_ref` CHECK (`repo_ref` IS NULL OR `item_type` = 'pkg'),
  CONSTRAINT `ck_prov_stack_items_pin`      CHECK (`item_type` NOT IN ('artifact','build') OR `version_pin` IS NOT NULL),
  CONSTRAINT `ck_prov_stack_items_suse`     CHECK (`name_suse`   IS NULL OR (`item_type` = 'pkg') = (`name_suse`   <> '=' AND `name_suse`   <> '')),
  CONSTRAINT `ck_prov_stack_items_debian`   CHECK (`name_debian` IS NULL OR (`item_type` = 'pkg') = (`name_debian` <> '=' AND `name_debian` <> '')),
  CONSTRAINT `ck_prov_stack_items_fedora`   CHECK (`name_fedora` IS NULL OR (`item_type` = 'pkg') = (`name_fedora` <> '=' AND `name_fedora` <> '')),
  CONSTRAINT `ck_prov_stack_items_spec`     CHECK (`spec` IS NULL OR (JSON_VALID(`spec`) AND JSON_TYPE(`spec`) = 'OBJECT')),
  CONSTRAINT `ck_prov_stack_items_spec_req` CHECK (CASE `item_type`
                                                     WHEN 'pkg'  THEN `spec` IS NULL
                                                     WHEN 'role' THEN `spec` IS NULL
                                                     WHEN 'artifact' THEN TRUE
                                                     ELSE `spec` IS NOT NULL END),
  -- script: inline body XOR remote url, and a remote url MUST carry a sha256.
  CONSTRAINT `ck_prov_stack_items_script`   CHECK (`item_type` <> 'script' OR
                                                   (JSON_EXISTS(`spec`,'$.body') AND NOT JSON_EXISTS(`spec`,'$.url')) OR
                                                   (JSON_EXISTS(`spec`,'$.url') AND JSON_EXISTS(`spec`,'$.sha256') AND NOT JSON_EXISTS(`spec`,'$.body'))),
  CONSTRAINT `ck_prov_stack_items_build`    CHECK (`item_type` <> 'build' OR (JSON_EXISTS(`spec`,'$.source') AND JSON_EXISTS(`spec`,'$.system'))),
  CONSTRAINT `fk_prov_stack_items_stack`    FOREIGN KEY (`stack_id`)  REFERENCES `prov_stacks`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_prov_stack_items_source`   FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Logical stack items: pkg/repo/artifact/script/build/role, per-family names, per-type JSON spec';

-- ----------------------------------------------------------------------------
-- 4. prov_stack_artifacts — per-arch download for artifact items
-- ----------------------------------------------------------------------------
-- arch 'any' = one artifact serves every arch (scripts, jars); an exact-arch
-- row wins over 'any'. Only meaningful for `artifact` items — the generator
-- rejects artifact rows on any other item type. A NULL sha256 is a render-time
-- hard error (nothing is fetched unpinned).
CREATE TABLE IF NOT EXISTS `prov_stack_artifacts` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `item_id`       INT UNSIGNED NOT NULL COMMENT 'FK: the artifact stack item',
  `arch`          ENUM('any','x86_64','arm64','armhf','riscv64','i386') NOT NULL COMMENT 'target arch; any = arch-independent',
  `url`           VARCHAR(512) NOT NULL COMMENT 'download URL for this arch at the item''s version_pin',
  `sha256`        CHAR(64)     NULL COMMENT 'expected sha256 of the download; NULL = not yet recorded (render is a hard error)',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`     TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_stack_artifacts_live` (`item_id`,`arch`,`live_flag`),
  KEY `ix_prov_stack_artifacts_source` (`source_id`),
  CONSTRAINT `ck_prov_stack_artifacts_sha256` CHECK (`sha256` IS NULL OR `sha256` REGEXP '^[0-9a-f]{64}$'),
  CONSTRAINT `fk_prov_stack_artifacts_item`   FOREIGN KEY (`item_id`)   REFERENCES `prov_stack_items`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_prov_stack_artifacts_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Per-arch download URL + sha256 for artifact stack items (any = arch-independent)';

-- ----------------------------------------------------------------------------
-- 5. prov_stack_deps — stack → depends-on-stack edges
-- ----------------------------------------------------------------------------
-- Self-dependency is rejected here; longer cycles are rejected by the
-- generator (a CHECK cannot see other rows).
CREATE TABLE IF NOT EXISTS `prov_stack_deps` (
  `id`                  INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `stack_id`            INT UNSIGNED NOT NULL COMMENT 'FK: the dependent stack',
  `depends_on_stack_id` INT UNSIGNED NOT NULL COMMENT 'FK: the stack it requires (applied earlier)',
  `source_id`           SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`       ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`          DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`           TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_stack_deps_live` (`stack_id`,`depends_on_stack_id`,`live_flag`),
  KEY `ix_prov_stack_deps_on` (`depends_on_stack_id`),
  KEY `ix_prov_stack_deps_source` (`source_id`),
  CONSTRAINT `ck_prov_stack_deps_self`   CHECK (`stack_id` <> `depends_on_stack_id`),
  CONSTRAINT `fk_prov_stack_deps_stack`  FOREIGN KEY (`stack_id`)            REFERENCES `prov_stacks`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_prov_stack_deps_on`     FOREIGN KEY (`depends_on_stack_id`) REFERENCES `prov_stacks`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_prov_stack_deps_source` FOREIGN KEY (`source_id`)           REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Stack dependency edges; self-dependency rejected, cycles rejected by the generator';

-- ----------------------------------------------------------------------------
-- 6. prov_runs — audit log of reimage / apply-stacks runs
-- ----------------------------------------------------------------------------
-- `stack_slugs` is the ORDERED, dependency-closed list actually run (the
-- generator's solari_stack_order), as a JSON array, so the record survives
-- later stack edits or deletes. Reimage requires an image; apply forbids one.
CREATE TABLE IF NOT EXISTS `prov_runs` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `mode`          ENUM('reimage','apply') NOT NULL COMMENT 'reimage = wipe + install + stacks at first boot; apply = Ansible over SSH, no reinstall',
  `entity_id`     BIGINT UNSIGNED NOT NULL COMMENT 'FK: target machine',
  `image_id`      INT UNSIGNED NULL COMMENT 'FK: image installed (reimage only; NULL for apply)',
  `stack_slugs`   LONGTEXT NOT NULL COMMENT 'JSON array: ordered dependency-closed stack slugs actually run',
  `requested_by`  VARCHAR(128) NOT NULL COMMENT 'operator principal that pressed Deploy',
  `status`        ENUM('queued','running','succeeded','failed','cancelled') NOT NULL DEFAULT 'queued' COMMENT 'lifecycle; succeeded/failed/cancelled are terminal',
  `dry_run`       TINYINT(1) NOT NULL DEFAULT 0 COMMENT '1 = diff only, nothing changed on the target',
  `detail`        VARCHAR(255) NULL COMMENT 'short outcome / error summary',
  `requested_at`  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'when the run was requested',
  `started_at`    DATETIME NULL COMMENT 'when execution began',
  `finished_at`   DATETIME NULL COMMENT 'when the run reached a terminal status',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  PRIMARY KEY (`id`),
  KEY `ix_prov_runs_entity` (`entity_id`,`requested_at`),
  KEY `ix_prov_runs_image` (`image_id`),
  KEY `ix_prov_runs_status` (`status`),
  KEY `ix_prov_runs_source` (`source_id`),
  CONSTRAINT `ck_prov_runs_json`    CHECK (JSON_VALID(`stack_slugs`) AND JSON_TYPE(`stack_slugs`) = 'ARRAY'),
  CONSTRAINT `ck_prov_runs_image`   CHECK ((`mode` = 'reimage') = (`image_id` IS NOT NULL)),
  CONSTRAINT `ck_prov_runs_dry`     CHECK (`dry_run` IN (0,1)),
  CONSTRAINT `fk_prov_runs_entity`  FOREIGN KEY (`entity_id`) REFERENCES `entities`(`id`),
  CONSTRAINT `fk_prov_runs_image`   FOREIGN KEY (`image_id`)  REFERENCES `prov_images`(`id`),
  CONSTRAINT `fk_prov_runs_source`  FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Audit log of provisioning runs; request columns immutable, terminal status final, no deletes';

-- Request columns are immutable; terminal status is final; status never returns
-- to queued. <=> so NULL→value changes are caught too.
DELIMITER //
CREATE OR REPLACE TRIGGER `trg_prov_runs_bu`
BEFORE UPDATE ON `prov_runs` FOR EACH ROW
BEGIN
  IF NOT (OLD.mode <=> NEW.mode AND OLD.entity_id <=> NEW.entity_id
          AND OLD.image_id <=> NEW.image_id AND OLD.stack_slugs <=> NEW.stack_slugs
          AND OLD.requested_by <=> NEW.requested_by AND OLD.dry_run <=> NEW.dry_run
          AND OLD.requested_at <=> NEW.requested_at AND OLD.source_id <=> NEW.source_id
          AND OLD.created_at <=> NEW.created_at) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'prov_runs request columns are immutable';
  END IF;
  IF OLD.status IN ('succeeded','failed','cancelled') THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'prov_runs terminal status is final';
  END IF;
  IF NEW.status = 'queued' AND OLD.status <> 'queued' THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'prov_runs status cannot return to queued';
  END IF;
END//

CREATE OR REPLACE TRIGGER `trg_prov_runs_bd`
BEFORE DELETE ON `prov_runs` FOR EACH ROW
BEGIN
  SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'prov_runs is an audit log; rows are never deleted';
END//
DELIMITER ;

-- ----------------------------------------------------------------------------
-- 7. Seed stacks — core (locked, = today's group_vars/all.yml) + dev-tools
-- ----------------------------------------------------------------------------
-- Seed-once: NOT EXISTS over ANY row (live or soft-deleted) with the natural
-- key, so a re-run neither clobbers edits nor resurrects deletes. source_id is
-- the 021 'provisioner' row; a missing provisioner fails loudly (NOT NULL).
INSERT INTO `prov_stacks` (`slug`,`name`,`description`,`locked`,`sort_order`,`source_id`,`asserted_kind`)
SELECT t.slug, t.name, t.description, t.locked, t.sort_order,
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM (
  SELECT 'core' AS slug, 'Core' AS name,
         'Admin baseline on every host (was group_vars/all.yml solari_core_packages)' AS description,
         1 AS locked, 0 AS sort_order
  UNION ALL SELECT 'dev-tools', 'Dev tools',
         'Operator CLI toolset + zsh/oh-my-zsh/powerlevel10k/starship (dev-tools role)', 0, 100
) AS t
WHERE NOT EXISTS (SELECT 1 FROM `prov_stacks` s WHERE s.slug = t.slug);

-- core items: exactly solari_core_packages, names via solari_pkg_map (Debian /
-- Suse keys); a logical name absent from the map installs unchanged, as the
-- common-stack role does today. Fedora names only where confident; NULL
-- otherwise (rendering core for fedora is then a hard error naming the item —
-- intended, it surfaces the gap). sort_order preserves all.yml order.
INSERT INTO `prov_stack_items`
  (`stack_id`,`logical_name`,`item_type`,`name_suse`,`name_debian`,`name_fedora`,`sort_order`,`notes`,`source_id`,`asserted_kind`)
SELECT s.id, t.logical_name, 'pkg', t.name_suse, t.name_debian, t.name_fedora, t.sort_order, t.notes,
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM (
  SELECT 'sudo' AS logical_name, 'sudo' AS name_suse, 'sudo' AS name_debian, 'sudo' AS name_fedora, 10 AS sort_order, NULL AS notes
  UNION ALL SELECT 'curl',            'curl',            'curl',            'curl',            20,  NULL
  UNION ALL SELECT 'wget',            'wget',            'wget',            'wget',            30,  'Fedora 40+: provided by wget2-wget'
  UNION ALL SELECT 'ca-certificates', 'ca-certificates', 'ca-certificates', 'ca-certificates', 40,  NULL
  UNION ALL SELECT 'gnupg',           'gnupg',           'gnupg',           'gnupg2',          50,  'SUSE name as installed today (unmapped in all.yml)'
  UNION ALL SELECT 'htop',            'htop',            'htop',            'htop',            60,  NULL
  UNION ALL SELECT 'tmux',            'tmux',            'tmux',            'tmux',            70,  NULL
  UNION ALL SELECT 'git',             'git',             'git',             'git',             80,  NULL
  UNION ALL SELECT 'vim',             'vim',             'vim',             'vim-enhanced',    90,  NULL
  UNION ALL SELECT 'rsync',           'rsync',           'rsync',           'rsync',           100, NULL
  UNION ALL SELECT 'jq',              'jq',              'jq',              'jq',              110, NULL
  UNION ALL SELECT 'unzip',           'unzip',           'unzip',           'unzip',           120, NULL
  UNION ALL SELECT 'lsof',            'lsof',            'lsof',            'lsof',            130, NULL
  UNION ALL SELECT 'chrony',          'chrony',          'chrony',          'chrony',          140, NULL
  UNION ALL SELECT 'python3',         'python3',         'python3',         'python3',         150, NULL
  UNION ALL SELECT 'netcat',          'netcat-openbsd',  'netcat-openbsd',  NULL,              160, 'Fedora: netcat vs nmap-ncat unresolved; NULL until decided'
  UNION ALL SELECT 'iproute2',        'iproute2',        'iproute2',        'iproute',         170, NULL
  UNION ALL SELECT 'net-tools',       'net-tools',       'net-tools',       'net-tools',       180, NULL
  UNION ALL SELECT 'openssh-server',  'openssh',         'openssh-server',  'openssh-server',  190, 'openSUSE ships client+server in one openssh package'
) AS t
JOIN `prov_stacks` s ON s.slug = 'core' AND s.deleted_at IS NULL
WHERE NOT EXISTS (SELECT 1 FROM `prov_stack_items` i
                   WHERE i.stack_id = s.id AND i.logical_name = t.logical_name);

-- dev-tools: the role as one item (its package lists are deliberately
-- asymmetric and best-effort; not decomposed here). It branches
-- Debian-else-Suse, so Fedora is unsupported (NULL) until it learns RedHat.
INSERT INTO `prov_stack_items`
  (`stack_id`,`logical_name`,`item_type`,`name_suse`,`name_debian`,`name_fedora`,`ansible_role`,`sort_order`,`notes`,`source_id`,`asserted_kind`)
SELECT s.id, 'dev-tools role', 'role', '=', '=', NULL, 'dev-tools', 10,
       'zsh, byobu, oh-my-zsh, powerlevel10k (clone), starship, CLI toolset',
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM `prov_stacks` s
WHERE s.slug = 'dev-tools' AND s.deleted_at IS NULL
  AND NOT EXISTS (SELECT 1 FROM `prov_stack_items` i
                   WHERE i.stack_id = s.id AND i.logical_name = 'dev-tools role');

-- powerlevel10k pin: the role clones p10k at master with update:false (it never
-- moves an existing checkout). This inline script runs AFTER the role
-- (item_order 700) and moves that checkout to tag v1.20.0, refusing if the tag
-- no longer points at the commit observed via `git ls-remote` on 2026-10-31.
-- Guard: `git describe --exact-match` must print the pin, so a converged host
-- is a no-op and the role's update:false never fights it. Assumes the role's
-- default solari_devtools_ohmyzsh_dir (/usr/share/oh-my-zsh); fails loudly if
-- the clone is absent (solari_devtools_ohmyzsh: false).
INSERT INTO `prov_stack_items`
  (`stack_id`,`logical_name`,`item_type`,`name_suse`,`name_debian`,`name_fedora`,`source_url`,`version_pin`,
   `spec`,`item_order`,`sort_order`,`notes`,`source_id`,`asserted_kind`)
SELECT s.id, 'powerlevel10k pin', 'script', '=', '=', NULL,
       'https://github.com/romkatv/powerlevel10k', 'v1.20.0',
       JSON_OBJECT(
         'body', CONCAT_WS(CHAR(10),
           'set -eu',
           'd=/usr/share/oh-my-zsh/custom/themes/powerlevel10k',
           '[ -d "$d/.git" ] || { echo "powerlevel10k clone missing at $d (dev-tools ohmyzsh disabled?)" >&2; exit 1; }',
           'git -C "$d" fetch --quiet --depth 1 origin tag v1.20.0',
           '[ "$(git -C "$d" rev-parse ''v1.20.0^{commit}'')" = ff0311157d6b24fea21aa70699783f362b0f554f ] || { echo "powerlevel10k tag v1.20.0 moved upstream" >&2; exit 1; }',
           'git -C "$d" -c advice.detachedHead=false checkout --quiet ff0311157d6b24fea21aa70699783f362b0f554f'),
         'interpreter', '/bin/sh',
         'run_as', 'root',
         'check', 'git -C /usr/share/oh-my-zsh/custom/themes/powerlevel10k describe --tags --exact-match HEAD',
         'version_check', '^(v[0-9][0-9.]*)$'),
       700, 10, 'Pins the role''s master clone to a release tag (runs after the role)',
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM `prov_stacks` s
WHERE s.slug = 'dev-tools' AND s.deleted_at IS NULL
  AND NOT EXISTS (SELECT 1 FROM `prov_stack_items` i
                   WHERE i.stack_id = s.id AND i.logical_name = 'powerlevel10k pin');

INSERT INTO `prov_stack_deps` (`stack_id`,`depends_on_stack_id`,`source_id`,`asserted_kind`)
SELECT d.id, c.id, (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM `prov_stacks` d
JOIN `prov_stacks` c ON c.slug = 'core' AND c.deleted_at IS NULL
WHERE d.slug = 'dev-tools' AND d.deleted_at IS NULL
  AND NOT EXISTS (SELECT 1 FROM `prov_stack_deps` x
                   WHERE x.stack_id = d.id AND x.depends_on_stack_id = c.id);

-- ----------------------------------------------------------------------------
-- 8. Seed images — only what the provisioning tree really carries today
-- ----------------------------------------------------------------------------
-- Sources: deploy/fleet/fetch-images.sh (Debian 12 / Ubuntu 24.04 / Leap 15.6
-- x86_64 kernel+initrd), netboot/configs/opensuse.autoyast.xml.tmpl (written
-- for Leap AND Tumbleweed), pi/build-pi-image.sh + pi/pi5-netboot (Pi OS Lite
-- arm64/armhf). The boot.ipxe menu also lists arm64 Debian/Ubuntu/Leap, but
-- fetch-images.sh fetches no arm64 installer, so those rows are NOT seeded.
-- Tumbleweed x86_64 is not yet in fetch-images.sh; it is seeded because it is
-- the P6 target and the AutoYaST template supports it. version / sha256 /
-- fetched_at stay NULL: nothing has been fetched through this table yet.
INSERT INTO `prov_images`
  (`distro`,`family`,`release_name`,`release_label`,`arch`,`boot_method`,`source_url`,`checksum_url`,
   `install_template`,`refresh_policy`,`netboot_verified`,`notes`,`source_id`,`asserted_kind`)
SELECT t.distro, t.family, t.release_name, t.release_label, t.arch, t.boot_method, t.source_url, t.checksum_url,
       t.install_template, t.refresh_policy, 0, t.notes,
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM (
  SELECT 'opensuse' AS distro, 'suse' AS family, 'tumbleweed' AS release_name, 'Tumbleweed' AS release_label,
         'x86_64' AS arch, 'ipxe' AS boot_method,
         'https://download.opensuse.org/tumbleweed/repo/oss/boot/x86_64/loader/' AS source_url,
         NULL AS checksum_url, 'opensuse.autoyast.xml.tmpl' AS install_template, 'weekly' AS refresh_policy,
         'Rolling. Not yet fetched by fetch-images.sh (P3 IMAGE_REFRESH); P6 target (steel)' AS notes
  UNION ALL SELECT 'opensuse', 'suse', '15.6', 'Leap 15.6', 'x86_64', 'ipxe',
         'https://download.opensuse.org/distribution/leap/15.6/repo/oss/boot/x86_64/loader/',
         NULL, 'opensuse.autoyast.xml.tmpl', 'point_release', 'fetch-images.sh SUSE source'
  UNION ALL SELECT 'debian', 'debian', '12', '12 (bookworm)', 'x86_64', 'ipxe',
         'https://deb.debian.org/debian/dists/bookworm/main/installer-amd64/current/images/netboot/debian-installer/amd64/',
         'https://deb.debian.org/debian/dists/bookworm/main/installer-amd64/current/images/SHA256SUMS',
         'debian.preseed.tmpl', 'point_release', 'fetch-images.sh DEB source'
  UNION ALL SELECT 'ubuntu', 'debian', '24.04', '24.04 LTS (noble)', 'x86_64', 'ipxe',
         'https://releases.ubuntu.com/24.04/',
         'https://releases.ubuntu.com/24.04/SHA256SUMS',
         'ubuntu.autoinstall.tmpl', 'point_release', 'netboot tarball name resolved at fetch time (fetch-images.sh)'
  UNION ALL SELECT 'raspios', 'debian', 'bookworm', 'Pi OS Lite (bookworm)', 'arm64', 'pi_netboot',
         'https://downloads.raspberrypi.com/raspios_lite_arm64_latest',
         NULL, NULL, 'manual', 'Pi 5 network boot via pi/pi5-netboot; also buildable as media by pi/build-pi-image.sh'
  UNION ALL SELECT 'raspios', 'debian', 'bookworm', 'Pi OS Lite (bookworm)', 'armhf', 'media',
         'https://downloads.raspberrypi.com/raspios_lite_armhf_latest',
         NULL, NULL, 'manual', 'Card image via pi/build-pi-image.sh --arch arm32'
) AS t
WHERE NOT EXISTS (SELECT 1 FROM `prov_images` i
                   WHERE i.distro = t.distro AND i.release_name = t.release_name AND i.arch = t.arch);

-- ============================================================================
-- End 029_provisioning_images_stacks.sql
-- ============================================================================
