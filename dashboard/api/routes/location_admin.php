<?php
declare(strict_types=1);

/**
 * Inventory I1 WP4: location move, merge, bulk storage units, container
 * project assignment, and container types.
 *
 * Loaded after inventory.php for its Inv and inv_* helpers. Every write
 * validates in PHP first and returns a named error; migration 036's triggers
 * and live sibling unique key are the backstop, and loc_map_exception() turns
 * their errors into the same codes rather than a 500.
 *
 * Level rule (036): a root is a property; property → floor → room → section;
 * a storage_unit may sit under any location, including another storage_unit.
 */
/** Level each non-storage location type may contain (036 trigger rule). */
const LOC_CHILD_LEVEL = ['property' => 'floor', 'floor' => 'room', 'room' => 'section'];

/** Polymorphic location references: counted and locked for merge; only live barcodes are retargeted. */
const LOC_POLYMORPHIC_REFS = [
    'barcodes'      => "SELECT id, code FROM barcodes WHERE subject_table = 'locations' AND subject_id = :id AND deleted_at IS NULL ORDER BY id",
    'barcode_scans' => "SELECT id FROM barcode_scans WHERE subject_table = 'locations' AND subject_id = :id ORDER BY id",
    'external_refs' => "SELECT id FROM external_refs WHERE subject_table = 'locations' AND subject_id = :id ORDER BY id",
];

