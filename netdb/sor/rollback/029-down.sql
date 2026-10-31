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
--     SHA2-256 over the byte-sorted, newline-joined rows, each row being
--     CONCAT_WS(',', QUOTE(col), ...) of every operator-editable column
--     (natural keys/slugs instead of auto-increment ids, the source SLUG
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
    `digest` CHAR(64) NULL
  );

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_stacks', COUNT(*), SHA2(GROUP_CONCAT(r ORDER BY r SEPARATOR '\n'), 256) FROM (
    SELECT CONVERT(CONCAT_WS(',', QUOTE(t.slug), QUOTE(t.name), QUOTE(t.description), QUOTE(t.locked),
             QUOTE(t.sort_order), QUOTE(src.slug), QUOTE(t.asserted_kind), QUOTE(t.deleted_at))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_stacks t LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_software', COUNT(*), SHA2(GROUP_CONCAT(r ORDER BY r SEPARATOR '\n'), 256) FROM (
    SELECT CONVERT(CONCAT_WS(',', QUOTE(t.logical_name), QUOTE(t.item_type), QUOTE(t.name_suse),
             QUOTE(t.name_debian), QUOTE(t.name_fedora), QUOTE(t.applies_families), QUOTE(t.applies_arches),
             QUOTE(t.ansible_role), QUOTE(t.source_url), QUOTE(t.version_pin), QUOTE(t.spec), QUOTE(t.notes),
             QUOTE(src.slug), QUOTE(t.asserted_kind), QUOTE(t.deleted_at))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_software t LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_stack_members', COUNT(*), SHA2(GROUP_CONCAT(r ORDER BY r SEPARATOR '\n'), 256) FROM (
    SELECT CONVERT(CONCAT_WS(',', QUOTE(s.slug), QUOTE(p.logical_name), QUOTE(t.item_order),
             QUOTE(t.sort_order), QUOTE(t.notes), QUOTE(src.slug), QUOTE(t.asserted_kind), QUOTE(t.deleted_at))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_stack_members t
    LEFT JOIN prov_stacks s ON s.id = t.stack_id
    LEFT JOIN prov_software p ON p.id = t.software_id
    LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_software_requires', COUNT(*), SHA2(GROUP_CONCAT(r ORDER BY r SEPARATOR '\n'), 256) FROM (
    SELECT CONVERT(CONCAT_WS(',', QUOTE(a.logical_name), QUOTE(b.logical_name), QUOTE(t.notes),
             QUOTE(src.slug), QUOTE(t.asserted_kind), QUOTE(t.deleted_at))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_software_requires t
    LEFT JOIN prov_software a ON a.id = t.software_id
    LEFT JOIN prov_software b ON b.id = t.requires_software_id
    LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_stack_deps', COUNT(*), SHA2(GROUP_CONCAT(r ORDER BY r SEPARATOR '\n'), 256) FROM (
    SELECT CONVERT(CONCAT_WS(',', QUOTE(a.slug), QUOTE(b.slug), QUOTE(src.slug), QUOTE(t.asserted_kind),
             QUOTE(t.deleted_at))
           USING utf8mb4) COLLATE utf8mb4_bin AS r
    FROM prov_stack_deps t
    LEFT JOIN prov_stacks a ON a.id = t.stack_id
    LEFT JOIN prov_stacks b ON b.id = t.depends_on_stack_id
    LEFT JOIN sources src ON src.id = t.source_id) x;

  INSERT INTO `_solarinet_m029_seed_actual`
  SELECT 'prov_images', COUNT(*), SHA2(GROUP_CONCAT(r ORDER BY r SEPARATOR '\n'), 256) FROM (
    SELECT CONVERT(CONCAT_WS(',', QUOTE(t.distro), QUOTE(t.family), QUOTE(t.release_name),
             QUOTE(t.release_label), QUOTE(t.arch), QUOTE(t.boot_method), QUOTE(t.source_url),
             QUOTE(t.checksum_url), QUOTE(t.version), QUOTE(t.sha256), QUOTE(t.fetched_at),
             QUOTE(t.install_template), QUOTE(t.refresh_policy), QUOTE(t.netboot_verified), QUOTE(t.notes),
             QUOTE(src.slug), QUOTE(t.asserted_kind), QUOTE(t.deleted_at))
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
  SELECT GROUP_CONCAT(CONCAT(e.tbl, ' (rows ', IFNULL(a.n, 0), ', seed ', e.n, ')') ORDER BY e.tbl SEPARATOR '; ')
    INTO bad
    FROM (
      -- expected: untouched 029 seed (see header to recompute)
      SELECT 'prov_images' AS tbl, 6 AS n, 'f520b99e8e978150cd936f350f486ff6000a3e54a3dbc15b0be053136967c793' AS digest
      UNION ALL SELECT 'prov_software', 21, '9fe9915d026c0cea695dfc0914e1e5dd7ad4e3a0a271c9354cc0960b60af90c1'
      UNION ALL SELECT 'prov_software_requires', 1, 'f1905dce8fcbe84201d0253b32d042a5f02c6356e098326907124b33e81c5203'
      UNION ALL SELECT 'prov_stack_deps', 1, '4b8df3c484f1b6acc03c0b93ad8c6e0ad07c76106ed6b02d1e824c668b50ac85'
      UNION ALL SELECT 'prov_stack_members', 21, 'cc609d6fc36714ab14971ee408ad8b06a9a78080fd3e3e51eeeb1e039e72d08c'
      UNION ALL SELECT 'prov_stacks', 2, 'fa9a51953d2bb2e0b629681835eb492bdf8387bea50b41776449ce6cf363b910'
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
