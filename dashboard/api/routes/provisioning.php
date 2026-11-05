<?php
declare(strict_types=1);

/**
 * Enrollments and builds read endpoints (§6):
 *   GET /api/enrollments?status=
 *   GET /api/builds
 *
 * Backed by: enrollment, buildArtifact, node, nodeConfig.
 * (The adopt/ignore/approve/reject/provision POSTs are added by the follow-up
 * mutation layer; they route through solariCtl. PHP never holds CA material.)
 *
 * NOTE: GET /api/discovery moved to routes/discovery.php, which enriches the
 * candidate list with mDNS services + LLDP neighbor cross-reference.
 *
 * PROVISIONING CATALOG (P3, docs/Provisioning_UI_Design.md) — SoR-backed via
 * Sor::db(), tables from migration 029. Field names in and out are the 029
 * column names (snake_case) so the P4 forms can round-trip a row into PUT.
 *   Reads (session):
 *     GET  /api/provisioning/images[/{id}]
 *     GET  /api/provisioning/software[/{id}]      (+ artifacts/requires/conflicts)
 *     GET  /api/provisioning/stacks[/{id}]        (+ ordered members, deps)
 *     GET  /api/provisioning/runs?entity=&mode=&status=&limit=   GET /runs/{id}
 *     GET  /api/provisioning/targets              (P4: entities with a MAC + arch)
 *     GET  /api/provisioning/log?kind=&id=        (P4: helper log tail, 64 KiB)
 *     GET  /api/provisioning/store                (NAS image store status, as
 *                                                  last published by prov-finish)
 *   Writes (operator; Origin + X-Solari-CSRF via Policy):
 *     POST/PUT/DELETE images, software, stacks (DELETE = soft delete)
 *     POST/DELETE software/{id}/artifacts|requires|conflicts
 *     POST/PUT/DELETE stacks/{id}/members, POST/DELETE stacks/{id}/deps
 *   Enqueue (operator) — SolariCtl::callQueued (REQUEST_SUBMIT), never call():
 *     POST /api/provisioning/image-refresh {image}                 -> IMAGE_REFRESH
 *     POST /api/provisioning/stage {entity, image, stacks[]}       -> PROV_STAGE
 *     POST /api/provisioning/apply {entity, stacks[], dry_run}     -> STACK_APPLY
 *   The operator principal is bound by callQueued (requestedBy); it is never
 *   placed in args. prov_runs is read-only here (an audit log; 029 triggers).
 */

require_once __DIR__ . '/../lib/ProvCatalog.php';