return static function (Router $router): void {
    $read  = Policy::INVENTORY_CATALOG_READ;
    $write = Policy::INVENTORY_CATALOG_WRITE; // Policy::enforce also requires same-origin CSRF.

    // ------------------------------------------------------------------
    // Move: body {parent_id} (null = make a root property).
    // ------------------------------------------------------------------
    $router->post('/api/inventory/locations/{id}/move', static function (array $p): void {
        $id   = Inv::idParam($p['id']);
        $body = solari_json_body();
        if (!array_key_exists('parent_id', $body)) {
            Response::error('bad_request', 'parent_id is required (nullable).', 400);
        }
        $parentId = loc_optional_id($body['parent_id'], 'parent_id');
        $source   = Sor::dashboardSourceId();

        $result = loc_transaction(static function (PDO $db) use ($id, $parentId, $source): array {
            $loc = loc_lock($db, $id);
            if ($loc === null || $loc['deleted_at'] !== null) {
                loc_error('not_found', "No location $id", 404);
            }
            $oldParent = loc_nullable_int($loc['parent_id']);

            loc_walk_ancestors($db, $parentId, $id);
            loc_check_transition($db, $parentId, (string) $loc['loc_type']);
            loc_sibling_available($db, (string) $loc['name'], $parentId, $id);

            $db->prepare("UPDATE locations SET parent_id = :parent, asserted_kind = 'human', asserted_at = NOW() WHERE id = :id")
               ->execute([':parent' => $parentId, ':id' => $id]);
            loc_audit($db, 'locations', $id, 'update', ['parent_id' => $oldParent], ['parent_id' => $parentId], 'Location move', $source);

            return ['id' => $id, 'parent_id' => $parentId, 'old_parent_id' => $oldParent];
        });
        Response::ok($result);
    }, $write);

    // ------------------------------------------------------------------
    // Merge preview: counts per reference class plus a fingerprint the
    // merge must echo back. Read-only; the merge recomputes under lock.
    // ------------------------------------------------------------------
    $router->get('/api/inventory/locations/{id}/merge-preview', static function (array $p): void {
        $id     = Inv::idParam($p['id']);
        $target = loc_query_id($_GET['target_id'] ?? null, 'target_id');
        if ($id === $target) {
            Response::error('bad_request', 'Source and target must differ.', 400);
        }
        $db        = Sor::db();
        $sourceRow = loc_live_location($db, $id);
        $targetRow = loc_live_location($db, $target);
        if ($sourceRow === null || $targetRow === null) {
            Response::error('not_found', 'Source or target location not found.', 404);
        }
        $fks    = loc_discover_fks($db);
        $counts = loc_merge_counts($db, $id, $fks);
        Response::ok([
            'source'      => loc_public_location($sourceRow),
            'target'      => loc_public_location($targetRow),
            'counts'      => $counts,
            'fingerprint' => loc_merge_fingerprint($db, $sourceRow, $targetRow, $counts, $fks),
        ]);
    }, $read);

    // ------------------------------------------------------------------
    // Merge source {id} into body {target_id}; body {fingerprint} from the
    // preview. Never hard-deletes: the source is soft-deleted and the
    // audit row records the target. Scan and external-ref history stay
    // pointed at the source as immutable snapshots.
    // ------------------------------------------------------------------
    $router->post('/api/inventory/locations/{id}/merge', static function (array $p): void {
        $id          = Inv::idParam($p['id']);
        $body        = solari_json_body();
        $target      = loc_required_id($body, 'target_id');
        $fingerprint = $body['fingerprint'] ?? null;
        if (!is_string($fingerprint) || !preg_match('/\A[a-f0-9]{64}\z/', $fingerprint)) {
            Response::error('bad_request', 'fingerprint is required.', 400);
        }
        if ($id === $target) {
            Response::error('bad_request', 'Source and target must differ.', 400);
        }
        $auditSource = Sor::dashboardSourceId();

        $result = loc_transaction(static function (PDO $db) use ($id, $target, $fingerprint, $auditSource): array {
            $source = loc_lock($db, $id);
            $dest   = loc_lock($db, $target);
            if ($source === null || $source['deleted_at'] !== null) {
                // A repeat of a merge that already committed is a no-op success.
                if (loc_completed_merge($db, $id, $target)) {
                    return ['source_id' => $id, 'target_id' => $target, 'merged' => true, 'idempotent' => true, 'counts' => [], 'moved' => []];
                }
                loc_error('not_found', 'Source location not found.', 404);
            }
            if ($dest === null || $dest['deleted_at'] !== null) {
                loc_error('not_found', 'Target location not found.', 404);
            }

            // Refuse before touching anything if a hard FK exists that this
            // code does not know how to rewrite.
            $fks = loc_discover_fks($db);
            foreach ($fks as $fk) {
                if (!loc_fk_supported($fk)) {
                    loc_error('merge_conflict', 'Merge refused: unsupported location reference ' . $fk['table_name'] . '.' . $fk['column_name'], 409);
                }
            }
            loc_lock_merge_refs($db, $id, $fks);
            $counts = loc_merge_counts($db, $id, $fks);
            if (!hash_equals($fingerprint, loc_merge_fingerprint($db, $source, $dest, $counts, $fks))) {
                loc_error('merge_conflict', 'Location references changed after preview.', 409);
            }

            if ($source['loc_type'] !== $dest['loc_type']) {
                loc_error('merge_conflict', 'Locations must have the same level.', 409);
            }
            $srcProject = loc_nullable_int($source['assigned_project_id']);
            $dstProject = loc_nullable_int($dest['assigned_project_id']);
            if ($srcProject !== null && $dstProject !== null && $srcProject !== $dstProject) {
                loc_error('merge_conflict', 'Locations are assigned to different projects.', 409);
            }
            // A target inside the source subtree would form a cycle.
            loc_walk_ancestors($db, $target, $id);

            $children = $db->prepare('SELECT id, name, loc_type FROM locations WHERE parent_id = :id AND deleted_at IS NULL ORDER BY id FOR UPDATE');
            $children->execute([':id' => $id]);
            $childRows = $children->fetchAll();
            foreach ($childRows as $child) {
                if (loc_sibling_collision($db, (string) $child['name'], $target, (int) $child['id'])) {
                    loc_error('merge_conflict', 'A source child name already exists under the target.', 409);
                }
                loc_check_transition($db, $target, (string) $child['loc_type']);
            }

            // Rewrite every known hard reference.
            $moved = [];
            foreach ($fks as $fk) {
                $table  = (string) $fk['table_name'];
                $column = (string) $fk['column_name'];
                if ($table === 'locations' && $column === 'parent_id') {
                    $st = $db->prepare("UPDATE locations SET parent_id = :target, asserted_kind = 'human', asserted_at = NOW() WHERE parent_id = :source AND deleted_at IS NULL");
                    $st->execute([':target' => $target, ':source' => $id]);
                    $moved['locations.parent_id'] = $st->rowCount();
                    foreach ($childRows as $child) {
                        loc_audit($db, 'locations', (int) $child['id'], 'update', ['parent_id' => $id], ['parent_id' => $target], 'Child moved by location merge', $auditSource);
                    }
                    continue;
                }
                $st = $db->prepare('UPDATE ' . loc_ident($table) . ' SET ' . loc_ident($column) . ' = :target WHERE ' . loc_ident($column) . ' = :source');
                $st->execute([':target' => $target, ':source' => $id]);
                if ($st->rowCount() > 0) {
                    $moved[$table . '.' . $column] = $st->rowCount();
                }
            }

            // Live system codes follow the location; barcodes.code is unique,
            // so retargeting cannot create a duplicate.
            $st = $db->prepare("UPDATE barcodes SET subject_id = :target, asserted_kind = 'human', asserted_at = NOW() WHERE subject_table = 'locations' AND subject_id = :source AND deleted_at IS NULL");
            $st->execute([':target' => $target, ':source' => $id]);
            if ($st->rowCount() > 0) {
                $moved['barcodes'] = $st->rowCount();
            }

            if ($dstProject === null && $srcProject !== null) {
                $db->prepare("UPDATE locations SET assigned_project_id = :project, asserted_kind = 'human', asserted_at = NOW() WHERE id = :id")
                   ->execute([':project' => $srcProject, ':id' => $target]);
                loc_audit($db, 'locations', $target, 'update', ['assigned_project_id' => null], ['assigned_project_id' => $srcProject], 'Project inherited by location merge', $auditSource);
            }

            $db->prepare("UPDATE locations SET deleted_at = NOW(), asserted_kind = 'human', asserted_at = NOW() WHERE id = :id")
               ->execute([':id' => $id]);
            loc_audit(
                $db, 'locations', $id, 'soft_delete',
                ['deleted_at' => null, 'parent_id' => loc_nullable_int($source['parent_id'])],
                ['deleted_at' => 'merged', 'merge_target_id' => $target, 'merge_references' => $moved],
                'Merged into location ' . $target,
                $auditSource
            );

            return ['source_id' => $id, 'target_id' => $target, 'merged' => true, 'idempotent' => false, 'counts' => $counts, 'moved' => $moved];
        });
        Response::ok($result);
    }, $write);

    // ------------------------------------------------------------------
    // Bulk storage units: body {parent_id, count 1..500, names[] | prefix +
    // start, code_prefix?, code_start?, container_type_id?, project_id?}.
    // All names and codes are prevalidated; all-or-none in one transaction.
    // ------------------------------------------------------------------
    $router->post('/api/inventory/locations/bulk-storage-units', static function (): void {
        $body   = solari_json_body();
        $parent = loc_required_nullable_id($body, 'parent_id');
        $count  = $body['count'] ?? null;
        if (!is_int($count) || $count < 1 || $count > 500) {
            Response::error('bad_request', 'count must be an integer from 1 to 500.', 400);
        }

        $names = $body['names'] ?? null;
        if ($names !== null) {
            if (!is_array($names) || !array_is_list($names) || count($names) !== $count) {
                Response::error('bad_request', 'names must contain exactly count strings.', 400);
            }
            $names = array_map(static fn($v): string => loc_name_value($v), $names);
        } else {
            $prefix = $body['prefix'] ?? null;
            $start  = $body['start'] ?? null;
            if (!is_string($prefix) || trim($prefix) === '' || mb_strlen(trim($prefix)) > 100 || !is_int($start) || $start < 0) {
                Response::error('bad_request', 'Supply explicit names, or prefix with a non-negative integer start.', 400);
            }
            $names = [];
            for ($i = 0; $i < $count; $i++) {
                $names[] = loc_name_value(trim($prefix) . ($start + $i));
            }
        }

        $codePrefix = $body['code_prefix'] ?? null;
        $codeStart  = $body['code_start'] ?? 0;
        if ($codePrefix !== null && (!is_string($codePrefix) || trim($codePrefix) === '' || mb_strlen(trim($codePrefix)) > 20 || !is_int($codeStart) || $codeStart < 0)) {
            Response::error('bad_request', 'code_prefix/code_start is invalid.', 400);
        }
        $codes = [];
        foreach (array_keys($names) as $i) {
            $code = $codePrefix === null ? null : trim($codePrefix) . ($codeStart + $i);
            if ($code !== null && mb_strlen($code) > 32) {
                Response::error('bad_request', 'Generated code exceeds 32 characters.', 400);
            }
            $codes[] = $code;
        }

        $containerType = loc_optional_id($body['container_type_id'] ?? null, 'container_type_id');
        $project       = loc_optional_id($body['project_id'] ?? null, 'project_id');
        $source        = Sor::dashboardSourceId();

        $result = loc_transaction(static function (PDO $db) use ($parent, $names, $codes, $containerType, $project, $source): array {
            loc_check_transition($db, $parent, 'storage_unit');
            if ($containerType !== null && !loc_live_row($db, 'container_types', $containerType)) {
                loc_error('not_found', 'Container type not found.', 404);
            }
            if ($project !== null && !loc_live_row($db, 'projects', $project)) {
                loc_error('not_found', 'Project not found.', 404);
            }

            $seen = [];
            foreach ($names as $name) {
                $key = mb_strtolower($name);
                if (isset($seen[$key])) {
                    loc_error('duplicate_location', 'Names collide within this batch.', 409);
                }
                $seen[$key] = true;
                loc_sibling_available($db, $name, $parent, null);
            }
            $seenCodes = [];
            $codeLookup = $db->prepare('SELECT id FROM locations WHERE code = :code LIMIT 1 FOR UPDATE');
            foreach ($codes as $code) {
                if ($code === null) {
                    continue;
                }
                $key = mb_strtolower($code);
                if (isset($seenCodes[$key])) {
                    loc_error('duplicate_location', 'Codes collide within this batch.', 409);
                }
                $seenCodes[$key] = true;
                $codeLookup->execute([':code' => $code]);
                if ($codeLookup->fetchColumn() !== false) {
                    loc_error('duplicate_location', "Location code $code already exists.", 409);
                }
            }

            $insert = $db->prepare(
                "INSERT INTO locations (parent_id, name, loc_type, code, container_type_id, assigned_project_id, source_id, asserted_kind, asserted_at)
                 VALUES (:parent, :name, 'storage_unit', :code, :ct, :project, :source, 'human', NOW())"
            );
            $ids = [];
            foreach ($names as $i => $name) {
                $insert->execute([':parent' => $parent, ':name' => $name, ':code' => $codes[$i], ':ct' => $containerType, ':project' => $project, ':source' => $source]);
                $newId = (int) $db->lastInsertId();
                $ids[] = $newId;
                loc_audit($db, 'locations', $newId, 'insert', null, [
                    'parent_id' => $parent, 'name' => $name, 'loc_type' => 'storage_unit', 'code' => $codes[$i],
                    'container_type_id' => $containerType, 'assigned_project_id' => $project,
                ], 'Bulk-created storage unit', $source);
            }
            return ['ids' => $ids, 'count' => count($ids)];
        });
        Response::ok($result, 201);
    }, $write);

    // ------------------------------------------------------------------
    // Container project assignment: body {project_id} (null = release).
    // Releasing needs an empty container and keeps its id and code.
    // ------------------------------------------------------------------
    $router->add('PUT', '/api/inventory/locations/{id}/project', static function (array $p): void {
        $id      = Inv::idParam($p['id']);
        $body    = solari_json_body();
        $project = loc_required_nullable_id($body, 'project_id');
        $source  = Sor::dashboardSourceId();

        $result = loc_transaction(static function (PDO $db) use ($id, $project, $source): array {
            $row = loc_lock($db, $id);
            if ($row === null || $row['deleted_at'] !== null) {
                loc_error('not_found', 'Location not found.', 404);
            }
            if ($row['loc_type'] !== 'storage_unit') {
                loc_error('bad_request', 'Project assignment requires a storage_unit.', 400);
            }
            if ($project !== null && !loc_live_row($db, 'projects', $project)) {
                loc_error('not_found', 'Project not found.', 404);
            }
            $old = loc_nullable_int($row['assigned_project_id']);
            if ($project === null && $old !== null) {
                loc_assert_container_empty($db, $id);
            }
            $db->prepare("UPDATE locations SET assigned_project_id = :project, asserted_kind = 'human', asserted_at = NOW() WHERE id = :id")
               ->execute([':project' => $project, ':id' => $id]);
            loc_audit($db, 'locations', $id, 'update', ['assigned_project_id' => $old], ['assigned_project_id' => $project], 'Container project assignment', $source);
            return ['id' => $id, 'project_id' => $project];
        });
        Response::ok($result);
    }, $write);

    // ------------------------------------------------------------------
    // Container types (D5): rename and retire are allowed while in use;
    // retired types keep their references and leave pick lists.
    // ------------------------------------------------------------------
    $router->get('/api/inventory/container-types', static function (): void {
        $flag = $_GET['include_deleted'] ?? null;
        if ($flag !== null && !in_array($flag, ['0', '1'], true)) {
            Response::error('bad_request', 'include_deleted must be 1 or 0.', 400);
        }
        $sql = loc_container_type_sql() . ($flag === '1' ? '' : ' WHERE deleted_at IS NULL') . ' ORDER BY name, id';
        Response::ok(array_map('loc_type_row', Inv::rows($sql)));
    }, $read);

    $router->post('/api/inventory/container-types', static function (): void {
        $body   = solari_json_body();
        $name   = loc_name_value($body['name'] ?? null, 64);
        $source = Sor::dashboardSourceId();
        $id = loc_transaction(static function (PDO $db) use ($name, $source): int {
            $db->prepare("INSERT INTO container_types (name, source_id, asserted_kind, asserted_at) VALUES (:name, :source, 'human', NOW())")
               ->execute([':name' => $name, ':source' => $source]);
            $id = (int) $db->lastInsertId();
            loc_audit($db, 'container_types', $id, 'insert', null, ['name' => $name], 'Container type create', $source);
            return $id;
        });
        Response::ok(loc_type_row(Inv::row(loc_container_type_sql() . ' WHERE id = :id', [':id' => $id])), 201);
    }, $write);

    $router->add('PUT', '/api/inventory/container-types/{id}', static function (array $p): void {
        $id       = Inv::idParam($p['id']);
        $body     = solari_json_body();
        $expected = $body['updated_at'] ?? null;
        if (!is_string($expected)) {
            Response::error('bad_request', 'updated_at is required.', 400);
        }
        $name   = loc_name_value($body['name'] ?? null, 64);
        $source = Sor::dashboardSourceId();

        loc_transaction(static function (PDO $db) use ($id, $expected, $name, $source): void {
            $st = $db->prepare('SELECT id, name, updated_at FROM container_types WHERE id = :id AND deleted_at IS NULL FOR UPDATE');
            $st->execute([':id' => $id]);
            $old = $st->fetch();
            if (!$old) {
                loc_error('not_found', 'Container type not found.', 404);
            }
            if (!loc_same_timestamp((string) $old['updated_at'], $expected)) {
                loc_error('conflict', 'Container type changed since it was read.', 409);
            }
            $db->prepare("UPDATE container_types SET name = :name, asserted_kind = 'human', asserted_at = NOW() WHERE id = :id")
               ->execute([':name' => $name, ':id' => $id]);
            loc_audit($db, 'container_types', $id, 'update', ['name' => $old['name']], ['name' => $name], 'Container type rename', $source);
        });
        Response::ok(loc_type_row(Inv::row(loc_container_type_sql() . ' WHERE id = :id', [':id' => $id])));
    }, $write);

    $router->add('DELETE', '/api/inventory/container-types/{id}', static function (array $p): void {
        $id     = Inv::idParam($p['id']);
        $source = Sor::dashboardSourceId();
        loc_transaction(static function (PDO $db) use ($id, $source): void {
            $st = $db->prepare('SELECT id, name FROM container_types WHERE id = :id AND deleted_at IS NULL FOR UPDATE');
            $st->execute([':id' => $id]);
            $row = $st->fetch();
            if (!$row) {
                loc_error('not_found', 'Container type not found.', 404);
            }
            $db->prepare("UPDATE container_types SET deleted_at = NOW(), asserted_kind = 'human', asserted_at = NOW() WHERE id = :id")
               ->execute([':id' => $id]);
            loc_audit($db, 'container_types', $id, 'soft_delete', ['deleted_at' => null], ['deleted_at' => 'retired', 'name' => $row['name']], 'Container type retire', $source);
        });
        Response::ok(['id' => $id, 'status' => 'deleted']);
    }, $write);
};

