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
--   2. `prov_software` — the shared SOFTWARE POOL. Every installable thing is
--      defined here exactly once (one live row per logical_name): one of six
--      item types (pkg, repo, artifact, script, build, role), a per-family name
--      map, applicability filters, version_pin and a per-type JSON `spec`.
--      The pool IS the family name map.
--   3. `prov_software_artifacts` — per-arch download URL + sha256 for
--      `artifact` pool entries.
--   4. `prov_software_requires` — entry → entry requirement (pkg → repo,
--      build → its build-dependency pkgs, script → role, …). The one canonical
--      place for dependencies between pool entries.
--   5. `prov_software_conflicts` — symmetric "never install together" pairs,
--      stored once in canonical order (lo id < hi id).
--   6. `prov_stacks` — named, architecture-independent software stacks (Core,
--      Web, AI, …). `locked` marks a stack that is always included (Core).
--   7. `prov_stack_members` — stack → pool entry references, with an optional
--      explicit position. A stack carries no definitions of its own.
--   8. `prov_stack_deps` — stack → depends-on-stack edges (AI → Core).
--   9. `prov_runs` — audit log of every reimage / apply-stacks run.
--
-- The generator (deploy/fleet/stackgen.py) reads these tables and renders the
-- Ansible vars (`solari_stack_*`) for one target family + arch. It owns the
-- rules the schema cannot express: stack closure, cycle rejection (stacks and
-- requirements), Core first, install-once dedupe, requirement resolution
-- against the pool, conflict rejection, and hard errors for unmapped packages
-- or missing per-arch artifacts.
--
-- WHY A POOL (operator-approved 2026-10-31, before 029 went live; see
-- docs/Provisioning_UI_Design.md §3 item 5): an item used by two stacks used to
-- be defined twice and could drift (two names, two pins, two repos). Now a pin
-- is bumped once, a conflict is declared once, and an entry shared by two
-- selected stacks is installed once, at its first position in run order.
--
-- ITEM TYPES (prov_software.item_type; ENUM order is fixed, append-only)
--   pkg      distro-native package; real name per family. A third-party repo
--            it needs is a `requires` edge to that repo entry.
--   repo     third-party zypper/apt/dnf repo added before packages. spec =
--            per-family {url, key_url, fingerprint[, suite, components]}
--            (debian needs suite + components). These land in sources.list /
--            .repo files, so CHECK refuses anything that could add a line:
--            url/key_url are one token with no whitespace or control
--            characters, fingerprint has no control characters, suite and
--            each component match [A-Za-z0-9._/-]+ without a leading '-'.
--   artifact upstream release binary/tarball; `version_pin` required; per-arch
--            URL + sha256 in prov_software_artifacts (arch 'any' = all arches).
--   script   install script: spec.body (inline, stored here) XOR spec.url +
--            spec.sha256 (remote, pinned — an unpinned curl|sh is rejected by
--            CHECK). interpreter (default /bin/sh), run_as root|admin, env
--            (non-secret), idempotency guard creates and/or check (an argv
--            list, run without a shell, as the item's user) + optional
--            version_check regex compared with version_pin.
--   build    source build: spec.source {git[, tag]} checked out at the full
--            commit SHA in `version_pin` (tag is display only), or
--            {tarball, sha256}; system cmake|make|autotools|meson|custom;
--            configure_args, prefix, same guard (build checks run
--            unprivileged). Build dependencies are `requires` edges to pkg
--            entries (spec.build_deps is refused). custom builds carry TWO
--            commands, `build` and `install`; other systems may carry
--            neither. The legacy single `commands` key is refused. No build
--            command ever runs as root: every system's fetch, compile and
--            install run as the unprivileged admin user, the install staged
--            under DESTDIR, and root only copies the staged prefix into
--            place (data only). Root-time install logic must be a `script`.
--   role     existing Ansible role named by `ansible_role`.
--
-- REQUIREMENTS (prov_software_requires): A requires B = B is installed before
-- A. B is resolved against the POOL, not only the selected stacks, but must
-- apply to the target family/arch. If no selected stack lists B, the
-- generator installs it just before A and reports it as implicit. If a
-- selected stack lists B later in run order, a pkg is pulled forward; any
-- other type is a hard error (the operator's explicit order contradicts the
-- requirement). Self-requirement is rejected here; cycles by the generator.
--
-- CONFLICTS (prov_software_conflicts): one row per unordered pair, stored with
-- software_lo_id < software_hi_id so (a,b) and (b,a) cannot both exist. The
-- generator refuses a render whose installed set holds both sides, whether
-- each side came from a stack or was pulled in by a requirement.
--
-- SECRETS: spec env names that look secret-ish are refused by stackgen.py,
-- but that is a token-boundary HEURISTIC on the name only. Whatever a
-- variable is called, secret VALUES must never be stored in spec.
--
-- WHY A JSON `spec` (not side tables): the per-type fields are heterogeneous,
-- mostly 1:1 with the entry, and are edited as one form (P4). Six side tables
-- would add joins and rollback surface for no query that needs them. The
-- fields that ARE queried or safety-critical stay real columns (item_type,
-- family names, applicability, version_pin, ansible_role), relationships are
-- real FK'd tables (artifacts, requires, conflicts, members), the shape of
-- spec is CHECKed where it guards safety (script pinning, build source
-- present, spec present/absent per type), and deploy/fleet/stackgen.py
-- validates the rest strictly before anything renders.
--
-- PER-FAMILY NAME CONVENTION (name_suse / name_debian / name_fedora)
--   * pkg:              the real package name on that family. NULL = no
--                       package there (render is a hard error). '=' rejected.
--   * every other type: '=' = supported on that family. NULL = unsupported
--                       (hard error). Any other value is rejected by CHECK.
--   APPLICABILITY (applies_families / applies_arches, SET, NULL = all) is the
--   deliberate opt-out: an entry not applicable to the target is SKIPPED (and
--   reported), whereas an applicable entry with a NULL family name is an error.
--
-- ORDER within a stack (prov_stack_members): `item_order` when set, else the
--   entry's type default repo 100 → pkg 200 → artifact 300 → build 400 →
--   script 500 → role 600; ties by type, then member `sort_order`, then
--   logical_name. Ordering is a property of the membership, not the entry.
--
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
-- SEED: in this migration (sections 10–11). Seed-once semantics: each seed row
-- is inserted only when no row (live OR soft-deleted) with that natural key exists,
-- so a re-run never clobbers operator edits nor resurrects a deliberate delete.
-- The seed fills the pool and references it from stacks; `core` reproduces
-- group_vars/all.yml solari_core_packages through solari_pkg_map exactly; tests/deploy/test_stackgen.py and
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
  `family`           ENUM('suse','debian','fedora') NOT NULL COMMENT 'package family; selects prov_software.name_<family>',
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
  CONSTRAINT `ck_prov_images_sha256`   CHECK (`sha256` IS NULL OR `sha256` REGEXP '(?-i)^[0-9a-f]{64}\\z'),
  CONSTRAINT `ck_prov_images_pi_arch`  CHECK (`boot_method` <> 'pi_netboot' OR `arch` IN ('arm64','armhf')),
  CONSTRAINT `ck_prov_images_slug`     CHECK (`distro` REGEXP '(?-i)^[a-z0-9][a-z0-9-]*\\z'),
  CONSTRAINT `fk_prov_images_source`   FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Installable OS images: real distro x release x arch rows with boot method and fetch state';

-- ----------------------------------------------------------------------------
-- 2. prov_software — the shared software pool (six item types)
-- ----------------------------------------------------------------------------
-- One live row per logical_name: every stack that needs the entry references
-- it through prov_stack_members, so it has exactly one definition, one pin and
-- one name map. See the header for types, the name_<family> convention and
-- applicability. CHECKs make the safety-relevant rules declarative.
CREATE TABLE IF NOT EXISTS `prov_software` (
  `id`               INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `logical_name`     VARCHAR(96)  NOT NULL COMMENT 'arch/distro-independent entry name, e.g. netcat, docker-ce repo; unique among live rows',
  `item_type`        ENUM('pkg','repo','artifact','script','build','role') NOT NULL DEFAULT 'pkg' COMMENT 'pkg | repo | artifact | script | build | role (see migration header)',
  `name_suse`        VARCHAR(128) NULL COMMENT 'pkg: real openSUSE name; other types: = when supported; NULL = unsupported',
  `name_debian`      VARCHAR(128) NULL COMMENT 'pkg: real Debian/Ubuntu name; other types: = when supported; NULL = unsupported',
  `name_fedora`      VARCHAR(128) NULL COMMENT 'pkg: real Fedora name; other types: = when supported; NULL = unsupported',
  `applies_families` SET('suse','debian','fedora') NULL COMMENT 'families this entry applies to; NULL = all; non-applicable targets skip it',
  `applies_arches`   SET('x86_64','arm64','armhf','riscv64','i386') NULL COMMENT 'arches this entry applies to; NULL = all; non-applicable targets skip it',
  `ansible_role`     VARCHAR(64)  NULL COMMENT 'role items only: Ansible role under deploy/fleet/ansible/roles',
  `source_url`       VARCHAR(512) NULL COMMENT 'informational project/release home',
  `version_pin`      VARCHAR(64)  NULL COMMENT 'pinned version: one safe path component; git builds = full 40/64-hex commit SHA; required for artifact and build; compared by version_check',
  `spec`             LONGTEXT     NULL COMMENT 'per-type JSON object (repo/script/build required, artifact optional, pkg/role none); validated by stackgen.py',
  `notes`            VARCHAR(255) NULL COMMENT 'free-form operator note',
  `source_id`        SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`    ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`       DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`        TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_software_name_live` (`logical_name`,`live_flag`),
  KEY `ix_prov_software_type` (`item_type`),
  KEY `ix_prov_software_source` (`source_id`),
  CONSTRAINT `ck_prov_software_role`     CHECK ((`item_type` = 'role') = (`ansible_role` IS NOT NULL)),
  -- logical_name reaches task names, logs and repo descriptions: no control
  -- characters (C0, DEL, C1, U+2028/2029 line separators).
  CONSTRAINT `ck_prov_software_name`     CHECK (`logical_name` NOT REGEXP '[[:cntrl:]\\x{80}-\\x{9f}\\x{2028}\\x{2029}]'),
  CONSTRAINT `ck_prov_software_pin`      CHECK (`item_type` NOT IN ('artifact','build') OR `version_pin` IS NOT NULL),
  CONSTRAINT `ck_prov_software_suse`     CHECK (`name_suse`   IS NULL OR (`item_type` = 'pkg') = (`name_suse`   <> '=' AND `name_suse`   <> '')),
  CONSTRAINT `ck_prov_software_debian`   CHECK (`name_debian` IS NULL OR (`item_type` = 'pkg') = (`name_debian` <> '=' AND `name_debian` <> '')),
  CONSTRAINT `ck_prov_software_fedora`   CHECK (`name_fedora` IS NULL OR (`item_type` = 'pkg') = (`name_fedora` <> '=' AND `name_fedora` <> '')),
  CONSTRAINT `ck_prov_software_spec`     CHECK (`spec` IS NULL OR (JSON_VALID(`spec`) AND JSON_TYPE(`spec`) = 'OBJECT')),
  CONSTRAINT `ck_prov_software_spec_req` CHECK (CASE `item_type`
                                                     WHEN 'pkg'  THEN `spec` IS NULL
                                                     WHEN 'role' THEN `spec` IS NULL
                                                     WHEN 'artifact' THEN TRUE
                                                     ELSE `spec` IS NOT NULL END),
  -- version_pin becomes a directory name on hosts (artifact/build dirs): one
  -- safe path component, never '/', '.', '..' or a leading '-'.
  CONSTRAINT `ck_prov_software_pin_fmt`  CHECK (`version_pin` IS NULL OR `version_pin` REGEXP '(?-i)^[A-Za-z0-9][A-Za-z0-9._+~-]{0,63}\\z'),
  -- script: inline body XOR remote url; a remote url MUST carry a sha256.
  -- Every value is type- and emptiness-checked (JSON null is not a value).
  -- NB: MariaDB passes a CHECK that evaluates to NULL, and a missing JSON key
  -- yields NULL, so every JSON predicate below is wrapped in IFNULL(..., 0).
  CONSTRAINT `ck_prov_software_script`   CHECK (`item_type` <> 'script' OR IFNULL(
      (JSON_TYPE(JSON_EXTRACT(`spec`,'$.body')) = 'STRING' AND JSON_VALUE(`spec`,'$.body') <> ''
       AND NOT JSON_EXISTS(`spec`,'$.url') AND NOT JSON_EXISTS(`spec`,'$.sha256')) OR
      (JSON_TYPE(JSON_EXTRACT(`spec`,'$.url')) = 'STRING' AND JSON_VALUE(`spec`,'$.url') <> ''
       AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.sha256')) = 'STRING'
       AND JSON_VALUE(`spec`,'$.sha256') REGEXP '(?-i)^[0-9a-f]{64}\\z'
       AND NOT JSON_EXISTS(`spec`,'$.body')), 0)),
  -- build: source is {git} pinned by full commit SHA in version_pin, or
  -- {tarball, sha256}; system is one of the supported build systems, compared
  -- case-SENSITIVELY (the table collation would accept 'CMake', which the
  -- generator and role do not know). custom carries non-empty `build` and
  -- `install` commands; every other system carries neither; `commands` (the
  -- pre-split single field) is refused. Build dependencies are
  -- prov_software_requires edges, so a spec.build_deps list is refused too.
  CONSTRAINT `ck_prov_software_build`    CHECK (`item_type` <> 'build' OR IFNULL((
      JSON_TYPE(JSON_EXTRACT(`spec`,'$.source')) = 'OBJECT'
      AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.system')) = 'STRING'
      AND JSON_VALUE(`spec`,'$.system') COLLATE utf8mb4_bin IN ('cmake','make','autotools','meson','custom')
      AND NOT JSON_EXISTS(`spec`,'$.commands')
      AND NOT JSON_EXISTS(`spec`,'$.build_deps')
      AND (CASE WHEN JSON_VALUE(`spec`,'$.system') COLLATE utf8mb4_bin = 'custom'
             THEN JSON_TYPE(JSON_EXTRACT(`spec`,'$.build')) = 'STRING' AND JSON_VALUE(`spec`,'$.build') REGEXP '[^[:space:]]'
              AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.install')) = 'STRING' AND JSON_VALUE(`spec`,'$.install') REGEXP '[^[:space:]]'
             ELSE NOT JSON_EXISTS(`spec`,'$.build') AND NOT JSON_EXISTS(`spec`,'$.install') END)
      AND ((JSON_TYPE(JSON_EXTRACT(`spec`,'$.source.git')) = 'STRING' AND JSON_VALUE(`spec`,'$.source.git') <> ''
            AND NOT JSON_EXISTS(`spec`,'$.source.tarball')
            AND `version_pin` REGEXP '(?-i)^[0-9a-f]{40}([0-9a-f]{24})?\\z')
        OR (JSON_TYPE(JSON_EXTRACT(`spec`,'$.source.tarball')) = 'STRING' AND JSON_VALUE(`spec`,'$.source.tarball') <> ''
            AND NOT JSON_EXISTS(`spec`,'$.source.git')
            AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.source.sha256')) = 'STRING'
            AND JSON_VALUE(`spec`,'$.source.sha256') REGEXP '(?-i)^[0-9a-f]{64}\\z'))), 0)),
  -- guard check is an argv list (run without a shell), never a string: a
  -- non-empty array whose EVERY element is a string, the first non-empty.
  -- No JSON function can quantify over elements inside a CHECK (JSON_TABLE
  -- needs a subquery; JSON_SEARCH also matches numbers/booleans/null), so the
  -- compact text form is matched instead: '[' "str" (',' "str")* ']', where a
  -- JSON string is quote, (non-quote-non-backslash | backslash + any)*, quote.
  CONSTRAINT `ck_prov_software_check`    CHECK (`spec` IS NULL OR NOT JSON_EXISTS(`spec`,'$.check') OR IFNULL(
      (JSON_TYPE(JSON_EXTRACT(`spec`,'$.check')) = 'ARRAY'
       AND JSON_COMPACT(JSON_EXTRACT(`spec`,'$.check'))
           REGEXP '(?-i)^\\["(?:[^"\\\\]++|\\\\.)++"(?:,"(?:[^"\\\\]++|\\\\.)*+")*+\\]\\z'), 0)),
  -- repo: every family block present must be an object whose fields cannot
  -- inject a config line (see header); debian additionally needs suite and
  -- components. Same rules as stackgen._check_repo_family.
  CONSTRAINT `ck_prov_software_repo`     CHECK (`item_type` <> 'repo' OR IFNULL(
      (NOT JSON_EXISTS(`spec`,'$.suse') OR (JSON_TYPE(JSON_EXTRACT(`spec`,'$.suse')) = 'OBJECT'
        AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.suse.url')) = 'STRING' AND JSON_VALUE(`spec`,'$.suse.url') REGEXP '(?-i)^[^[:space:][:cntrl:]\\x{80}-\\x{9f}\\p{Z}]+\\z'
        AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.suse.key_url')) = 'STRING' AND JSON_VALUE(`spec`,'$.suse.key_url') REGEXP '(?-i)^[^[:space:][:cntrl:]\\x{80}-\\x{9f}\\p{Z}]+\\z'
        AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.suse.fingerprint')) = 'STRING' AND JSON_VALUE(`spec`,'$.suse.fingerprint') <> ''
        AND JSON_VALUE(`spec`,'$.suse.fingerprint') NOT REGEXP '[[:cntrl:]\\x{80}-\\x{9f}\\x{2028}\\x{2029}]'
        AND (NOT JSON_EXISTS(`spec`,'$.suse.suite') OR (JSON_TYPE(JSON_EXTRACT(`spec`,'$.suse.suite')) = 'STRING'
             AND JSON_VALUE(`spec`,'$.suse.suite') REGEXP '(?-i)^[A-Za-z0-9._/][A-Za-z0-9._/-]*\\z'))
        AND (NOT JSON_EXISTS(`spec`,'$.suse.components')
             OR JSON_COMPACT(JSON_EXTRACT(`spec`,'$.suse.components')) REGEXP '(?-i)^\\["(?:[A-Za-z0-9._/]|\\\\/)(?:[A-Za-z0-9._/-]|\\\\/)*+"(?:,"(?:[A-Za-z0-9._/]|\\\\/)(?:[A-Za-z0-9._/-]|\\\\/)*+")*+\\]\\z')))
      AND
      (NOT JSON_EXISTS(`spec`,'$.debian') OR (JSON_TYPE(JSON_EXTRACT(`spec`,'$.debian')) = 'OBJECT'
        AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.debian.url')) = 'STRING' AND JSON_VALUE(`spec`,'$.debian.url') REGEXP '(?-i)^[^[:space:][:cntrl:]\\x{80}-\\x{9f}\\p{Z}]+\\z'
        AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.debian.key_url')) = 'STRING' AND JSON_VALUE(`spec`,'$.debian.key_url') REGEXP '(?-i)^[^[:space:][:cntrl:]\\x{80}-\\x{9f}\\p{Z}]+\\z'
        AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.debian.fingerprint')) = 'STRING' AND JSON_VALUE(`spec`,'$.debian.fingerprint') <> ''
        AND JSON_VALUE(`spec`,'$.debian.fingerprint') NOT REGEXP '[[:cntrl:]\\x{80}-\\x{9f}\\x{2028}\\x{2029}]'
        AND (NOT JSON_EXISTS(`spec`,'$.debian.suite') OR (JSON_TYPE(JSON_EXTRACT(`spec`,'$.debian.suite')) = 'STRING'
             AND JSON_VALUE(`spec`,'$.debian.suite') REGEXP '(?-i)^[A-Za-z0-9._/][A-Za-z0-9._/-]*\\z'))
        AND (NOT JSON_EXISTS(`spec`,'$.debian.components')
             OR JSON_COMPACT(JSON_EXTRACT(`spec`,'$.debian.components')) REGEXP '(?-i)^\\["(?:[A-Za-z0-9._/]|\\\\/)(?:[A-Za-z0-9._/-]|\\\\/)*+"(?:,"(?:[A-Za-z0-9._/]|\\\\/)(?:[A-Za-z0-9._/-]|\\\\/)*+")*+\\]\\z') AND JSON_EXISTS(`spec`,'$.debian.suite') AND JSON_EXISTS(`spec`,'$.debian.components')))
      AND
      (NOT JSON_EXISTS(`spec`,'$.fedora') OR (JSON_TYPE(JSON_EXTRACT(`spec`,'$.fedora')) = 'OBJECT'
        AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.fedora.url')) = 'STRING' AND JSON_VALUE(`spec`,'$.fedora.url') REGEXP '(?-i)^[^[:space:][:cntrl:]\\x{80}-\\x{9f}\\p{Z}]+\\z'
        AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.fedora.key_url')) = 'STRING' AND JSON_VALUE(`spec`,'$.fedora.key_url') REGEXP '(?-i)^[^[:space:][:cntrl:]\\x{80}-\\x{9f}\\p{Z}]+\\z'
        AND JSON_TYPE(JSON_EXTRACT(`spec`,'$.fedora.fingerprint')) = 'STRING' AND JSON_VALUE(`spec`,'$.fedora.fingerprint') <> ''
        AND JSON_VALUE(`spec`,'$.fedora.fingerprint') NOT REGEXP '[[:cntrl:]\\x{80}-\\x{9f}\\x{2028}\\x{2029}]'
        AND (NOT JSON_EXISTS(`spec`,'$.fedora.suite') OR (JSON_TYPE(JSON_EXTRACT(`spec`,'$.fedora.suite')) = 'STRING'
             AND JSON_VALUE(`spec`,'$.fedora.suite') REGEXP '(?-i)^[A-Za-z0-9._/][A-Za-z0-9._/-]*\\z'))
        AND (NOT JSON_EXISTS(`spec`,'$.fedora.components')
             OR JSON_COMPACT(JSON_EXTRACT(`spec`,'$.fedora.components')) REGEXP '(?-i)^\\["(?:[A-Za-z0-9._/]|\\\\/)(?:[A-Za-z0-9._/-]|\\\\/)*+"(?:,"(?:[A-Za-z0-9._/]|\\\\/)(?:[A-Za-z0-9._/-]|\\\\/)*+")*+\\]\\z'))), 0)),
  CONSTRAINT `fk_prov_software_source`   FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Shared software pool: one definition per logical name (pkg/repo/artifact/script/build/role), per-family names, per-type JSON spec';

-- ----------------------------------------------------------------------------
-- 3. prov_software_artifacts — per-arch download for artifact entries
-- ----------------------------------------------------------------------------
-- arch 'any' = one artifact serves every arch (scripts, jars); an exact-arch
-- row wins over 'any'. Only meaningful for `artifact` entries — the generator
-- rejects artifact rows on any other item type. sha256 is mandatory and
-- format-checked: a row exists only once its digest is known (nothing is
-- fetched unpinned, and no digest is ever invented).
CREATE TABLE IF NOT EXISTS `prov_software_artifacts` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `software_id`   INT UNSIGNED NOT NULL COMMENT 'FK: the artifact pool entry',
  `arch`          ENUM('any','x86_64','arm64','armhf','riscv64','i386') NOT NULL COMMENT 'target arch; any = arch-independent',
  `url`           VARCHAR(512) NOT NULL COMMENT 'download URL for this arch at the entry''s version_pin',
  `sha256`        CHAR(64)     NOT NULL COMMENT 'expected sha256 of the download (64 lowercase hex); never invented, so no row until it is known',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`     TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_software_artifacts_live` (`software_id`,`arch`,`live_flag`),
  KEY `ix_prov_software_artifacts_source` (`source_id`),
  CONSTRAINT `ck_prov_software_artifacts_sha256` CHECK (`sha256` REGEXP '(?-i)^[0-9a-f]{64}\\z'),
  CONSTRAINT `ck_prov_software_artifacts_url`    CHECK (`url` <> ''),
  CONSTRAINT `fk_prov_software_artifacts_item`   FOREIGN KEY (`software_id`) REFERENCES `prov_software`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_prov_software_artifacts_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Per-arch download URL + sha256 for artifact pool entries (any = arch-independent)';

-- ----------------------------------------------------------------------------
-- 4. prov_software_requires — entry → entry requirement edges
-- ----------------------------------------------------------------------------
-- `software_id` requires `requires_software_id`: the requirement is installed
-- before the dependent (see the header for pool resolution, implicit installs
-- and pull-forward). This is the ONE canonical dependency mechanism: a pkg's
-- third-party repo and a build's build-dependency pkgs are both rows here (a
-- build's rendered `build_deps` are its pkg requirements). Rows carry ids, so
-- a requirement can never name an entry that does not exist, and a rename
-- cannot silently break it. Hard-deleting a REQUIRED entry is refused
-- (RESTRICT); hard-deleting the dependent drops its edges. Self-requirement is
-- rejected here; longer cycles by the generator.
CREATE TABLE IF NOT EXISTS `prov_software_requires` (
  `id`                   INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `software_id`          INT UNSIGNED NOT NULL COMMENT 'FK: the dependent pool entry',
  `requires_software_id` INT UNSIGNED NOT NULL COMMENT 'FK: the pool entry it requires (installed first)',
  `notes`                VARCHAR(255) NULL COMMENT 'why the requirement exists',
  `source_id`            SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`        ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`           DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`           DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`           DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`            TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_software_requires_live` (`software_id`,`requires_software_id`,`live_flag`),
  KEY `ix_prov_software_requires_on` (`requires_software_id`),
  KEY `ix_prov_software_requires_source` (`source_id`),
  CONSTRAINT `ck_prov_software_requires_self`   CHECK (`software_id` <> `requires_software_id`),
  CONSTRAINT `fk_prov_software_requires_sw`     FOREIGN KEY (`software_id`)          REFERENCES `prov_software`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_prov_software_requires_on`     FOREIGN KEY (`requires_software_id`) REFERENCES `prov_software`(`id`) ON DELETE RESTRICT,
  CONSTRAINT `fk_prov_software_requires_source` FOREIGN KEY (`source_id`)            REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Pool entry requirement edges (pkg->repo, build->build-dep pkg, ...); self rejected, cycles rejected by the generator';

-- ----------------------------------------------------------------------------
-- 5. prov_software_conflicts — symmetric "never together" pairs
-- ----------------------------------------------------------------------------
-- A conflict has no direction, so each pair is stored ONCE in canonical
-- order: software_lo_id < software_hi_id (which also rejects a self-conflict).
-- Together with the live-unique key this makes (a,b) and (b,a) the same row.
-- Writers order the ids (LEAST/GREATEST); the generator treats rows as
-- unordered pairs. Hard-deleting either entry drops the pair.
CREATE TABLE IF NOT EXISTS `prov_software_conflicts` (
  `id`             INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `software_lo_id` INT UNSIGNED NOT NULL COMMENT 'FK: the pool entry with the lower id',
  `software_hi_id` INT UNSIGNED NOT NULL COMMENT 'FK: the pool entry with the higher id',
  `notes`          VARCHAR(255) NULL COMMENT 'why the two cannot coexist',
  `source_id`      SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind`  ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`     DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`      TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_software_conflicts_live` (`software_lo_id`,`software_hi_id`,`live_flag`),
  KEY `ix_prov_software_conflicts_hi` (`software_hi_id`),
  KEY `ix_prov_software_conflicts_source` (`source_id`),
  CONSTRAINT `ck_prov_software_conflicts_self`      CHECK (`software_lo_id` <> `software_hi_id`),
  CONSTRAINT `ck_prov_software_conflicts_canonical` CHECK (`software_lo_id` < `software_hi_id`),
  CONSTRAINT `fk_prov_software_conflicts_lo`        FOREIGN KEY (`software_lo_id`) REFERENCES `prov_software`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_prov_software_conflicts_hi`        FOREIGN KEY (`software_hi_id`) REFERENCES `prov_software`(`id`) ON DELETE CASCADE,
  CONSTRAINT `fk_prov_software_conflicts_source`    FOREIGN KEY (`source_id`)      REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Unordered conflict pairs between pool entries, stored once with lo id < hi id';

-- ----------------------------------------------------------------------------
-- 6. prov_stacks — named architecture-independent software stacks
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
  CONSTRAINT `ck_prov_stacks_slug`   CHECK (`slug` REGEXP '(?-i)^[a-z0-9][a-z0-9-]*\\z'),
  CONSTRAINT `fk_prov_stacks_source` FOREIGN KEY (`source_id`) REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Named arch-independent software stacks; locked = always included (Core)';

-- ----------------------------------------------------------------------------
-- 7. prov_stack_members — stack → pool entry references
-- ----------------------------------------------------------------------------
-- A stack is an ordered list of references into the pool. Ordering belongs to
-- the membership (the same entry may sit at different positions in two
-- stacks): `item_order` when set, else the entry's type default; ties by
-- type, `sort_order`, logical_name. One live row per (stack, entry). Deleting
-- a stack drops its memberships; hard-deleting a pool entry that a stack still
-- references is refused (RESTRICT) — remove the membership first.
CREATE TABLE IF NOT EXISTS `prov_stack_members` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'surrogate PK',
  `stack_id`      INT UNSIGNED NOT NULL COMMENT 'FK: the stack',
  `software_id`   INT UNSIGNED NOT NULL COMMENT 'FK: the pool entry it includes',
  `item_order`    SMALLINT UNSIGNED NULL COMMENT 'explicit position within the stack; NULL = type default (repo 100 .. role 600)',
  `sort_order`    SMALLINT UNSIGNED NOT NULL DEFAULT 100 COMMENT 'tie-break within the same position (lower first, then logical_name)',
  `notes`         VARCHAR(255) NULL COMMENT 'free-form operator note',
  `source_id`     SMALLINT UNSIGNED NOT NULL COMMENT 'provenance: system that asserted this row',
  `asserted_kind` ENUM('human','machine') NOT NULL DEFAULT 'human' COMMENT 'provenance: human- or machine-asserted',
  `asserted_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'provenance: when the source last asserted this',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'row creation time',
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT 'last row modification',
  `deleted_at`    DATETIME NULL DEFAULT NULL COMMENT 'soft delete; NULL = live',
  `live_flag`     TINYINT AS (IF(`deleted_at` IS NULL, 1, NULL)) PERSISTENT COMMENT 'soft-delete-aware unique helper',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_prov_stack_members_live` (`stack_id`,`software_id`,`live_flag`),
  KEY `ix_prov_stack_members_software` (`software_id`),
  KEY `ix_prov_stack_members_source` (`source_id`),
  CONSTRAINT `fk_prov_stack_members_stack`    FOREIGN KEY (`stack_id`)    REFERENCES `prov_stacks`(`id`)   ON DELETE CASCADE,
  CONSTRAINT `fk_prov_stack_members_software` FOREIGN KEY (`software_id`) REFERENCES `prov_software`(`id`) ON DELETE RESTRICT,
  CONSTRAINT `fk_prov_stack_members_source`   FOREIGN KEY (`source_id`)   REFERENCES `sources`(`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Stack membership: references into the software pool with per-stack ordering';

-- ----------------------------------------------------------------------------
-- 8. prov_stack_deps — stack → depends-on-stack edges
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
-- 9. prov_runs — audit log of reimage / apply-stacks runs
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
-- 10. Seed pool + stacks — core (locked, = today's group_vars/all.yml) + dev-tools
-- ----------------------------------------------------------------------------
-- Seed-once: NOT EXISTS over ANY row (live or soft-deleted) with the natural
-- key, so a re-run neither clobbers edits nor resurrects deletes. source_id is
-- the 021 'provisioner' row; a missing provisioner fails loudly (NOT NULL).
-- Order: stacks, pool entries, memberships, requirement edges, stack deps.
-- No conflict pair is seeded: nothing in the seed genuinely conflicts (e.g.
-- chrony vs ntp would, but ntp is not a pool entry).
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

-- Core pool entries: exactly solari_core_packages, names via solari_pkg_map
-- (Debian / Suse keys); a logical name absent from the map installs
-- unchanged, as the common-stack role does today. Fedora names only where
-- confident; NULL otherwise (rendering core for fedora is then a hard error
-- naming the entry — intended, it surfaces the gap).
INSERT INTO `prov_software`
  (`logical_name`,`item_type`,`name_suse`,`name_debian`,`name_fedora`,`notes`,`source_id`,`asserted_kind`)
SELECT t.logical_name, 'pkg', t.name_suse, t.name_debian, t.name_fedora, t.notes,
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM (
  SELECT 'sudo' AS logical_name, 'sudo' AS name_suse, 'sudo' AS name_debian, 'sudo' AS name_fedora, NULL AS notes
  UNION ALL SELECT 'curl',            'curl',            'curl',            'curl',            NULL
  UNION ALL SELECT 'wget',            'wget',            'wget',            'wget',            'Fedora 40+: provided by wget2-wget'
  UNION ALL SELECT 'ca-certificates', 'ca-certificates', 'ca-certificates', 'ca-certificates', NULL
  UNION ALL SELECT 'gnupg',           'gnupg',           'gnupg',           'gnupg2',          'SUSE name as installed today (unmapped in all.yml)'
  UNION ALL SELECT 'htop',            'htop',            'htop',            'htop',            NULL
  UNION ALL SELECT 'tmux',            'tmux',            'tmux',            'tmux',            NULL
  UNION ALL SELECT 'git',             'git',             'git',             'git',             NULL
  UNION ALL SELECT 'vim',             'vim',             'vim',             'vim-enhanced',    NULL
  UNION ALL SELECT 'rsync',           'rsync',           'rsync',           'rsync',           NULL
  UNION ALL SELECT 'jq',              'jq',              'jq',              'jq',              NULL
  UNION ALL SELECT 'unzip',           'unzip',           'unzip',           'unzip',           NULL
  UNION ALL SELECT 'lsof',            'lsof',            'lsof',            'lsof',            NULL
  UNION ALL SELECT 'chrony',          'chrony',          'chrony',          'chrony',          NULL
  UNION ALL SELECT 'python3',         'python3',         'python3',         'python3',         NULL
  UNION ALL SELECT 'netcat',          'netcat-openbsd',  'netcat-openbsd',  NULL,              'Fedora: netcat vs nmap-ncat unresolved; NULL until decided'
  UNION ALL SELECT 'iproute2',        'iproute2',        'iproute2',        'iproute',         NULL
  UNION ALL SELECT 'net-tools',       'net-tools',       'net-tools',       'net-tools',       NULL
  UNION ALL SELECT 'openssh-server',  'openssh',         'openssh-server',  'openssh-server',  'openSUSE ships client+server in one openssh package'
) AS t
WHERE NOT EXISTS (SELECT 1 FROM `prov_software` p WHERE p.logical_name = t.logical_name);

-- dev-tools role as one pool entry (its package lists are deliberately
-- asymmetric and best-effort; not decomposed here). It branches
-- Debian-else-Suse, so Fedora is unsupported (NULL) until it learns RedHat.
INSERT INTO `prov_software`
  (`logical_name`,`item_type`,`name_suse`,`name_debian`,`name_fedora`,`ansible_role`,`notes`,`source_id`,`asserted_kind`)
SELECT 'dev-tools role', 'role', '=', '=', NULL, 'dev-tools',
       'zsh, byobu, oh-my-zsh, powerlevel10k (clone), starship, CLI toolset',
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM `prov_software` p WHERE p.logical_name = 'dev-tools role');

-- powerlevel10k pin: the role clones p10k at master with update:false (it never
-- moves an existing checkout). This inline script runs AFTER the role (its
-- dev-tools membership has item_order 700, and it `requires` the role) and
-- moves that checkout to the commit of tag v1.20.0 (observed via `git
-- ls-remote` on 2026-10-31). The pin is the full commit SHA; the tag is only
-- how the commit is fetched, and the script refuses if the fetched commit
-- differs. Guard: `git rev-parse HEAD` must print the pin, so a converged host
-- is a no-op and the role's update:false never fights it. Assumes the role's
-- default solari_devtools_ohmyzsh_dir (/usr/share/oh-my-zsh); fails loudly if
-- the clone is absent (solari_devtools_ohmyzsh: false).
INSERT INTO `prov_software`
  (`logical_name`,`item_type`,`name_suse`,`name_debian`,`name_fedora`,`source_url`,`version_pin`,
   `spec`,`notes`,`source_id`,`asserted_kind`)
SELECT 'powerlevel10k pin', 'script', '=', '=', NULL,
       'https://github.com/romkatv/powerlevel10k', 'ff0311157d6b24fea21aa70699783f362b0f554f',
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
         'check', JSON_ARRAY('git', '-C', '/usr/share/oh-my-zsh/custom/themes/powerlevel10k', 'rev-parse', 'HEAD'),
         'version_check', '^([0-9a-f]{40})$'),
       'Pins the dev-tools role''s master clone to the v1.20.0 commit (runs after the role)',
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM `prov_software` p WHERE p.logical_name = 'powerlevel10k pin');

-- Memberships. core: the 19 packages, sort_order preserving all.yml order.
-- dev-tools: the role, then the pin at item_order 700 (after every role).
INSERT INTO `prov_stack_members`
  (`stack_id`,`software_id`,`item_order`,`sort_order`,`source_id`,`asserted_kind`)
SELECT s.id, p.id, t.item_order, t.sort_order,
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM (
  SELECT 'core' AS slug, 'sudo' AS logical_name, NULL AS item_order, 10 AS sort_order
  UNION ALL SELECT 'core', 'curl',            NULL, 20
  UNION ALL SELECT 'core', 'wget',            NULL, 30
  UNION ALL SELECT 'core', 'ca-certificates', NULL, 40
  UNION ALL SELECT 'core', 'gnupg',           NULL, 50
  UNION ALL SELECT 'core', 'htop',            NULL, 60
  UNION ALL SELECT 'core', 'tmux',            NULL, 70
  UNION ALL SELECT 'core', 'git',             NULL, 80
  UNION ALL SELECT 'core', 'vim',             NULL, 90
  UNION ALL SELECT 'core', 'rsync',           NULL, 100
  UNION ALL SELECT 'core', 'jq',              NULL, 110
  UNION ALL SELECT 'core', 'unzip',           NULL, 120
  UNION ALL SELECT 'core', 'lsof',            NULL, 130
  UNION ALL SELECT 'core', 'chrony',          NULL, 140
  UNION ALL SELECT 'core', 'python3',         NULL, 150
  UNION ALL SELECT 'core', 'netcat',          NULL, 160
  UNION ALL SELECT 'core', 'iproute2',        NULL, 170
  UNION ALL SELECT 'core', 'net-tools',       NULL, 180
  UNION ALL SELECT 'core', 'openssh-server',  NULL, 190
  UNION ALL SELECT 'dev-tools', 'dev-tools role',    NULL, 10
  UNION ALL SELECT 'dev-tools', 'powerlevel10k pin', 700,  10
) AS t
JOIN `prov_stacks`   s ON s.slug = t.slug AND s.deleted_at IS NULL
JOIN `prov_software` p ON p.logical_name = t.logical_name AND p.deleted_at IS NULL
WHERE NOT EXISTS (SELECT 1 FROM `prov_stack_members` m
                   WHERE m.stack_id = s.id AND m.software_id = p.id);

-- The pin script fails without the role's clone: a real requirement.
INSERT INTO `prov_software_requires` (`software_id`,`requires_software_id`,`notes`,`source_id`,`asserted_kind`)
SELECT a.id, b.id, 'checks out the clone the role creates',
       (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM `prov_software` a
JOIN `prov_software` b ON b.logical_name = 'dev-tools role' AND b.deleted_at IS NULL
WHERE a.logical_name = 'powerlevel10k pin' AND a.deleted_at IS NULL
  AND NOT EXISTS (SELECT 1 FROM `prov_software_requires` r
                   WHERE r.software_id = a.id AND r.requires_software_id = b.id);

INSERT INTO `prov_stack_deps` (`stack_id`,`depends_on_stack_id`,`source_id`,`asserted_kind`)
SELECT d.id, c.id, (SELECT id FROM sources WHERE slug='provisioner' AND deleted_at IS NULL), 'human'
FROM `prov_stacks` d
JOIN `prov_stacks` c ON c.slug = 'core' AND c.deleted_at IS NULL
WHERE d.slug = 'dev-tools' AND d.deleted_at IS NULL
  AND NOT EXISTS (SELECT 1 FROM `prov_stack_deps` x
                   WHERE x.stack_id = d.id AND x.depends_on_stack_id = c.id);

-- ----------------------------------------------------------------------------
-- 11. Seed images — only what the provisioning tree really carries today
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