return static function (Router $router): void {

    // GET /api/enrollments?status=
    $router->get('/api/enrollments', static function (): void {
        $params = [];
        $where  = '';
        $status = $_GET['status'] ?? null;
        if (is_string($status) && $status !== '') {
            if (!in_array($status, ['token', 'pending', 'approved', 'rejected', 'expired'], true)) {
                Response::error('bad_request',
                    "status must be one of: token, pending, approved, rejected, expired", 400);
            }
            $where           = 'WHERE status = :status';
            $params[':status'] = $status;
        }

        $rows = Db::rows(
            "SELECT enrId, host, ip, role, certFp, status, requestedAt, decidedAt, decidedBy
               FROM enrollment
               $where
              ORDER BY requestedAt DESC",
            $params
        );

        // csrPem is intentionally never returned to the web tier.
        $out = array_map(static function (array $r): array {
            return [
                'enrId'       => Coerce::id($r['enrId']),
                'host'        => $r['host'],
                'ip'          => $r['ip'],
                'role'        => $r['role'],
                'certFp'      => $r['certFp'],
                'status'      => $r['status'],
                'requestedAt' => Coerce::iso($r['requestedAt']),
                'decidedAt'   => Coerce::iso($r['decidedAt']),
                'decidedBy'   => $r['decidedBy'],
            ];
        }, $rows);

        Response::ok($out);
    });

    // GET /api/builds — registry + per-arch convergence (nodes on each version).
    $router->get('/api/builds', static function (): void {
        $builds = Db::rows(
            'SELECT buildId, arch, os, version, channel, sha256, sizeBytes,
                    artifactUri, publishedAt
               FROM buildArtifact
              ORDER BY arch, os, publishedAt DESC'
        );

        // Convergence: count nodes whose applied build (configBlob->version, or
        // appliedEpoch presence) lands on a given (arch, version). The schema
        // tracks appliedEpoch in nodeConfig and arch on node; we report the raw
        // node-version distribution per arch so the adapter can flag updates.
        $nodeVers = Db::rows(
            "SELECT n.arch AS arch,
                    JSON_UNQUOTE(JSON_EXTRACT(c.configBlob, '$.version')) AS version,
                    COUNT(*) AS n
               FROM node n
               LEFT JOIN nodeConfig c ON c.nodeId = n.nodeId
              WHERE n.state <> 'retired'
              GROUP BY n.arch, version"
        );
        // index: arch -> version -> count
        $dist = [];
        foreach ($nodeVers as $r) {
            $arch = (string) ($r['arch'] ?? '');
            $ver  = $r['version'] === null ? '' : (string) $r['version'];
            $dist[$arch][$ver] = Coerce::int($r['n']);
        }

        $out = array_map(static function (array $b) use ($dist): array {
            $arch = (string) $b['arch'];
            $ver  = (string) $b['version'];
            return [
                'buildId'     => Coerce::id($b['buildId']),
                'arch'        => $arch,
                'os'          => $b['os'],
                'version'     => $ver,
                'channel'     => $b['channel'],
                'sha256'      => $b['sha256'],
                'sizeBytes'   => Coerce::int($b['sizeBytes']),
                'artifactUri' => $b['artifactUri'],
                'publishedAt' => Coerce::iso($b['publishedAt']),
                'nodesOnThisVersion' => $dist[$arch][$ver] ?? 0,
            ];
        }, $builds);

        Response::ok([
            'builds'           => $out,
            'nodeVersionDist'  => $dist,
        ]);
    });


    // =================================================================
    // Provisioning catalog (P3) — see the file header.
    // =================================================================
    $P = '/api/provisioning';
    $IMG_COLS = 'id, distro, family, release_name, release_label, arch, boot_method, source_url, checksum_url,
                 version, sha256, fetched_at, install_template, refresh_policy, netboot_verified, notes, updated_at';
    $SW_COLS  = 'id, logical_name, item_type, name_suse, name_debian, name_fedora, applies_families,
                 applies_arches, ansible_role, source_url, version_pin, spec, notes, updated_at';
    $ST_COLS  = 'id, slug, name, description, locked, sort_order, updated_at';

    $imageGet = static function (int $id, bool $lock = false) use ($IMG_COLS): ?array {
        return ProvCatalog::row("SELECT $IMG_COLS FROM prov_images WHERE id = :id AND deleted_at IS NULL"
            . ($lock ? ' FOR UPDATE' : ''), [':id' => $id]);
    };
    $softwareGet = static function (int $id, bool $lock = false) use ($SW_COLS): ?array {
        return ProvCatalog::row("SELECT $SW_COLS FROM prov_software WHERE id = :id AND deleted_at IS NULL"
            . ($lock ? ' FOR UPDATE' : ''), [':id' => $id]);
    };
    $stackGet = static function (int $id, bool $lock = false) use ($ST_COLS): ?array {
        return ProvCatalog::row("SELECT $ST_COLS FROM prov_stacks WHERE id = :id AND deleted_at IS NULL"
            . ($lock ? ' FOR UPDATE' : ''), [':id' => $id]);
    };

    /** Full software detail: row + artifacts, requires, required_by, conflicts, stacks. */
    $softwareDetail = static function (array $r): array {
        $id = (int) $r['id'];
        $out = ProvCatalog::softwareOut($r);
        $out['artifacts'] = array_map(static fn (array $a): array => [
            'id' => (int) $a['id'], 'arch' => $a['arch'], 'url' => $a['url'], 'sha256' => $a['sha256'],
        ], ProvCatalog::rows(
            'SELECT id, arch, url, sha256 FROM prov_software_artifacts
              WHERE software_id = :id AND deleted_at IS NULL ORDER BY arch, id', [':id' => $id]));
        $edge = static fn (array $e): array => [
            'software_id' => (int) $e['other_id'], 'logical_name' => $e['logical_name'],
            'item_type' => $e['item_type'], 'notes' => $e['notes'],
        ];
        $out['requires'] = array_map($edge, ProvCatalog::rows(
            'SELECT r.requires_software_id AS other_id, p.logical_name, p.item_type, r.notes
               FROM prov_software_requires r JOIN prov_software p ON p.id = r.requires_software_id
              WHERE r.software_id = :id AND r.deleted_at IS NULL AND p.deleted_at IS NULL
              ORDER BY p.logical_name', [':id' => $id]));
        $out['required_by'] = array_map($edge, ProvCatalog::rows(
            'SELECT r.software_id AS other_id, p.logical_name, p.item_type, r.notes
               FROM prov_software_requires r JOIN prov_software p ON p.id = r.software_id
              WHERE r.requires_software_id = :id AND r.deleted_at IS NULL AND p.deleted_at IS NULL
              ORDER BY p.logical_name', [':id' => $id]));
        $out['conflicts'] = array_map($edge, ProvCatalog::rows(
            'SELECT p.id AS other_id, p.logical_name, p.item_type, c.notes
               FROM prov_software_conflicts c
               JOIN prov_software p ON p.id = IF(c.software_lo_id = :a, c.software_hi_id, c.software_lo_id)
              WHERE (c.software_lo_id = :b OR c.software_hi_id = :c)
                AND c.deleted_at IS NULL AND p.deleted_at IS NULL
              ORDER BY p.logical_name', [':a' => $id, ':b' => $id, ':c' => $id]));
        $out['stacks'] = array_map(static fn (array $s): array => [
            'id' => (int) $s['id'], 'slug' => $s['slug'],
        ], ProvCatalog::rows(
            'SELECT s.id, s.slug FROM prov_stack_members m JOIN prov_stacks s ON s.id = m.stack_id
              WHERE m.software_id = :id AND m.deleted_at IS NULL AND s.deleted_at IS NULL
              ORDER BY s.slug', [':id' => $id]));
        return $out;
    };

    /** Full stack detail: row + members in run order + deps + dependents. */
    $stackDetail = static function (array $r): array {
        $id = (int) $r['id'];
        $out = ProvCatalog::stackOut($r);
        $rank = ProvCatalog::TYPE_RANK_SQL;
        $out['members'] = array_map(static fn (array $m): array => [
            'software_id'     => (int) $m['software_id'],
            'logical_name'    => $m['logical_name'],
            'item_type'       => $m['item_type'],
            'item_order'      => $m['item_order'] === null ? null : (int) $m['item_order'],
            'effective_order' => (int) $m['effective_order'],
            'sort_order'      => (int) $m['sort_order'],
            'notes'           => $m['notes'],
        ], ProvCatalog::rows(
            "SELECT m.software_id, p.logical_name, p.item_type, m.item_order, m.sort_order, m.notes,
                    COALESCE(m.item_order, $rank * 100) AS effective_order
               FROM prov_stack_members m JOIN prov_software p ON p.id = m.software_id
              WHERE m.stack_id = :id AND m.deleted_at IS NULL AND p.deleted_at IS NULL
              ORDER BY effective_order, $rank, m.sort_order, p.logical_name", [':id' => $id]));
        $dep = static fn (array $d): array => ['stack_id' => (int) $d['id'], 'slug' => $d['slug']];
        $out['depends_on'] = array_map($dep, ProvCatalog::rows(
            'SELECT s.id, s.slug FROM prov_stack_deps d JOIN prov_stacks s ON s.id = d.depends_on_stack_id
              WHERE d.stack_id = :id AND d.deleted_at IS NULL AND s.deleted_at IS NULL ORDER BY s.slug',
            [':id' => $id]));
        $out['depended_on_by'] = array_map($dep, ProvCatalog::rows(
            'SELECT s.id, s.slug FROM prov_stack_deps d JOIN prov_stacks s ON s.id = d.stack_id
              WHERE d.depends_on_stack_id = :id AND d.deleted_at IS NULL AND s.deleted_at IS NULL ORDER BY s.slug',
            [':id' => $id]));
        return $out;
    };

    // ------------------------------------------------------------- images

    $router->get("$P/images", static function () use ($IMG_COLS): void {
        $rows = ProvCatalog::rows("SELECT $IMG_COLS FROM prov_images WHERE deleted_at IS NULL
                                    ORDER BY distro, release_name, arch, boot_method, id");
        Response::ok(array_map([ProvCatalog::class, 'imageOut'], $rows));
    });

    $router->get("$P/images/{id}", static function (array $p) use ($imageGet): void {
        $r = $imageGet(ProvCatalog::pathId($p['id']));
        if ($r === null) {
            ProvCatalog::notFound('Image not found.');
        }
        Response::ok(ProvCatalog::imageOut($r));
    });

    $router->post("$P/images", static function () use ($imageGet): void {
        Operator::requireOperator();
        $b = ProvCatalog::body(array_merge(ProvCatalog::IMAGE_FIELDS, ProvCatalog::IMAGE_MANAGED));
        $set = ProvCatalog::imageFields($b, null);
        $id = ProvCatalog::insertRow('prov_images', $set);
        Response::ok(ProvCatalog::imageOut($imageGet($id) ?? ProvCatalog::fail('db_error', 'Created row not readable.', 500)), 201);
    });

    $router->add('PUT', "$P/images/{id}", static function (array $p) use ($imageGet): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $b = ProvCatalog::body(array_merge(ProvCatalog::IMAGE_FIELDS, ProvCatalog::IMAGE_MANAGED));
        // Shape-only pre-check (no DB): refresh-owned fields and empty bodies
        // fail before the row lock. imageFields() re-checks with the row.
        foreach (ProvCatalog::IMAGE_MANAGED as $k) {
            if (array_key_exists($k, $b)) {
                ProvCatalog::bad("$k is managed by image refresh and cannot be set directly.");
            }
        }
        if ($b === []) {
            ProvCatalog::bad('Nothing to update.');
        }
        ProvCatalog::tx(static function () use ($id, $b, $imageGet): void {
            $cur = $imageGet($id, true);
            if ($cur === null) {
                ProvCatalog::notFound('Image not found.');
            }
            ProvCatalog::updateRow('prov_images', $id, ProvCatalog::imageFields($b, $cur));
        });
        Response::ok(ProvCatalog::imageOut($imageGet($id) ?? ProvCatalog::notFound('Image not found.')));
    });

    $router->add('DELETE', "$P/images/{id}", static function (array $p): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $n = ProvCatalog::exec(
            "UPDATE prov_images SET deleted_at = NOW(), source_id = :src, asserted_kind = 'human', asserted_at = NOW()
              WHERE id = :id AND deleted_at IS NULL", ProvCatalog::prov() + [':id' => $id]);
        if ($n === 0) {
            ProvCatalog::notFound('Image not found.');
        }
        Response::ok(['id' => $id, 'deleted' => true]);
    });

    // ----------------------------------------------------------- software

    $router->get("$P/software", static function () use ($SW_COLS): void {
        $where = 'deleted_at IS NULL';
        $params = [];
        $type = $_GET['type'] ?? null;
        if ($type !== null && $type !== '') {
            $params[':t'] = ProvCatalog::enum($type, 'type', ProvCatalog::ITEM_TYPES);
            $where .= ' AND item_type = :t';
        }
        $rows = ProvCatalog::rows("SELECT $SW_COLS FROM prov_software WHERE $where ORDER BY logical_name, id", $params);
        // Live conflict pairs ride along so the deploy view can block a
        // conflicting selection before confirm (the generator refuses it anyway).
        $cf = [];
        foreach (ProvCatalog::rows('SELECT c.software_lo_id AS lo, c.software_hi_id AS hi
                                      FROM prov_software_conflicts c
                                      JOIN prov_software a ON a.id = c.software_lo_id AND a.deleted_at IS NULL
                                      JOIN prov_software b ON b.id = c.software_hi_id AND b.deleted_at IS NULL
                                     WHERE c.deleted_at IS NULL') as $c) {
            $cf[(int) $c['lo']][] = (int) $c['hi'];
            $cf[(int) $c['hi']][] = (int) $c['lo'];
        }
        Response::ok(array_map(static function (array $r) use ($cf): array {
            $o = ProvCatalog::softwareOut($r);
            $o['conflicts_with'] = $cf[(int) $r['id']] ?? [];
            return $o;
        }, $rows));
    });

    $router->get("$P/software/{id}", static function (array $p) use ($softwareGet, $softwareDetail): void {
        $r = $softwareGet(ProvCatalog::pathId($p['id']));
        if ($r === null) {
            ProvCatalog::notFound('Software entry not found.');
        }
        Response::ok($softwareDetail($r));
    });

    $router->post("$P/software", static function () use ($softwareGet, $softwareDetail): void {
        Operator::requireOperator();
        $b = ProvCatalog::body(ProvCatalog::SOFTWARE_FIELDS);
        $set = ProvCatalog::softwareFields($b, null);
        $id = ProvCatalog::insertRow('prov_software', $set);
        Response::ok($softwareDetail($softwareGet($id) ?? ProvCatalog::fail('db_error', 'Created row not readable.', 500)), 201);
    });

    $router->add('PUT', "$P/software/{id}", static function (array $p) use ($softwareGet, $softwareDetail): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $b = ProvCatalog::body(ProvCatalog::SOFTWARE_FIELDS);
        ProvCatalog::tx(static function () use ($id, $b, $softwareGet): void {
            $cur = $softwareGet($id, true);
            if ($cur === null) {
                ProvCatalog::notFound('Software entry not found.');
            }
            $arts = (int) ProvCatalog::scalar(
                'SELECT COUNT(*) AS n FROM prov_software_artifacts WHERE software_id = :id AND deleted_at IS NULL',
                [':id' => $id]);
            ProvCatalog::updateRow('prov_software', $id, ProvCatalog::softwareFields($b, $cur, $arts > 0));
        });
        Response::ok($softwareDetail($softwareGet($id) ?? ProvCatalog::notFound('Software entry not found.')));
    });

    // Soft delete. Mirrors the RESTRICT FKs on live rows: refused while a live
    // stack lists it or a live entry requires it. Its own artifacts, outgoing
    // requires and conflict pairs are soft-deleted with it (029 CASCADE).
    $router->add('DELETE', "$P/software/{id}", static function (array $p) use ($softwareGet): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        ProvCatalog::tx(static function () use ($id, $softwareGet): void {
            if ($softwareGet($id, true) === null) {
                ProvCatalog::notFound('Software entry not found.');
            }
            $stacks = ProvCatalog::rows(
                'SELECT s.slug FROM prov_stack_members m JOIN prov_stacks s ON s.id = m.stack_id
                  WHERE m.software_id = :id AND m.deleted_at IS NULL AND s.deleted_at IS NULL ORDER BY s.slug',
                [':id' => $id]);
            if ($stacks !== []) {
                ProvCatalog::conflict('Still a member of stack(s): '
                    . implode(', ', array_column($stacks, 'slug')) . '. Remove the membership first.');
            }
            $req = ProvCatalog::rows(
                'SELECT p.logical_name FROM prov_software_requires r JOIN prov_software p ON p.id = r.software_id
                  WHERE r.requires_software_id = :id AND r.deleted_at IS NULL AND p.deleted_at IS NULL
                  ORDER BY p.logical_name', [':id' => $id]);
            if ($req !== []) {
                ProvCatalog::conflict('Still required by: '
                    . implode(', ', array_map([ProvCatalog::class, 'clip'], array_column($req, 'logical_name')))
                    . '. Remove the requirement first.');
            }
            $src = ProvCatalog::prov();
            ProvCatalog::exec("UPDATE prov_software SET deleted_at = NOW(), source_id = :src, asserted_kind = 'human',
                                      asserted_at = NOW() WHERE id = :id AND deleted_at IS NULL", $src + [':id' => $id]);
            ProvCatalog::exec('UPDATE prov_software_artifacts SET deleted_at = NOW()
                                WHERE software_id = :id AND deleted_at IS NULL', [':id' => $id]);
            ProvCatalog::exec('UPDATE prov_software_requires SET deleted_at = NOW()
                                WHERE software_id = :id AND deleted_at IS NULL', [':id' => $id]);
            ProvCatalog::exec('UPDATE prov_software_conflicts SET deleted_at = NOW()
                                WHERE (software_lo_id = :lo OR software_hi_id = :hi) AND deleted_at IS NULL',
                [':lo' => $id, ':hi' => $id]);
        });
        Response::ok(['id' => $id, 'deleted' => true]);
    });

    // artifacts ---------------------------------------------------------
    $router->post("$P/software/{id}/artifacts", static function (array $p) use ($softwareGet, $softwareDetail): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $b = ProvCatalog::body(['arch', 'url', 'sha256'], 16384);
        $arch = ProvCatalog::enum(ProvCatalog::requireKey($b, 'arch'), 'arch', ProvCatalog::ART_ARCHES);
        $url = ProvCatalog::url(ProvCatalog::requireKey($b, 'url'), 'url', false);
        $sha = ProvCatalog::token(ProvCatalog::requireKey($b, 'sha256'), 'sha256', ProvCatalog::SHA256_RE,
            '64 lowercase hex characters', false);
        ProvCatalog::tx(static function () use ($id, $arch, $url, $sha, $softwareGet): void {
            $sw = $softwareGet($id, true);
            if ($sw === null) {
                ProvCatalog::notFound('Software entry not found.');
            }
            if ($sw['item_type'] !== 'artifact') {
                ProvCatalog::bad('Artifacts can only be added to an artifact entry.');
            }
            ProvCatalog::insertRow('prov_software_artifacts',
                ['software_id' => $id, 'arch' => $arch, 'url' => $url, 'sha256' => $sha]);
        });
        Response::ok($softwareDetail($softwareGet($id) ?? ProvCatalog::notFound('Software entry not found.')), 201);
    });

    $router->add('DELETE', "$P/software/{id}/artifacts/{artifactId}", static function (array $p): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $aid = ProvCatalog::pathId($p['artifactId'], 'artifactId');
        $n = ProvCatalog::exec('UPDATE prov_software_artifacts SET deleted_at = NOW()
                                 WHERE id = :aid AND software_id = :id AND deleted_at IS NULL',
            [':aid' => $aid, ':id' => $id]);
        if ($n === 0) {
            ProvCatalog::notFound('Artifact not found.');
        }
        Response::ok(['id' => $aid, 'deleted' => true]);
    });

    // requires ----------------------------------------------------------
    $router->post("$P/software/{id}/requires", static function (array $p) use ($softwareGet, $softwareDetail): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $b = ProvCatalog::body(['requires_software_id', 'notes'], 16384);
        $req = ProvCatalog::idValue(ProvCatalog::requireKey($b, 'requires_software_id'), 'requires_software_id');
        $notes = ProvCatalog::text($b['notes'] ?? null, 'notes', 255);
        if ($req === $id) {
            ProvCatalog::bad('An entry cannot require itself.');
        }
        ProvCatalog::tx(static function () use ($id, $req, $notes, $softwareGet): void {
            if ($softwareGet($id, true) === null || $softwareGet($req, true) === null) {
                ProvCatalog::notFound('Software entry not found.');
            }
            ProvCatalog::insertRow('prov_software_requires',
                ['software_id' => $id, 'requires_software_id' => $req, 'notes' => $notes]);
        });
        Response::ok($softwareDetail($softwareGet($id) ?? ProvCatalog::notFound('Software entry not found.')), 201);
    });

    $router->add('DELETE', "$P/software/{id}/requires/{requiresId}", static function (array $p): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $req = ProvCatalog::pathId($p['requiresId'], 'requiresId');
        $n = ProvCatalog::exec('UPDATE prov_software_requires SET deleted_at = NOW()
                                 WHERE software_id = :id AND requires_software_id = :req AND deleted_at IS NULL',
            [':id' => $id, ':req' => $req]);
        if ($n === 0) {
            ProvCatalog::notFound('Requirement not found.');
        }
        Response::ok(['software_id' => $id, 'requires_software_id' => $req, 'deleted' => true]);
    });

    // conflicts (stored canonically lo < hi) ------------------------------
    $router->post("$P/software/{id}/conflicts", static function (array $p) use ($softwareGet, $softwareDetail): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $b = ProvCatalog::body(['software_id', 'notes'], 16384);
        $other = ProvCatalog::idValue(ProvCatalog::requireKey($b, 'software_id'), 'software_id');
        $notes = ProvCatalog::text($b['notes'] ?? null, 'notes', 255);
        if ($other === $id) {
            ProvCatalog::bad('An entry cannot conflict with itself.');
        }
        ProvCatalog::tx(static function () use ($id, $other, $notes, $softwareGet): void {
            if ($softwareGet($id, true) === null || $softwareGet($other, true) === null) {
                ProvCatalog::notFound('Software entry not found.');
            }
            ProvCatalog::insertRow('prov_software_conflicts', [
                'software_lo_id' => min($id, $other), 'software_hi_id' => max($id, $other), 'notes' => $notes,
            ]);
        });
        Response::ok($softwareDetail($softwareGet($id) ?? ProvCatalog::notFound('Software entry not found.')), 201);
    });

    $router->add('DELETE', "$P/software/{id}/conflicts/{otherId}", static function (array $p): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $other = ProvCatalog::pathId($p['otherId'], 'otherId');
        $n = ProvCatalog::exec('UPDATE prov_software_conflicts SET deleted_at = NOW()
                                 WHERE software_lo_id = :lo AND software_hi_id = :hi AND deleted_at IS NULL',
            [':lo' => min($id, $other), ':hi' => max($id, $other)]);
        if ($n === 0) {
            ProvCatalog::notFound('Conflict not found.');
        }
        Response::ok(['software_id' => $id, 'other_id' => $other, 'deleted' => true]);
    });

    // ------------------------------------------------------------- stacks

    $router->get("$P/stacks", static function () use ($ST_COLS): void {
        $rows = ProvCatalog::rows(
            "SELECT $ST_COLS,
                    (SELECT COUNT(*) FROM prov_stack_members m JOIN prov_software p ON p.id = m.software_id
                      WHERE m.stack_id = s.id AND m.deleted_at IS NULL AND p.deleted_at IS NULL) AS member_count
               FROM prov_stacks s WHERE s.deleted_at IS NULL ORDER BY s.locked DESC, s.sort_order, s.slug");
        $deps = ProvCatalog::rows(
            'SELECT d.stack_id, t.slug FROM prov_stack_deps d JOIN prov_stacks t ON t.id = d.depends_on_stack_id
              WHERE d.deleted_at IS NULL AND t.deleted_at IS NULL ORDER BY t.slug');
        $byStack = [];
        foreach ($deps as $d) {
            $byStack[(int) $d['stack_id']][] = $d['slug'];
        }
        Response::ok(array_map(static function (array $r) use ($byStack): array {
            $o = ProvCatalog::stackOut($r);
            $o['member_count'] = (int) $r['member_count'];
            $o['depends_on'] = $byStack[(int) $r['id']] ?? [];
            return $o;
        }, $rows));
    });

    $router->get("$P/stacks/{id}", static function (array $p) use ($stackGet, $stackDetail): void {
        $r = $stackGet(ProvCatalog::pathId($p['id']));
        if ($r === null) {
            ProvCatalog::notFound('Stack not found.');
        }
        Response::ok($stackDetail($r));
    });

    $router->post("$P/stacks", static function () use ($stackGet, $stackDetail): void {
        Operator::requireOperator();
        $b = ProvCatalog::body(ProvCatalog::STACK_FIELDS, 16384);
        $id = ProvCatalog::insertRow('prov_stacks', ProvCatalog::stackFields($b, true));
        Response::ok($stackDetail($stackGet($id) ?? ProvCatalog::fail('db_error', 'Created row not readable.', 500)), 201);
    });

    $router->add('PUT', "$P/stacks/{id}", static function (array $p) use ($stackGet, $stackDetail): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $b = ProvCatalog::body(ProvCatalog::STACK_FIELDS, 16384);
        $set = ProvCatalog::stackFields($b, false);
        ProvCatalog::tx(static function () use ($id, $set, $stackGet): void {
            if ($stackGet($id, true) === null) {
                ProvCatalog::notFound('Stack not found.');
            }
            ProvCatalog::updateRow('prov_stacks', $id, $set);
        });
        Response::ok($stackDetail($stackGet($id) ?? ProvCatalog::notFound('Stack not found.')));
    });

    // Soft delete: refused for a locked stack or one a live stack depends on;
    // its memberships and outgoing deps go with it (029 CASCADE).
    $router->add('DELETE', "$P/stacks/{id}", static function (array $p) use ($stackGet): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        ProvCatalog::tx(static function () use ($id, $stackGet): void {
            $st = $stackGet($id, true);
            if ($st === null) {
                ProvCatalog::notFound('Stack not found.');
            }
            if ((int) $st['locked'] === 1) {
                ProvCatalog::conflict('A locked stack cannot be deleted.');
            }
            $dependents = ProvCatalog::rows(
                'SELECT s.slug FROM prov_stack_deps d JOIN prov_stacks s ON s.id = d.stack_id
                  WHERE d.depends_on_stack_id = :id AND d.deleted_at IS NULL AND s.deleted_at IS NULL
                  ORDER BY s.slug', [':id' => $id]);
            if ($dependents !== []) {
                ProvCatalog::conflict('Stack is a dependency of: '
                    . implode(', ', array_column($dependents, 'slug')) . '. Remove those dependencies first.');
            }
            ProvCatalog::exec("UPDATE prov_stacks SET deleted_at = NOW(), source_id = :src, asserted_kind = 'human',
                                      asserted_at = NOW() WHERE id = :id AND deleted_at IS NULL",
                ProvCatalog::prov() + [':id' => $id]);
            ProvCatalog::exec('UPDATE prov_stack_members SET deleted_at = NOW()
                                WHERE stack_id = :id AND deleted_at IS NULL', [':id' => $id]);
            ProvCatalog::exec('UPDATE prov_stack_deps SET deleted_at = NOW()
                                WHERE stack_id = :id AND deleted_at IS NULL', [':id' => $id]);
        });
        Response::ok(['id' => $id, 'deleted' => true]);
    });

    // members -----------------------------------------------------------
    $router->post("$P/stacks/{id}/members", static function (array $p) use ($stackGet, $softwareGet, $stackDetail): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $b = ProvCatalog::body(['software_id', 'item_order', 'sort_order', 'notes'], 16384);
        $sw = ProvCatalog::idValue(ProvCatalog::requireKey($b, 'software_id'), 'software_id');
        $set = ['stack_id' => $id, 'software_id' => $sw] + ProvCatalog::memberFields($b, true);
        ProvCatalog::tx(static function () use ($id, $sw, $set, $stackGet, $softwareGet): void {
            if ($stackGet($id, true) === null) {
                ProvCatalog::notFound('Stack not found.');
            }
            if ($softwareGet($sw, true) === null) {
                ProvCatalog::notFound('Software entry not found.');
            }
            ProvCatalog::insertRow('prov_stack_members', $set);
        });
        Response::ok($stackDetail($stackGet($id) ?? ProvCatalog::notFound('Stack not found.')), 201);
    });

    $router->add('PUT', "$P/stacks/{id}/members/{softwareId}", static function (array $p) use ($stackGet, $stackDetail): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $sw = ProvCatalog::pathId($p['softwareId'], 'softwareId');
        $set = ProvCatalog::memberFields(ProvCatalog::body(ProvCatalog::MEMBER_FIELDS, 16384), false);
        $params = ProvCatalog::prov() + [':stack' => $id, ':sw' => $sw];
        $parts = [];
        foreach ($set as $c => $v) {
            $parts[] = "$c = :$c";
            $params[":$c"] = $v;
        }
        $n = ProvCatalog::exec('UPDATE prov_stack_members SET ' . implode(', ', $parts)
            . ", source_id = :src, asserted_kind = 'human', asserted_at = NOW()
              WHERE stack_id = :stack AND software_id = :sw AND deleted_at IS NULL", $params);
        if ($n === 0 && ProvCatalog::scalar('SELECT COUNT(*) AS n FROM prov_stack_members
                 WHERE stack_id = :stack AND software_id = :sw AND deleted_at IS NULL',
                [':stack' => $id, ':sw' => $sw]) == 0) {
            ProvCatalog::notFound('Stack member not found.');
        }
        Response::ok($stackDetail($stackGet($id) ?? ProvCatalog::notFound('Stack not found.')));
    });

    $router->add('DELETE', "$P/stacks/{id}/members/{softwareId}", static function (array $p): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $sw = ProvCatalog::pathId($p['softwareId'], 'softwareId');
        $n = ProvCatalog::exec('UPDATE prov_stack_members SET deleted_at = NOW()
                                 WHERE stack_id = :stack AND software_id = :sw AND deleted_at IS NULL',
            [':stack' => $id, ':sw' => $sw]);
        if ($n === 0) {
            ProvCatalog::notFound('Stack member not found.');
        }
        Response::ok(['stack_id' => $id, 'software_id' => $sw, 'deleted' => true]);
    });

    // deps --------------------------------------------------------------
    $router->post("$P/stacks/{id}/deps", static function (array $p) use ($stackGet, $stackDetail): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $b = ProvCatalog::body(['depends_on_stack_id'], 4096);
        $on = ProvCatalog::idValue(ProvCatalog::requireKey($b, 'depends_on_stack_id'), 'depends_on_stack_id');
        if ($on === $id) {
            ProvCatalog::bad('A stack cannot depend on itself.');
        }
        ProvCatalog::tx(static function () use ($id, $on, $stackGet): void {
            if ($stackGet($id, true) === null || $stackGet($on, true) === null) {
                ProvCatalog::notFound('Stack not found.');
            }
            ProvCatalog::insertRow('prov_stack_deps', ['stack_id' => $id, 'depends_on_stack_id' => $on]);
        });
        Response::ok($stackDetail($stackGet($id) ?? ProvCatalog::notFound('Stack not found.')), 201);
    });

    $router->add('DELETE', "$P/stacks/{id}/deps/{dependsOnId}", static function (array $p): void {
        Operator::requireOperator();
        $id = ProvCatalog::pathId($p['id']);
        $on = ProvCatalog::pathId($p['dependsOnId'], 'dependsOnId');
        $n = ProvCatalog::exec('UPDATE prov_stack_deps SET deleted_at = NOW()
                                 WHERE stack_id = :id AND depends_on_stack_id = :on AND deleted_at IS NULL',
            [':id' => $id, ':on' => $on]);
        if ($n === 0) {
            ProvCatalog::notFound('Stack dependency not found.');
        }
        Response::ok(['stack_id' => $id, 'depends_on_stack_id' => $on, 'deleted' => true]);
    });

    // ------------------------------------------------------------ targets
    // Deploy targets for the P4 screen: live SoR entities with at least one
    // live MAC-bearing interface (PROV_STAGE refuses anything else) plus the
    // hardware model. CPU arch precedence (031): the operator-set
    // hardware_models.arch, else the agent-reported monitoring `node.arch`
    // (matched on the short hostname, normalised to the prov_images arch
    // enum), else null = unknown. POST stage resolves it the same way.

    // short hostname => normalised agent arch; [] when monitoring is down.
    $agentArch = static function (): array {
        $arch = [];
        try {
            foreach (Db::rows("SELECT hostFqdn, arch FROM node WHERE state <> 'retired' AND arch IS NOT NULL") as $n) {
                $short = strtolower(explode('.', (string) $n['hostFqdn'])[0]);
                $norm = ProvCatalog::normArch((string) $n['arch']);
                if ($short !== '' && $norm !== null) {
                    $arch[$short] = $norm;
                }
            }
        } catch (Throwable $e) {
            return [];   // monitoring DB unavailable: arch falls back to unknown
        }
        return $arch;
    };
    // [arch|null, source 'hardware_model'|'agent'|null]
    $resolveArch = static function (?string $hwArch, string $name, array $agent): array {
        $hw = $hwArch === null ? null : ProvCatalog::normArch($hwArch);
        if ($hw !== null) {
            return [$hw, 'hardware_model'];
        }
        $a = $agent[strtolower($name)] ?? null;
        return $a === null ? [null, null] : [$a, 'agent'];
    };

    $router->get("$P/targets", static function () use ($agentArch, $resolveArch): void {
        $rows = ProvCatalog::rows(
            "SELECT e.id, e.name, e.entity_type, e.lifecycle, e.role,
                    hm.id AS hw_model_id, hm.make AS hw_make, hm.model AS hw_model, hm.hw_type,
                    hm.arch AS hw_arch,
                    i.name AS if_name, i.mac, i.is_management
               FROM entities e
               JOIN interfaces i ON i.entity_id = e.id AND i.deleted_at IS NULL AND i.mac IS NOT NULL
               LEFT JOIN hardware_units hu ON hu.id = e.hardware_unit_id AND hu.deleted_at IS NULL
               LEFT JOIN hardware_models hm ON hm.id = hu.model_id AND hm.deleted_at IS NULL
              WHERE e.deleted_at IS NULL
                AND e.lifecycle NOT IN ('retired', 'decommissioned')
                AND e.entity_type IN ('server', 'workstation', 'vm', 'appliance', 'iot_device', 'other')
              ORDER BY e.name, e.id, i.is_management DESC, i.name");
        $agent = $agentArch();
        $out = [];
        foreach ($rows as $r) {
            $id = (string) $r['id'];
            if (!isset($out[$id])) {
                [$a, $src] = $resolveArch($r['hw_arch'] ?? null, (string) $r['name'], $agent);
                $out[$id] = [
                    'id'          => $id,
                    'name'        => $r['name'],
                    'entity_type' => $r['entity_type'],
                    'lifecycle'   => $r['lifecycle'],
                    'role'        => $r['role'],
                    'hw_model_id' => ($r['hw_model_id'] ?? null) === null ? null : (int) $r['hw_model_id'],
                    'hw_make'     => $r['hw_make'],
                    'hw_model'    => $r['hw_model'],
                    'hw_type'     => $r['hw_type'],
                    'arch'        => $a,
                    'arch_source' => $src,
                    // agent's view even when the hardware model wins, so the
                    // UI can flag a model/agent disagreement
                    'agent_arch'  => $agent[strtolower((string) $r['name'])] ?? null,
                    'interfaces'  => [],
                ];
            }
            $out[$id]['interfaces'][] = [
                'name' => $r['if_name'], 'mac' => $r['mac'], 'management' => (int) $r['is_management'] === 1,
            ];
        }
        Response::ok(array_values($out));
    });

    // ---------------------------------------------------------------- log
    // Tail of a helper's per-run log. The C helpers write
    // <logDir>/<kind>-<id>.log, 0640 to the ctl socket group (4a6d822) so the
    // dashboard can read it; logDir defaults to the ctl socket directory, the
    // same convention GET /api/control/deploy uses. Bounded to the last 64 KiB.

    $router->get("$P/log", static function (): void {
        $kind = $_GET['kind'] ?? '';
        if (!is_string($kind) || !in_array($kind, ['image-refresh', 'prov-stage', 'stack-apply', 'fleet-updates'], true)) {
            ProvCatalog::bad('kind must be one of: image-refresh, prov-stage, stack-apply, fleet-updates.');
        }
        $id = $kind === 'fleet-updates' ? 1 : ProvCatalog::idValue($_GET['id'] ?? null, 'id');
        $sock = getenv('SOLARI_CTL_SOCK') ?: '/run/solari/solariCtl.sock';
        $file = dirname($sock) . '/' . $kind . '-' . $id . '.log';
        $max = 65536;
        $log = '';
        $size = 0;
        $mtime = null;
        // Never follow a link at the expected name, and read from the very
        // inode that was checked; size and the byte cap come from the open fd
        // so a log growing mid-read cannot exceed $max.
        $st = @lstat($file);
        $fh = ($st !== false && ($st['mode'] & 0170000) === 0100000) ? @fopen($file, 'rb') : false;
        if ($fh !== false) {
            $fs = fstat($fh);
            if ($fs !== false && $fs['dev'] === $st['dev'] && $fs['ino'] === $st['ino']) {
                $size = (int) $fs['size'];
                $mtime = gmdate('Y-m-d\TH:i:s\Z', (int) $fs['mtime']);
                if ($size > $max) {
                    fseek($fh, $size - $max, SEEK_SET);
                }
                $log = (string) stream_get_contents($fh, $max);
            }
            fclose($fh);
            if ($size > $max && ($nl = strpos($log, "\n")) !== false) {
                $log = substr($log, $nl + 1);   // start the tail on a whole line
            }
            $log = (string) preg_replace('/\x1b\[[0-9;]*m/', '', $log);
        }
        Response::ok([
            'kind'       => $kind,
            'id'         => $id,
            'exists'     => $mtime !== null,
            'log'        => $log,
            'truncated'  => $size > $max,
            'size'       => $size,
            'updated_at' => $mtime,
        ]);
    });

    $router->post("$P/updates", static function (): void {
        $op = Operator::requireOperator();
        $idem = ProvCatalog::idemKey();
        $b = ProvCatalog::body(['mode', 'confirm'], 4096);
        $mode = $b['mode'] ?? null;
        if (!is_string($mode) || !in_array($mode, ['check', 'apply'], true)) {
            ProvCatalog::bad('mode must be check or apply.');
        }
        if ($mode === 'apply' && ($b['confirm'] ?? null) !== true) {
            ProvCatalog::conflict('Applying OS updates requires confirm=true.');
        }
        if (array_key_exists('confirm', $b) && !is_bool($b['confirm'])) {
            ProvCatalog::bad('confirm must be boolean.');
        }
        $reply = SolariCtl::callQueued('FLEET_UPDATES', ['mode' => $mode], $op, $idem, 4.0);
        Response::ok(ProvCatalog::queuedOut(['mode' => $mode], $reply), 202);
    });

    // -------------------------------------------------------------- store
    // The NAS image store (installers/ + images/ on benzene) as last checked
    // by prov-finish.py (every timer tick, ~2 min) — provlib.store_status()
    // written to <ctl socket dir>/prov-store.json. No new privileged path:
    // PHP only reads that file, with the same no-link / same-inode rule as
    // the log tail. `stale` once the check is older than 10 minutes (the
    // timer or benzene's ssh is down; the store state is then unknown).

    $router->get("$P/store", static function (): void {
        $sock = getenv('SOLARI_CTL_SOCK') ?: '/run/solari/solariCtl.sock';
        $file = dirname($sock) . '/prov-store.json';
        $raw = false;
        $st = @lstat($file);
        $fh = ($st !== false && ($st['mode'] & 0170000) === 0100000) ? @fopen($file, 'rb') : false;
        if ($fh !== false) {
            $fs = fstat($fh);
            if ($fs !== false && $fs['dev'] === $st['dev'] && $fs['ino'] === $st['ino']) {
                $raw = stream_get_contents($fh, 16384);
            }
            fclose($fh);
        }
        $d = is_string($raw) ? json_decode($raw, true) : null;
        if (!is_array($d)) {
            Response::ok(['known' => false, 'stale' => true, 'checked_at' => null]);
        }
        $str = static fn($v): ?string => is_string($v) && $v !== '' ? substr($v, 0, 512) : null;
        $num = static fn($v): ?int => is_int($v) && $v >= 0 ? $v : null;
        $checked = $str($d['checked_at'] ?? null);
        $ts = $checked !== null ? strtotime($checked) : false;
        Response::ok([
            'known'       => true,
            'store'       => $str($d['store'] ?? null),
            'fstype'      => $str($d['fstype'] ?? null),
            'source'      => $str($d['source'] ?? null),
            'reachable'   => ($d['reachable'] ?? false) === true,
            'mounted'     => ($d['mounted'] ?? false) === true,
            'linked'      => ($d['linked'] ?? false) === true,
            'ok'          => ($d['ok'] ?? false) === true,
            'size_bytes'  => $num($d['size_bytes'] ?? null),
            'avail_bytes' => $num($d['avail_bytes'] ?? null),
            'reason'      => $str($d['reason'] ?? null),
            'checked_at'  => $checked,
            'stale'       => $ts === false || time() - $ts > 600,
        ]);
    });

    // --------------------------------------------------------------- runs

    $RUN_SQL = "SELECT r.id, r.mode, r.entity_id, e.name AS entity_name, r.image_id,
                       CONCAT(i.distro, ' ', i.release_name, ' ', i.arch) AS image_label,
                       r.stack_slugs, r.requested_by, r.status, r.dry_run, r.detail,
                       r.requested_at, r.started_at, r.finished_at
                  FROM prov_runs r
                  LEFT JOIN entities e ON e.id = r.entity_id
                  LEFT JOIN prov_images i ON i.id = r.image_id";

    $router->get("$P/runs", static function () use ($RUN_SQL): void {
        $where = [];
        $params = [];
        if (isset($_GET['entity']) && $_GET['entity'] !== '') {
            $params[':entity'] = ProvCatalog::idValue($_GET['entity'], 'entity');
            $where[] = 'r.entity_id = :entity';
        }
        if (isset($_GET['image']) && $_GET['image'] !== '') {
            $params[':image'] = ProvCatalog::idValue($_GET['image'], 'image');
            $where[] = 'r.image_id = :image';
        }
        if (isset($_GET['mode']) && $_GET['mode'] !== '') {
            $params[':mode'] = ProvCatalog::enum($_GET['mode'], 'mode', ProvCatalog::RUN_MODES);
            $where[] = 'r.mode = :mode';
        }
        if (isset($_GET['status']) && $_GET['status'] !== '') {
            $params[':status'] = ProvCatalog::enum($_GET['status'], 'status', ProvCatalog::RUN_STATUSES);
            $where[] = 'r.status = :status';
        }
        $limit = 50;
        if (isset($_GET['limit']) && $_GET['limit'] !== '') {
            $l = $_GET['limit'];
            if (!is_string($l) || preg_match('/^[1-9][0-9]{0,2}\z/', $l) !== 1 || (int) $l > 200) {
                ProvCatalog::bad('limit must be an integer 1..200.');
            }
            $limit = (int) $l;
        }
        $sql = $RUN_SQL . ($where ? ' WHERE ' . implode(' AND ', $where) : '')
             . " ORDER BY r.requested_at DESC, r.id DESC LIMIT $limit";
        Response::ok(array_map([ProvCatalog::class, 'runOut'], ProvCatalog::rows($sql, $params)));
    });

    $router->get("$P/runs/{id}", static function (array $p) use ($RUN_SQL): void {
        $raw = (string) $p['id'];
        if (preg_match('/^[1-9][0-9]{0,17}\z/', $raw) !== 1) {
            ProvCatalog::bad('id must be a positive integer run id.');
        }
        $r = ProvCatalog::row($RUN_SQL . ' WHERE r.id = :id', [':id' => $raw]);
        if ($r === null) {
            ProvCatalog::notFound('Run not found.');
        }
        Response::ok(ProvCatalog::runOut($r));
    });

    // ------------------------------------------------------------ enqueue
    // Pre-validated against the SoR for clean 4xx; the helpers re-validate
    // everything themselves (TOCTOU is theirs to close). op is bound by
    // callQueued from the session principal — NEVER placed in $args.

    $liveEntity = static function (int $id): void {
        if (ProvCatalog::row('SELECT id FROM entities WHERE id = :id AND deleted_at IS NULL', [':id' => $id]) === null) {
            ProvCatalog::notFound('Entity not found.');
        }
    };
    $liveStacks = static function (array $slugs): void {
        $params = [];
        foreach (array_values($slugs) as $i => $s) {
            $params[":s$i"] = $s;
        }
        $found = array_column(ProvCatalog::rows(
            'SELECT slug FROM prov_stacks WHERE deleted_at IS NULL AND slug IN ('
            . implode(', ', array_keys($params)) . ')', $params), 'slug');
        $missing = array_values(array_diff($slugs, $found));
        if ($missing !== []) {
            ProvCatalog::notFound('Unknown stack(s): ' . implode(', ', $missing) . '.');
        }
    };

    $router->post("$P/image-refresh", static function () use ($imageGet): void {
        $op = Operator::requireOperator();
        $idem = ProvCatalog::idemKey();
        $b = ProvCatalog::body(['image'], 4096);
        $image = ProvCatalog::idValue(ProvCatalog::requireKey($b, 'image'), 'image');
        if ($imageGet($image) === null) {
            ProvCatalog::notFound('Image not found.');
        }
        $args = ['image' => (string) $image];
        $reply = SolariCtl::callQueued('IMAGE_REFRESH', $args, $op, $idem);
        Response::ok(ProvCatalog::queuedOut(['image' => $image], $reply));
    });

    // PROV_STAGE: the operator picks which interface netboots (mac), and the
    // target arch is resolved here exactly as GET targets does — a client
    // arch is never accepted. Unknown arch or an image/target mismatch is
    // refused: staging the wrong arch boots nothing (or the wrong thing).
    $router->post("$P/stage", static function () use ($imageGet, $liveStacks, $agentArch, $resolveArch): void {
        $op = Operator::requireOperator();
        $idem = ProvCatalog::idemKey();
        $b = ProvCatalog::body(['entity', 'image', 'stacks', 'mac'], 16384);
        $entity = ProvCatalog::idValue(ProvCatalog::requireKey($b, 'entity'), 'entity');
        $image  = ProvCatalog::idValue(ProvCatalog::requireKey($b, 'image'), 'image');
        $stacks = ProvCatalog::stackList(ProvCatalog::requireKey($b, 'stacks'));
        $mac    = ProvCatalog::macValue(ProvCatalog::requireKey($b, 'mac'));
        $ent = ProvCatalog::row(
            'SELECT e.id, e.name, hm.id AS hw_model_id, hm.make AS hw_make, hm.model AS hw_model,
                    hm.arch AS hw_arch
               FROM entities e
               LEFT JOIN hardware_units hu ON hu.id = e.hardware_unit_id AND hu.deleted_at IS NULL
               LEFT JOIN hardware_models hm ON hm.id = hu.model_id AND hm.deleted_at IS NULL
              WHERE e.id = :id AND e.deleted_at IS NULL', [':id' => $entity]);
        if ($ent === null) {
            ProvCatalog::notFound('Entity not found.');
        }
        $macs = [];
        foreach (ProvCatalog::rows('SELECT mac FROM interfaces
                WHERE entity_id = :id AND deleted_at IS NULL AND mac IS NOT NULL', [':id' => $entity]) as $m) {
            $macs[ProvCatalog::normMac((string) $m['mac'])] = true;
        }
        if ($macs === []) {
            ProvCatalog::conflict('Entity has no live interface with a MAC address; it cannot be netbooted.');
        }
        if (!isset($macs[$mac])) {
            ProvCatalog::conflict("MAC $mac is not a live interface of " . $ent['name'] . '.');
        }
        $img = $imageGet($image);
        if ($img === null) {
            ProvCatalog::notFound('Image not found.');
        }
        if ($img['boot_method'] !== 'ipxe') {
            ProvCatalog::conflict('Only ipxe images can be staged for netboot (this image is '
                . $img['boot_method'] . ').');
        }
        if ($img['sha256'] === null || $img['fetched_at'] === null) {
            ProvCatalog::conflict('image not fetched — refresh it first');
        }
        [$arch, $archSrc] = $resolveArch($ent['hw_arch'] ?? null, (string) $ent['name'], $agentArch());
        if ($arch === null) {
            ProvCatalog::conflict(($ent['hw_model_id'] ?? null) === null
                ? 'Target CPU architecture is unknown: assign ' . $ent['name']
                  . ' a hardware model with a CPU architecture (Inventory) or let the agent report it.'
                : 'Target CPU architecture is unknown: set the CPU architecture on hardware model '
                  . trim($ent['hw_make'] . ' ' . $ent['hw_model']) . ' (Inventory) or let the agent report it.');
        }
        if ($img['arch'] !== $arch) {
            ProvCatalog::conflict('Image is ' . $img['arch'] . ' but target is ' . $arch
                . ($archSrc === 'agent' ? ' (agent-reported)' : ' (hardware model)') . '.');
        }
        $liveStacks($stacks);
        $args = ['entity' => (string) $entity, 'image' => (string) $image, 'stacks' => implode(',', $stacks),
                 'mac' => $mac, 'arch' => $arch];
        $reply = SolariCtl::callQueued('PROV_STAGE', $args, $op, $idem);
        Response::ok(ProvCatalog::queuedOut(['entity' => $entity, 'image' => $image, 'stacks' => $stacks,
                                             'mac' => $mac, 'arch' => $arch, 'arch_source' => $archSrc], $reply));
    });

    $router->post("$P/apply", static function () use ($liveEntity, $liveStacks): void {
        $op = Operator::requireOperator();
        $idem = ProvCatalog::idemKey();
        $b = ProvCatalog::body(['entity', 'stacks', 'dry_run'], 16384);
        $entity = ProvCatalog::idValue(ProvCatalog::requireKey($b, 'entity'), 'entity');
        $stacks = ProvCatalog::stackList(ProvCatalog::requireKey($b, 'stacks'));
        if (!array_key_exists('dry_run', $b) || !is_bool($b['dry_run'])) {
            ProvCatalog::bad('dry_run is required and must be a boolean.');
        }
        $dry = $b['dry_run'];
        $liveEntity($entity);
        $liveStacks($stacks);
        $args = ['entity' => (string) $entity, 'stacks' => implode(',', $stacks), 'dryRun' => $dry ? '1' : '0'];
        $reply = SolariCtl::callQueued('STACK_APPLY', $args, $op, $idem);
        Response::ok(ProvCatalog::queuedOut(
            ['entity' => $entity, 'stacks' => $stacks, 'dry_run' => $dry], $reply));
    });
};