// ======================================================================
// Errors and transactions
// ======================================================================

function loc_error(string $code, string $message, int $status): never
{
    Response::error($code, $message, $status);
}

/** Inv::transaction with database constraint errors mapped to named codes. */
function loc_transaction(callable $fn): mixed
{
    try {
        return Inv::transaction($fn);
    } catch (PDOException $e) {
        loc_map_exception($e);
    }
}

function loc_map_exception(PDOException $e): never
{
    $state   = (string) $e->getCode();
    $errno   = (int) ($e->errorInfo[1] ?? 0);
    $message = (string) ($e->errorInfo[2] ?? $e->getMessage());
    if ($errno === 1062) {
        loc_error('duplicate_location', 'A location or container type with that name already exists.', 409);
    }
    if ($state === '23000') {
        loc_error('conflict', 'A related database constraint prevented the change.', 409);
    }
    if ($state === '45000') {
        // 036 trigger messages.
        if (stripos($message, 'cycle') !== false || stripos($message, 'depth') !== false) {
            loc_error('location_cycle', 'Location hierarchy cycle rejected.', 409);
        }
        if (stripos($message, 'duplicate') !== false) {
            loc_error('duplicate_location', 'A live sibling location already exists.', 409);
        }
        loc_error('bad_request', 'Location level transition is invalid.', 400);
    }
    throw $e;
}

