-- Manual rollback for 029_provisioning_images_stacks.sql.
-- This file intentionally lives outside migrations/ so the forward-only
-- migration runner cannot discover or execute it.
--
-- Before use: stop anything writing prov_* (dashboard Provisioning API, P3+),
-- take and verify a fresh SoR backup. MariaDB DDL autocommits; this is not a
-- transactional rollback. Apply only on Cesium, never Benzene. Afterwards
-- remove the 029 row from the migration ledger (tools/migrate.py) by hand.
--
-- Preflight: refuses unless the prov_* tables hold EXACTLY the untouched 029
-- seed, because dropping the tables would destroy anything else:
--   * prov_runs, prov_software_artifacts and prov_software_conflicts must be
--     empty (029 seeds none of them);
--   * every seeded table must match its seed row count AND a content digest:
--     SHA2-256 over the newline-joined per-row SHA2-256 hashes (byte-sorted by
--     row), each row being CONCAT_WS(',', IFNULL(QUOTE(col),'NULL'), ...) of
--     every operator-editable column. QUOTE() output always starts with a
--     quote, so the bare NULL marker keeps NULLs and column positions
--     unambiguous (CONCAT_WS alone would skip NULLs). Hashing per row keeps
--     the aggregate at exactly 65*n-1 bytes; any other length means
--     group_concat_max_len truncated it and the rollback refuses (strict
--     sql_mode already errors with ER 1260; the length check covers a lax
--     session) (natural keys/slugs instead of auto-increment ids, the source SLUG
--     instead of source_id, deleted_at included; id, asserted_at, created_at
--     and updated_at excluded). Any added, removed, edited (even within the
--     same second as the seed) or soft-deleted row, or a row asserted by a
--     source other than 'provisioner', changes the digest.
--
-- The expected counts/digests below are constants computed from the 029 seed.
-- If 029's seed ever changes, recompute them on a SCRATCH database holding an
-- untouched seed (schema.sql + migrations/*.sql applied, nothing else; never a
-- live DB) and paste the result into the `expected` list:
--
--   { sed '/^-- RECOMPUTE STOP/q' netdb/sor/rollback/029-down.sql
--     echo 'CALL _solarinet_rollback_m029_seed_digests();'
--     echo 'SELECT tbl, n, digest FROM _solarinet_m029_seed_actual ORDER BY tbl;'
--     echo 'DROP PROCEDURE _solarinet_rollback_m029_seed_digests;'
--     echo 'DROP PROCEDURE _solarinet_rollback_m029_assert_seed_only;'
--   } | mariadb -N -B <scratch_db>
--
-- Everything above the RECOMPUTE STOP marker only (re)creates the two helper
-- procedures; it reads and drops nothing. tests/netdb/test_prov_images_stacks.sh
-- runs that command and requires it to reproduce the constants.
DROP PROCEDURE IF EXISTS `_solarinet_rollback_m029_seed_digests`;
DROP PROCEDURE IF EXISTS `_solarinet_rollback_m029_assert_seed_only`;
DELIMITER //
CREATE PROCEDURE `_solarinet_rollback_m029_seed_digests`()
BEGIN
  SET SESSION group_concat_max_len = 16777216;
  DROP TEMPORARY TABLE IF EXISTS `_solarinet_m029_seed_actual`;
  CREATE TEMPORARY TABLE `_solarinet_m029_seed_actual` (
    `tbl`    VARCHAR(64) NOT NULL PRIMARY KEY,
    `n`      BIGINT UNSIGNED NOT NULL,
    `digest` CHAR(64) NULL,
    `len`    BIGINT UNSIGNED NOT NULL
  );

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_stacks', COUNT(*), SHA2(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n'), 256),
         IFNULL(LENGTH(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n')), 0) FROM (
    SELECT CONVERT(CONCAT_WS(',', IFNULL(QUOTE(t.slug),'NULL'), IFNULL(QUOTE(t.name),'NULL'), IFNULL(QUOTE(t.description),'NULL'), IFNULL(QUOTE(t.locked),'NULL'),
             IFNULL(QUOTE(t.sort_order),'NULL'), IFNULL(QUOTE(src.slug),'NULL'), IFNULL(QUOTE(t.asserted_kind),'NULL'), IFNULL(QUOTE(t.deleted_at),'NULL'))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_stacks t LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_software', COUNT(*), SHA2(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n'), 256),
         IFNULL(LENGTH(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n')), 0) FROM (
    SELECT CONVERT(CONCAT_WS(',', IFNULL(QUOTE(t.logical_name),'NULL'), IFNULL(QUOTE(t.item_type),'NULL'), IFNULL(QUOTE(t.name_suse),'NULL'),
             IFNULL(QUOTE(t.name_debian),'NULL'), IFNULL(QUOTE(t.name_fedora),'NULL'), IFNULL(QUOTE(t.applies_families),'NULL'), IFNULL(QUOTE(t.applies_arches),'NULL'),
             IFNULL(QUOTE(t.ansible_role),'NULL'), IFNULL(QUOTE(t.source_url),'NULL'), IFNULL(QUOTE(t.version_pin),'NULL'), IFNULL(QUOTE(t.spec),'NULL'), IFNULL(QUOTE(t.notes),'NULL'),
             IFNULL(QUOTE(src.slug),'NULL'), IFNULL(QUOTE(t.asserted_kind),'NULL'), IFNULL(QUOTE(t.deleted_at),'NULL'))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_software t LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_stack_members', COUNT(*), SHA2(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n'), 256),
         IFNULL(LENGTH(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n')), 0) FROM (
    SELECT CONVERT(CONCAT_WS(',', IFNULL(QUOTE(s.slug),'NULL'), IFNULL(QUOTE(p.logical_name),'NULL'), IFNULL(QUOTE(t.item_order),'NULL'),
             IFNULL(QUOTE(t.sort_order),'NULL'), IFNULL(QUOTE(t.notes),'NULL'), IFNULL(QUOTE(src.slug),'NULL'), IFNULL(QUOTE(t.asserted_kind),'NULL'), IFNULL(QUOTE(t.deleted_at),'NULL'))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_stack_members t
    LEFT JOIN prov_stacks s ON s.id = t.stack_id
    LEFT JOIN prov_software p ON p.id = t.software_id
    LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_software_requires', COUNT(*), SHA2(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n'), 256),
         IFNULL(LENGTH(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n')), 0) FROM (
    SELECT CONVERT(CONCAT_WS(',', IFNULL(QUOTE(a.logical_name),'NULL'), IFNULL(QUOTE(b.logical_name),'NULL'), IFNULL(QUOTE(t.notes),'NULL'),
             IFNULL(QUOTE(src.slug),'NULL'), IFNULL(QUOTE(t.asserted_kind),'NULL'), IFNULL(QUOTE(t.deleted_at),'NULL'))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_software_requires t
    LEFT JOIN prov_software a ON a.id = t.software_id
    LEFT JOIN prov_software b ON b.id = t.requires_software_id
    LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_stack_deps', COUNT(*), SHA2(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n'), 256),
         IFNULL(LENGTH(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n')), 0) FROM (
    SELECT CONVERT(CONCAT_WS(',', IFNULL(QUOTE(a.slug),'NULL'), IFNULL(QUOTE(b.slug),'NULL'), IFNULL(QUOTE(src.slug),'NULL'), IFNULL(QUOTE(t.asserted_kind),'NULL'),
             IFNULL(QUOTE(t.deleted_at),'NULL'))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_stack_deps t
    LEFT JOIN prov_stacks a ON a.id = t.stack_id
    LEFT JOIN prov_stacks b ON b.id = t.depends_on_stack_id
    LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_images', COUNT(*), SHA2(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n'), 256),
         IFNULL(LENGTH(GROUP_CONCAT(SHA2(r, 256) ORDER BY r SEPARATOR '\n')), 0) FROM (
    SELECT CONVERT(CONCAT_WS(',', IFNULL(QUOTE(t.distro),'NULL'), IFNULL(QUOTE(t.family),'NULL'), IFNULL(QUOTE(t.release_name),'NULL'),
             IFNULL(QUOTE(t.release_label),'NULL'), IFNULL(QUOTE(t.arch),'NULL'), IFNULL(QUOTE(t.boot_method),'NULL'), IFNULL(QUOTE(t.source_url),'NULL'),
             IFNULL(QUOTE(t.checksum_url),'NULL'), IFNULL(QUOTE(t.version),'NULL'), IFNULL(QUOTE(t.sha256),'NULL'), IFNULL(QUOTE(t.fetched_at),'NULL'),
             IFNULL(QUOTE(t.install_template),'NULL'), IFNULL(QUOTE(t.refresh_policy),'NULL'), IFNULL(QUOTE(t.netboot_verified),'NULL'), IFNULL(QUOTE(t.notes),'NULL'),
             IFNULL(QUOTE(src.slug),'NULL'), IFNULL(QUOTE(t.asserted_kind),'NULL'), IFNULL(QUOTE(t.deleted_at),'NULL'))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_images t LEFT JOIN sources src ON src.id = t.source_id) x;
END//

CREATE PROCEDURE `_solarinet_rollback_m029_assert_seed_only`()
BEGIN
  DECLARE bad TEXT;

  IF EXISTS (SELECT 1 FROM prov_runs) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT = 'refusing rollback: prov_runs holds audit records';
  END IF;
  IF EXISTS (SELECT 1 FROM prov_software_artifacts) OR EXISTS (SELECT 1 FROM prov_software_conflicts) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT = 'refusing rollback: prov_software_artifacts/conflicts have rows (029 seeds none)';
  END IF;

  CALL `_solarinet_rollback_m029_seed_digests`();
  IF EXISTS (SELECT 1 FROM `_solarinet_m029_seed_actual` WHERE `len` <> IF(`n` = 0, 0, `n` * 65 - 1)) THEN
    SIGNAL SQLSTATE '45000'
      SET MESSAGE_TEXT = 'refusing rollback: seed digest input truncated (group_concat_max_len)';
  END IF;
  SELECT GROUP_CONCAT(CONCAT(e.tbl, ' (rows ', IFNULL(a.n, 0), ', seed ', e.n, ')') ORDER BY e.tbl SEPARATOR '; ')
    INTO bad
    FROM (
      -- expected: untouched 029 seed (see header to recompute)
      SELECT 'prov_images' AS tbl, 6 AS n, 'a4dd8721c419ae7806dbce56e4770b7e7d41201faee3d8ec93920f4623a7072f' AS digest
      UNION ALL SELECT 'prov_software', 21, '24ab351c3eeccff7a77e553e7955560ecd624050e77bceff67a4f2d2fce09ba6'
      UNION ALL SELECT 'prov_software_requires', 1, '1210eb61d21f48ed19035c467845fdb1a5da46c149af107f8271bec558f6f66d'
      UNION ALL SELECT 'prov_stack_deps', 1, '923b87a9ef29aaedc092426a578625ee3abd2eee25769b15ce634a682cbdcbd5'
      UNION ALL SELECT 'prov_stack_members', 21, '8b48fe928f1a8f6cd782bd5e2e959a071cc2eb37dfdb5a4f99ec323b975dac21'
      UNION ALL SELECT 'prov_stacks', 2, '65951ef969957ed49d6b0999ae380010d0464be3afce8ac0f5bd5801ffbbb070'
    ) e
    LEFT JOIN `_solarinet_m029_seed_actual` a ON a.tbl = e.tbl
   WHERE a.tbl IS NULL OR a.n <> e.n OR NOT (a.digest <=> e.digest);
  DROP TEMPORARY TABLE IF EXISTS `_solarinet_m029_seed_actual`;
  IF bad IS NOT NULL THEN
    SET bad = LEFT(CONCAT('refusing rollback: not the untouched 029 seed (row added, removed, edited, ',
                          'soft-deleted or asserted by another source) in: ', bad), 500);
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = bad;
  END IF;
END//
DELIMITER ;
-- RECOMPUTE STOP (nothing above this line reads or drops prov_* data)
CALL `_solarinet_rollback_m029_assert_seed_only`();
DROP PROCEDURE `_solarinet_rollback_m029_assert_seed_only`;
DROP PROCEDURE `_solarinet_rollback_m029_seed_digests`;

DROP TRIGGER IF EXISTS `trg_prov_runs_bd`;
DROP TRIGGER IF EXISTS `trg_prov_runs_bu`;
DROP TABLE IF EXISTS `prov_runs`;
DROP TABLE IF EXISTS `prov_stack_deps`;
DROP TABLE IF EXISTS `prov_stack_members`;
DROP TABLE IF EXISTS `prov_stacks`;
DROP TABLE IF EXISTS `prov_software_conflicts`;
DROP TABLE IF EXISTS `prov_software_requires`;
DROP TABLE IF EXISTS `prov_software_artifacts`;
DROP TABLE IF EXISTS `prov_software`;
DROP TABLE IF EXISTS `prov_images`;