// ======================================================================
// Input
// ======================================================================

function loc_optional_id(mixed $v, string $field): ?int
{
    if ($v === null || $v === '') {
        return null;
    }
    if (!is_int($v) && !(is_string($v) && ctype_digit($v))) {
        loc_error('bad_request', "$field must be a positive integer or null.", 400);
    }
    $n = (int) $v;
    if ($n < 1) {
        loc_error('bad_request', "$field must be a positive integer or null.", 400);
    }
    return $n;
}

function loc_required_id(array $body, string $field): int
{
    $v = array_key_exists($field, $body) ? loc_optional_id($body[$field], $field) : null;
    if ($v === null) {
        loc_error('bad_request', "$field is required.", 400);
    }
    return $v;
}

function loc_required_nullable_id(array $body, string $field): ?int
{
    if (!array_key_exists($field, $body)) {
        loc_error('bad_request', "$field is required and may be null.", 400);
    }
    return loc_optional_id($body[$field], $field);
}

function loc_query_id(mixed $v, string $field): int
{
    if (!is_string($v) || !ctype_digit($v) || (int) $v < 1) {
        loc_error('bad_request', "$field must be a positive integer.", 400);
    }
    return (int) $v;
}

function loc_name_value(mixed $v, int $max = 128): string
{
    if (!is_string($v)) {
        loc_error('bad_request', 'name must be a string.', 400);
    }
    $v = trim($v);
    if ($v === '' || mb_strlen($v) > $max) {
        loc_error('bad_request', "name must contain 1 to $max characters.", 400);
    }
    return $v;
}

function loc_nullable_int(mixed $v): ?int
{
    return $v === null ? null : (int) $v;
}

/** Optimistic-lock compare: accept the raw DB value or its Coerce::iso form. */
function loc_same_timestamp(string $current, string $expected): bool
{
    return $current === $expected || Coerce::iso($current) === $expected;
}

// ======================================================================
// Rows and locks
// ======================================================================

function loc_live_row(PDO $db, string $table, int $id): bool
{
    if (!in_array($table, ['projects', 'container_types'], true)) {
        return false;
    }
    $st = $db->prepare("SELECT id FROM `$table` WHERE id = :id AND deleted_at IS NULL");
    $st->execute([':id' => $id]);
    return $st->fetchColumn() !== false;
}

function loc_lock(PDO $db, int $id): ?array
{
    $st = $db->prepare('SELECT * FROM locations WHERE id = :id FOR UPDATE');
    $st->execute([':id' => $id]);
    return $st->fetch() ?: null;
}

function loc_live_location(PDO $db, int $id): ?array
{
    $st = $db->prepare('SELECT * FROM locations WHERE id = :id AND deleted_at IS NULL');
    $st->execute([':id' => $id]);
    return $st->fetch() ?: null;
}

function loc_public_location(array $r): array
{
    return [
        'id'                  => (int) $r['id'],
        'parent_id'           => loc_nullable_int($r['parent_id']),
        'name'                => $r['name'],
        'loc_type'            => $r['loc_type'],
        'code'                => $r['code'] ?? null,
        'assigned_project_id' => loc_nullable_int($r['assigned_project_id']),
    ];
}

function loc_container_type_sql(): string
{
    return 'SELECT id, name, created_at, updated_at, deleted_at FROM container_types';
}

function loc_type_row(array $r): array
{
    $r['id'] = (int) $r['id'];
    return $r;
}

/** Backtick-quote an identifier read from information_schema. */
function loc_ident(string $name): string
{
    return '`' . str_replace('`', '``', $name) . '`';
}

// ======================================================================
// Hierarchy rules
// ======================================================================

/** Walk up from $parent, locking each ancestor; refuse if $source appears. */
function loc_walk_ancestors(PDO $db, ?int $parent, int $source): void
{
    $seen  = [];
    $steps = 0;
    while ($parent !== null) {
        if ($parent === $source || isset($seen[$parent])) {
            loc_error('location_cycle', 'Move would create or traverse a parent cycle.', 409);
        }
        if (++$steps > 10000) {
            loc_error('location_cycle', 'Location ancestor chain exceeds the depth limit.', 409);
        }
        $seen[$parent] = true;
        $row = loc_lock($db, $parent);
        if ($row === null || $row['deleted_at'] !== null) {
            loc_error('not_found', 'Parent location not found.', 404);
        }
        $parent = loc_nullable_int($row['parent_id']);
    }
}

/** May a $type location sit under $parent (locked here)? */
function loc_check_transition(PDO $db, ?int $parent, string $type): void
{
    if ($parent === null) {
        if ($type !== 'property') {
            loc_error('bad_request', 'A root location must be a property.', 400);
        }
        return;
    }
    $row = loc_lock($db, $parent);
    if ($row === null || $row['deleted_at'] !== null) {
        loc_error('not_found', 'Parent location not found.', 404);
    }
    if ($type !== 'storage_unit' && (LOC_CHILD_LEVEL[$row['loc_type']] ?? null) !== $type) {
        loc_error('bad_request', 'Location level transition is invalid.', 400);
    }
}

/** Would changing location $id to $type strand any of its live children? */
function loc_check_children(PDO $db, int $id, string $type): void
{
    $st = $db->prepare('SELECT loc_type FROM locations WHERE parent_id = :id AND deleted_at IS NULL FOR UPDATE');
    $st->execute([':id' => $id]);
    foreach ($st->fetchAll(PDO::FETCH_COLUMN) as $child) {
        if ($child !== 'storage_unit' && (LOC_CHILD_LEVEL[$type] ?? null) !== $child) {
            loc_error('bad_request', 'Location level change would invalidate a child.', 400);
        }
    }
}

function loc_assert_container_empty(PDO $db, int $id): void
{
    $child = $db->prepare('SELECT id FROM locations WHERE parent_id = :id AND deleted_at IS NULL LIMIT 1 FOR UPDATE');
    $child->execute([':id' => $id]);
    $unit = $db->prepare('SELECT id FROM hardware_units WHERE location_id = :id AND deleted_at IS NULL LIMIT 1 FOR UPDATE');
    $unit->execute([':id' => $id]);
    if ($child->fetchColumn() !== false || $unit->fetchColumn() !== false) {
        loc_error('container_not_empty', 'Container has live children or hardware units.', 409);
    }
}

/** Live sibling name clash, using the same collation as uq_locations_live_sibling. */
function loc_sibling_collision(PDO $db, string $name, ?int $parent, ?int $except): bool
{
    $sql    = 'SELECT id FROM locations WHERE deleted_at IS NULL AND parent_key = :parent AND name = :name';
    $params = [':parent' => $parent ?? 0, ':name' => $name];
    if ($except !== null) {
        $sql .= ' AND id <> :except';
        $params[':except'] = $except;
    }
    $st = $db->prepare($sql . ' LIMIT 1 FOR UPDATE');
    $st->execute($params);
    return $st->fetchColumn() !== false;
}

function loc_sibling_available(PDO $db, string $name, ?int $parent, ?int $except): void
{
    if (loc_sibling_collision($db, $name, $parent, $except)) {
        loc_error('duplicate_location', 'A live sibling already has that name.', 409);
    }
}

function inv_location_container_type(mixed $v, string $type): ?int
{
    $id = loc_optional_id($v, 'container_type_id');
    if ($id !== null) {
        if ($type !== 'storage_unit') {
            loc_error('bad_request', 'container_type_id requires a storage_unit.', 400);
        }
        if (!loc_live_row(Sor::db(), 'container_types', $id)) {
            loc_error('not_found', 'Container type not found.', 404);
        }
    }
    return $id;
}

function inv_location_project(mixed $v, string $type): ?int
{
    $id = loc_optional_id($v, 'assigned_project_id');
    if ($id !== null) {
        if ($type !== 'storage_unit') {
            loc_error('bad_request', 'assigned_project_id requires a storage_unit.', 400);
        }
        if (!loc_live_row(Sor::db(), 'projects', $id)) {
            loc_error('not_found', 'Project not found.', 404);
        }
    }
    return $id;
}

function loc_audit(PDO $db, string $table, int $id, string $action, ?array $old, ?array $new, string $note, int $source): void
{
    $db->prepare(
        "INSERT INTO audit_log (table_name, row_id, action, actor_label, source_id, asserted_kind, old_values, new_values, note)
         VALUES (:table, :id, :action, :actor, :source, 'human', :old, :new, :note)"
    )->execute([
        ':table'  => $table,
        ':id'     => $id,
        ':action' => $action,
        ':actor'  => 'dashboard:' . Operator::name(),
        ':source' => $source,
        ':old'    => $old === null ? null : json_encode($old, JSON_THROW_ON_ERROR),
        ':new'    => $new === null ? null : json_encode($new, JSON_THROW_ON_ERROR),
        ':note'   => $note,
    ]);
}

// ======================================================================
// Merge support
// ======================================================================

/** Every hard FK that references locations, read live from information_schema. */
function loc_discover_fks(PDO $db): array
{
    $st = $db->prepare(
        "SELECT TABLE_NAME AS table_name, COLUMN_NAME AS column_name
           FROM information_schema.KEY_COLUMN_USAGE
          WHERE TABLE_SCHEMA = DATABASE() AND REFERENCED_TABLE_SCHEMA = DATABASE()
            AND REFERENCED_TABLE_NAME = 'locations'
          ORDER BY TABLE_NAME, COLUMN_NAME"
    );
    $st->execute();
    return $st->fetchAll();
}

/** References the merge knows how to rewrite; anything else refuses the merge. */
function loc_fk_supported(array $fk): bool
{
    return in_array($fk['table_name'] . '.' . $fk['column_name'], [
        'locations.parent_id',
        'hardware_units.location_id',
        'entities.location_id',
        'power_circuits.location_id',
        'projects.tray_location_id',
    ], true);
}

function loc_merge_counts(PDO $db, int $id, array $fks): array
{
    $counts = [];
    foreach ($fks as $fk) {
        $st = $db->prepare('SELECT COUNT(*) FROM ' . loc_ident($fk['table_name']) . ' WHERE ' . loc_ident($fk['column_name']) . ' = :id');
        $st->execute([':id' => $id]);
        $counts[$fk['table_name'] . '.' . $fk['column_name']] = (int) $st->fetchColumn();
    }
    foreach (LOC_POLYMORPHIC_REFS as $table => $sql) {
        $st = $db->prepare($sql);
        $st->execute([':id' => $id]);
        $counts[$table] = count($st->fetchAll());
    }
    return $counts;
}

function loc_lock_merge_refs(PDO $db, int $id, array $fks): void
{
    foreach ($fks as $fk) {
        $pk = loc_primary_key($db, (string) $fk['table_name']);
        if ($pk === null) {
            continue;
        }
        $st = $db->prepare('SELECT ' . loc_ident($pk) . ' FROM ' . loc_ident($fk['table_name']) . ' WHERE ' . loc_ident($fk['column_name']) . ' = :id ORDER BY ' . loc_ident($pk) . ' FOR UPDATE');
        $st->execute([':id' => $id]);
        $st->fetchAll();
    }
    foreach (LOC_POLYMORPHIC_REFS as $sql) {
        $st = $db->prepare($sql . ' FOR UPDATE');
        $st->execute([':id' => $id]);
        $st->fetchAll();
    }
}

/** Hash of both rows and every referencing row id, so any change between preview and merge is detected. */
function loc_merge_fingerprint(PDO $db, array $source, array $target, array $counts, array $fks): string
{
    $refs = [];
    foreach ($fks as $fk) {
        $pk = loc_primary_key($db, (string) $fk['table_name']);
        if ($pk === null) {
            continue;
        }
        $st = $db->prepare('SELECT ' . loc_ident($pk) . ' FROM ' . loc_ident($fk['table_name']) . ' WHERE ' . loc_ident($fk['column_name']) . ' = :id ORDER BY ' . loc_ident($pk));
        $st->execute([':id' => $source['id']]);
        $refs[$fk['table_name'] . '.' . $fk['column_name']] = array_map('intval', $st->fetchAll(PDO::FETCH_COLUMN));
    }
    foreach (LOC_POLYMORPHIC_REFS as $table => $sql) {
        $st = $db->prepare($sql);
        $st->execute([':id' => $source['id']]);
        $refs[$table] = $st->fetchAll();
    }
    $shape = static fn(array $r): array => [(int) $r['id'], $r['updated_at'], $r['loc_type'], loc_nullable_int($r['parent_id']), loc_nullable_int($r['assigned_project_id'])];
    return hash('sha256', json_encode(['source' => $shape($source), 'target' => $shape($target), 'counts' => $counts, 'refs' => $refs], JSON_THROW_ON_ERROR));
}

function loc_primary_key(PDO $db, string $table): ?string
{
    $st = $db->prepare(
        "SELECT COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE
          WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = :t AND CONSTRAINT_NAME = 'PRIMARY'
          ORDER BY ORDINAL_POSITION LIMIT 1"
    );
    $st->execute([':t' => $table]);
    $v = $st->fetchColumn();
    return $v === false ? null : (string) $v;
}

function loc_completed_merge(PDO $db, int $source, int $target): bool
{
    $st = $db->prepare(
        "SELECT id FROM audit_log
          WHERE table_name = 'locations' AND row_id = :id AND action = 'soft_delete'
            AND JSON_UNQUOTE(JSON_EXTRACT(new_values, '$.merge_target_id')) = :target
          ORDER BY id DESC LIMIT 1"
    );
    $st->execute([':id' => $source, ':target' => (string) $target]);
    return $st->fetchColumn() !== false;
}
