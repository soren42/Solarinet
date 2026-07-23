<?php
declare(strict_types=1);

/**
 * Barcode / asset-tag layer for the inventory SoR (migration 015_barcodes.sql).
 *
 * The feature word is "codes" (/api/inventory/codes/...). "scan" is RESERVED:
 * /api/inventory/receipts/{id}/scan already means receipt-photo upload/stream
 * and must not be shadowed. A "scan" in this module is a HID keyboard-wedge
 * read on the frontend; every server verb is stateless.
 *
 * ORDERING INVARIANT — LOAD-BEARING: routes.php plain-`require`s each group
 * file, and routes/inventory.php (loaded immediately BEFORE this file) defines
 * `final class Inv` plus the `inv_*` helper functions this module reuses
 * verbatim (inv_component_sql/row, inv_location_sql/row, inv_project_sql/row,
 * inv_allocation_sql/row, inv_str, inv_date, inv_optional_fk, inv_quantity,
 * inv_cents, …). This file must therefore be registered directly AFTER
 * routes/inventory.php in routes.php and must NEVER itself require inventory.php
 * — a re-require would fatally redeclare `Inv`. See routes.php $groups.
 *
 * STORAGE / PROVENANCE / SOFT-DELETE / RBAC: identical contract to
 * inventory.php — SoR is authoritative via Sor::db()/Inv:: (503 when
 * unreachable); every barcode insert stamps source_id = Sor::dashboardSourceId(),
 * asserted_kind='human', asserted_at=NOW(); every read filters deleted_at IS
 * NULL; deletes SET deleted_at = NOW(); mutations call
 * Operator::requireOperator() first.
 *
 * GET PURITY: GET handlers are pure SELECTs (read-through-cache contract). That
 * is why `lookup` is a POST (it writes barcode_scans) and why GET .../label
 * does NOT stamp label_printed_at — POST .../printed does (R12).
 */

require_once __DIR__ . '/../lib/Sor.php';
require_once __DIR__ . '/../lib/Operator.php';
require_once __DIR__ . '/../lib/Coerce.php';
require_once __DIR__ . '/../lib/Label.php';

return static function (Router $router): void {

    // ==================================================================
    // R1. GET /api/inventory/codes — list (session)
    // ==================================================================
    $router->get('/api/inventory/codes', static function (): void {
        $where  = ['deleted_at IS NULL'];
        $params = [];

        if (isset($_GET['subject_table']) && $_GET['subject_table'] !== '') {
            $st = (string) $_GET['subject_table'];
            if (!in_array($st, InvCodes::SUBJECTS, true)) {
                Response::error('bad_request', 'subject_table must be one of: ' . implode(', ', InvCodes::SUBJECTS), 400);
            }
            $where[]          = 'subject_table = :st';
            $params[':st']    = $st;
        }
        if (isset($_GET['subject_id']) && $_GET['subject_id'] !== '') {
            if (!ctype_digit((string) $_GET['subject_id'])) {
                Response::error('bad_request', 'subject_id must be an integer.', 400);
            }
            $where[]          = 'subject_id = :sid';
            $params[':sid']   = (int) $_GET['subject_id'];
        }
        if (isset($_GET['code_type']) && $_GET['code_type'] !== '') {
            $where[]          = 'code_type = :ct';
            $params[':ct']    = Inv::requireEnum('barcodes', 'code_type', $_GET['code_type'], 'code_type');
        }
        if (isset($_GET['q']) && is_string($_GET['q']) && trim($_GET['q']) !== '') {
            $where[]          = 'code LIKE :q';
            $params[':q']     = '%' . trim($_GET['q']) . '%';
        }

        $limit = Inv::clampLimit($_GET['limit'] ?? null, 500, 5000);
        $rows  = Inv::rows(
            inv_code_sql() . ' WHERE ' . implode(' AND ', $where) . " ORDER BY id DESC LIMIT $limit",
            $params
        );
        Response::ok(array_map('inv_code_row', $rows));
    });

    // ==================================================================
    // R5. POST /api/inventory/codes/lookup — resolve a scan (operator; WRITES audit)
    //     (declared before /{id} so the literal path never shadows nothing —
    //      Router matches literals fine, but keep the reads grouped logically.)
    // ==================================================================
    $router->post('/api/inventory/codes/lookup', static function (): void {
        $operator = Operator::requireOperator();
        $b        = solari_json_body();
        $code     = inv_str_required($b, 'code', 128);

        $row = Inv::row(inv_code_sql() . ' WHERE code = :c AND deleted_at IS NULL ORDER BY id DESC LIMIT 1', [':c' => $code]);
        if ($row === null) {
            inv_code_log($code, null, null, null, 'lookup', $operator, ['found' => false]);
            Response::ok([
                'found'       => false,
                'code'        => $code,
                'guessedType' => inv_guess_type($code),
            ]);
        }

        $table = (string) $row['subject_table'];
        $sid   = (int) $row['subject_id'];
        $card  = inv_subject_card($table, $sid);
        inv_code_log($code, (int) $row['id'], $table, $sid, 'lookup', $operator, ['found' => true]);
        Response::ok([
            'found'        => true,
            'barcode'      => inv_code_row($row),
            'subjectTable' => $table,
            'subject'      => $card,
        ]);
    });

    // ==================================================================
    // R6. POST /api/inventory/codes/receive — intake an unknown scanned code (operator)
    // ==================================================================
    $router->post('/api/inventory/codes/receive', static function (): void {
        $operator = Operator::requireOperator();
        $b        = solari_json_body();

        $code = inv_str_required($b, 'code', 128);

        // 1. This endpoint is only for UNKNOWN codes.
        if (Inv::row('SELECT id FROM barcodes WHERE code = :c AND deleted_at IS NULL', [':c' => $code]) !== null) {
            Response::error('conflict', 'code already in use; use lookup/move/associate instead.', 409);
        }

        // Resolve code_type: default = guess, but a prefixed guess is coerced to
        // 'custom' (receive only mints serial/upc/custom scanned codes).
        $codeType = isset($b['code_type']) && $b['code_type'] !== ''
            ? Inv::requireEnum('barcodes', 'code_type', $b['code_type'], 'code_type')
            : inv_guess_type($code);
        if (!in_array($codeType, ['serial', 'upc', 'custom'], true)) {
            $codeType = 'custom';
        }

        // 2. Create the hardware_units row exactly as POST /components does.
        $modelId = inv_optional_fk('hardware_models', $b['model_id'] ?? null, 'model_id');
        if ($modelId === null) {
            Response::error('bad_request', 'model_id is required.', 400);
        }
        $serial = inv_str($b, 'serial', 128);
        if ($codeType === 'serial' && $serial === null) {
            $serial = substr($code, 0, 128);
        }

        Inv::exec(
            'INSERT INTO hardware_units
                (model_id, serial, asset_tag, location_id, acquired_on, quantity, unit_cost_cents,
                 stock_status, receipt_id, notes, source_id, asserted_kind, asserted_at)
             VALUES
                (:model_id, :serial, NULL, :location_id, :acquired_on, :quantity, :unit_cost_cents,
                 \'in_stock\', :receipt_id, :notes, :source_id, \'human\', NOW())',
            [
                ':model_id'        => $modelId,
                ':serial'          => $serial,
                ':location_id'     => inv_optional_fk('locations', $b['location_id'] ?? null, 'location_id'),
                ':acquired_on'     => inv_date($b['acquired_on'] ?? null),
                ':quantity'        => inv_quantity($b),
                ':unit_cost_cents' => inv_cents($b, 'unit_cost_cents'),
                ':receipt_id'      => inv_optional_fk('receipts', $b['receipt_id'] ?? null, 'receipt_id'),
                ':notes'           => inv_str($b, 'notes'),
                ':source_id'       => Sor::dashboardSourceId(),
            ]
        );
        $unitId = (int) Inv::lastInsertId();

        // 3. Mint + mirror the AKO asset tag onto the unit.
        $tag = inv_next_code('AKO');
        Inv::exec('UPDATE hardware_units SET asset_tag = :tag WHERE id = :id', [':tag' => $tag, ':id' => $unitId]);

        // 4. Barcode A: the AKO asset tag.
        $tagId = inv_code_insert([
            'code'          => $tag,
            'code_type'     => 'asset_tag',
            'symbology'     => 'both',
            'subject_table' => 'hardware_units',
            'subject_id'    => $unitId,
            'notes'         => null,
            'auto'          => true,
            'prefix'        => 'AKO',
        ]);

        // 4b. inv_code_insert may have regenerated the tag on a concurrent
        // collision; re-read the stored code and re-mirror it so the unit's
        // asset_tag, the audit detail, and the log code all match the barcode.
        $stored = inv_code_stored_code($tagId);
        if ($stored !== $tag) {
            Inv::exec('UPDATE hardware_units SET asset_tag = :tag WHERE id = :id', [':tag' => $stored, ':id' => $unitId]);
            $tag = $stored;
        }

        // 5. Barcode B: the scanned code (skip only if identical to the tag).
        $scanId = null;
        if ($code !== $tag) {
            $scanId = inv_code_insert([
                'code'          => $code,
                'code_type'     => $codeType,
                'symbology'     => 'both',
                'subject_table' => 'hardware_units',
                'subject_id'    => $unitId,
                'notes'         => inv_str($b, 'notes'),
                'auto'          => false,
                'prefix'        => null,
            ]);
        }

        // 6. Audit against barcode B (or A if B was skipped).
        inv_code_log(
            $scanId !== null ? $code : $tag,
            $scanId ?? $tagId,
            'hardware_units',
            $unitId,
            'receive',
            $operator,
            ['unitId' => $unitId, 'modelId' => $modelId, 'assetTag' => $tag]
        );

        $unitRow  = inv_component_row(Inv::row(inv_component_sql() . ' WHERE hu.id = :i', [':i' => $unitId]));
        $tagRow   = inv_code_row(Inv::row(inv_code_sql() . ' WHERE id = :i', [':i' => $tagId]));
        $scanRow  = $scanId !== null ? inv_code_row(Inv::row(inv_code_sql() . ' WHERE id = :i', [':i' => $scanId])) : null;
        Response::ok([
            'unit'         => $unitRow,
            'assetTag'     => $tagRow,
            'scannedCode'  => $scanRow,
        ], 201);
    });

    // ==================================================================
    // R7. POST /api/inventory/codes/move — move a unit to a location (operator)
    // ==================================================================
    $router->post('/api/inventory/codes/move', static function (): void {
        $operator = Operator::requireOperator();
        $b        = solari_json_body();

        $unitId = inv_optional_fk('hardware_units', $b['hardware_unit_id'] ?? null, 'hardware_unit_id');
        if ($unitId === null) {
            Response::error('bad_request', 'hardware_unit_id is required.', 400);
        }
        $locId = inv_optional_fk('locations', $b['location_id'] ?? null, 'location_id');
        if ($locId === null) {
            Response::error('bad_request', 'location_id is required.', 400);
        }

        $cur  = Inv::row('SELECT location_id FROM hardware_units WHERE id = :i AND deleted_at IS NULL', [':i' => $unitId]);
        $from = $cur !== null ? Coerce::int($cur['location_id']) : null;

        Inv::exec('UPDATE hardware_units SET location_id = :loc WHERE id = :id', [':loc' => $locId, ':id' => $unitId]);

        inv_code_log(
            inv_unit_code_label($unitId),
            null,
            'hardware_units',
            $unitId,
            'move',
            $operator,
            ['fromLocationId' => $from, 'toLocationId' => $locId]
        );

        Response::ok(inv_component_row(Inv::row(inv_component_sql() . ' WHERE hu.id = :i', [':i' => $unitId])));
    });

    // ==================================================================
    // R8. POST /api/inventory/codes/associate — project association (operator)
    // ==================================================================
    $router->post('/api/inventory/codes/associate', static function (): void {
        $operator  = Operator::requireOperator();
        $b         = solari_json_body();

        $projectId = inv_optional_fk('projects', $b['project_id'] ?? null, 'project_id');
        if ($projectId === null) {
            Response::error('bad_request', 'project_id is required.', 400);
        }

        $hasLoc  = isset($b['location_id']) && $b['location_id'] !== null && $b['location_id'] !== '';
        $hasUnit = isset($b['hardware_unit_id']) && $b['hardware_unit_id'] !== null && $b['hardware_unit_id'] !== '';
        if ($hasLoc === $hasUnit) {
            Response::error('bad_request', 'Exactly one of location_id or hardware_unit_id is required.', 400);
        }

        // ---- Bin variant: attach a bin/location to the project. ----------
        if ($hasLoc) {
            $locId = inv_optional_fk('locations', $b['location_id'], 'location_id');
            $proj  = Inv::row('SELECT id, tray_location_id FROM projects WHERE id = :i AND deleted_at IS NULL', [':i' => $projectId]);
            $tray  = $proj !== null ? Coerce::int($proj['tray_location_id']) : null;

            if ($tray === null) {
                // First bin becomes the project tray.
                Inv::exec('UPDATE projects SET tray_location_id = :loc WHERE id = :id', [':loc' => $locId, ':id' => $projectId]);
                $mode = 'tray';
            } else {
                // Reparent the bin under the tray, guarding against a cycle.
                if ($locId === $tray) {
                    Response::error('bad_request', 'The tray cannot be nested under itself.', 400);
                }
                inv_guard_no_cycle($tray, $locId);
                Inv::exec('UPDATE locations SET parent_id = :tray WHERE id = :loc', [':tray' => $tray, ':loc' => $locId]);
                $mode = 'nested';
            }

            inv_code_log(
                inv_loc_code_label($locId),
                null,
                'locations',
                $locId,
                'associate',
                $operator,
                ['projectId' => $projectId, 'locationId' => $locId, 'mode' => $mode]
            );

            Response::ok([
                'project'  => inv_project_row(Inv::row(inv_project_sql() . ' WHERE id = :i', [':i' => $projectId])),
                'location' => inv_location_row(Inv::row(inv_location_sql() . ' WHERE id = :i', [':i' => $locId])),
                'mode'     => $mode,
            ]);
        }

        // ---- Unit variant: commit a unit to the project. -----------------
        $unitId = inv_optional_fk('hardware_units', $b['hardware_unit_id'], 'hardware_unit_id');
        // The DB's double-commit trigger SIGNALs 45000 → Inv::exec → 409, and
        // trg_alloc_tray_move (AFTER INSERT) parks the unit on the tray + flips
        // stock_status, so no manual mirror is needed here.
        Inv::exec(
            'INSERT INTO project_allocations (project_id, hardware_unit_id, allocation, quantity, source_id)
             VALUES (:pid, :uid, \'committed\', 1, :src)',
            [':pid' => $projectId, ':uid' => $unitId, ':src' => Sor::dashboardSourceId()]
        );
        $allocId = (int) Inv::lastInsertId();

        inv_code_log(
            inv_unit_code_label($unitId),
            null,
            'hardware_units',
            $unitId,
            'allocate',
            $operator,
            ['projectId' => $projectId, 'hardwareUnitId' => $unitId, 'allocationId' => $allocId]
        );

        Response::ok(inv_allocation_row(Inv::row(inv_allocation_sql() . ' WHERE pa.id = :i', [':i' => $allocId])), 201);
    });

    // ==================================================================
    // R13. GET /api/inventory/codes/history — audit read (session)
    //      (literal path; declared before /{id} routes for clarity)
    // ==================================================================
    $router->get('/api/inventory/codes/history', static function (): void {
        $where  = [];
        $params = [];
        if (isset($_GET['code']) && is_string($_GET['code']) && $_GET['code'] !== '') {
            $where[]        = 'code = :c';
            $params[':c']   = substr(trim((string) $_GET['code']), 0, 128);
        }
        if (isset($_GET['subject_table']) && $_GET['subject_table'] !== '') {
            $st = (string) $_GET['subject_table'];
            if (!in_array($st, InvCodes::SUBJECTS, true)) {
                Response::error('bad_request', 'subject_table must be one of: ' . implode(', ', InvCodes::SUBJECTS), 400);
            }
            $where[]        = 'subject_table = :st';
            $params[':st']  = $st;
        }
        if (isset($_GET['subject_id']) && $_GET['subject_id'] !== '') {
            if (!ctype_digit((string) $_GET['subject_id'])) {
                Response::error('bad_request', 'subject_id must be an integer.', 400);
            }
            $where[]        = 'subject_id = :sid';
            $params[':sid'] = (int) $_GET['subject_id'];
        }
        if (isset($_GET['action']) && $_GET['action'] !== '') {
            $where[]        = 'action = :a';
            $params[':a']   = Inv::requireEnum('barcode_scans', 'action', $_GET['action'], 'action');
        }

        $limit  = Inv::clampLimit($_GET['limit'] ?? null, 100, 1000);
        $clause = $where === [] ? '' : ' WHERE ' . implode(' AND ', $where);
        $rows   = Inv::rows(inv_scanlog_sql() . $clause . " ORDER BY id DESC LIMIT $limit", $params);
        Response::ok(array_map('inv_scanlog_row', $rows));
    });

    // ==================================================================
    // R3. POST /api/inventory/codes — create/assign a code (operator)
    // ==================================================================
    $router->post('/api/inventory/codes', static function (): void {
        Operator::requireOperator();
        $b        = solari_json_body();
        $operator = Operator::name();

        if (!isset($b['code_type']) || $b['code_type'] === '') {
            Response::error('bad_request', 'code_type is required.', 400);
        }
        $codeType = Inv::requireEnum('barcodes', 'code_type', $b['code_type'], 'code_type');

        $symbology = isset($b['symbology']) && $b['symbology'] !== ''
            ? Inv::requireEnum('barcodes', 'symbology', $b['symbology'], 'symbology')
            : 'both';

        [$table, $subjectId] = inv_code_subject($b);

        // Resolve the code: explicit (validated) or auto-generated from prefix.
        $auto   = false;
        $prefix = null;
        $suppliedRaw = $b['code'] ?? null;
        if ($suppliedRaw === null || (is_string($suppliedRaw) && trim($suppliedRaw) === '')) {
            $prefix = InvCodes::PREFIX[$codeType] ?? null;
            if ($prefix === null) {
                Response::error('bad_request', "code is required for code_type '$codeType' (no auto-prefix).", 400);
            }
            $code = inv_next_code($prefix);
            $auto = true;
        } else {
            $code = inv_str_required($b, 'code', 128);
        }

        // asset_tag codes are mirrored into hardware_units.asset_tag VARCHAR(64);
        // reject over-long caller-supplied codes up front rather than 500'ing on
        // a 22001 truncation AFTER the barcode row is already written.
        if ($codeType === 'asset_tag' && strlen($code) > 64) {
            Response::error('bad_request', 'asset_tag codes must be at most 64 characters.', 400);
        }

        $id = inv_code_insert([
            'code'          => $code,
            'code_type'     => $codeType,
            'symbology'     => $symbology,
            'subject_table' => $table,
            'subject_id'    => $subjectId,
            'notes'         => inv_str($b, 'notes', 255),
            'auto'          => $auto,
            'prefix'        => $prefix,
        ]);

        // Mirror an AKO asset tag onto the unit's asset_tag when appropriate.
        if ($table === 'hardware_units' && $codeType === 'asset_tag') {
            $stored = inv_code_stored_code($id);
            Inv::exec(
                "UPDATE hardware_units SET asset_tag = :tag WHERE id = :id AND (asset_tag IS NULL OR asset_tag = '')",
                [':tag' => $stored, ':id' => $subjectId]
            );
        }

        inv_code_log(inv_code_stored_code($id), $id, $table, $subjectId, 'create', $operator, [
            'subjectTable' => $table,
            'subjectId'    => $subjectId,
            'codeType'     => $codeType,
        ]);

        Response::ok(inv_code_row(Inv::row(inv_code_sql() . ' WHERE id = :i', [':i' => $id])), 201);
    });

    // ==================================================================
    // R11. GET /api/inventory/codes/{id}/label — stream the rendered label (session)
    //      Bypasses the JSON envelope (raw bytes), like receipts/{id}/scan.
    // ==================================================================
    $router->get('/api/inventory/codes/{id}/label', static function (array $p): void {
        $id  = Inv::idParam($p['id']);
        $row = Inv::row(inv_code_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($row === null) {
            Response::error('not_found', "No code $id", 404);
        }

        $format = (isset($_GET['format']) && $_GET['format'] === 'pdf') ? 'pdf' : 'png';
        $prof   = Label::profile(isset($_GET['profile']) ? (string) $_GET['profile'] : 'default');
        $w      = inv_clamp_dots($_GET['width_dots']  ?? null, $prof['width']);
        $h      = inv_clamp_dots($_GET['height_dots'] ?? null, $prof['height']);
        $dpi    = inv_clamp_dpi($_GET['dpi'] ?? null, $prof['dpi']);

        $code  = (string) $row['code'];
        $lines = inv_code_label_lines($row);
        $sym   = (string) $row['symbology'];

        try {
            $bytes = Label::render($code, $lines, $sym, $w, $h, $dpi, $format);
        } catch (LabelUnavailable $e) {
            Response::error('label_renderer_unavailable', $e->getMessage(), 503);
        } catch (\Throwable $e) {
            error_log('label render failed for code ' . $id . ': ' . $e->getMessage());
            Response::error('internal_error', 'Label rendering failed.', 500);
        }

        $mime = $format === 'pdf' ? 'application/pdf' : 'image/png';
        $ext  = $format === 'pdf' ? 'pdf' : 'png';
        $safe = preg_replace('/[^A-Za-z0-9._-]+/', '_', $code) ?: 'label';
        if (!headers_sent()) {
            http_response_code(200);
            header('Content-Type: ' . $mime);
            header('Content-Length: ' . (string) strlen($bytes));
            header('Cache-Control: private, max-age=0, no-cache');
            header('X-Content-Type-Options: nosniff');
            header('Content-Disposition: inline; filename="label-' . $safe . '.' . $ext . '"');
        }
        echo $bytes;
        exit;
    });

    // ==================================================================
    // R12. POST /api/inventory/codes/{id}/printed — stamp label_printed_at (operator)
    // ==================================================================
    $router->post('/api/inventory/codes/{id}/printed', static function (array $p): void {
        $operator = Operator::requireOperator();
        $id       = Inv::idParam($p['id']);
        $row      = Inv::row('SELECT id, code, subject_table, subject_id FROM barcodes WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($row === null) {
            Response::error('not_found', "No code $id", 404);
        }
        Inv::exec('UPDATE barcodes SET label_printed_at = NOW() WHERE id = :i', [':i' => $id]);
        inv_code_log((string) $row['code'], $id, (string) $row['subject_table'], (int) $row['subject_id'], 'print', $operator, []);
        Response::ok(inv_code_row(Inv::row(inv_code_sql() . ' WHERE id = :i', [':i' => $id])));
    });

    // ==================================================================
    // R2. GET /api/inventory/codes/{id} — one code + its subject card (session)
    // ==================================================================
    $router->get('/api/inventory/codes/{id}', static function (array $p): void {
        $id  = Inv::idParam($p['id']);
        $row = Inv::row(inv_code_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($row === null) {
            Response::error('not_found', "No code $id", 404);
        }
        $card = inv_subject_card((string) $row['subject_table'], (int) $row['subject_id']);
        Response::ok([
            'barcode' => inv_code_row($row),
            'subject' => $card,
        ]);
    });

    // ==================================================================
    // R4. DELETE /api/inventory/codes/{id} — soft-delete (operator)
    // ==================================================================
    $router->add('DELETE', '/api/inventory/codes/{id}', static function (array $p): void {
        $operator = Operator::requireOperator();
        $id       = Inv::idParam($p['id']);
        $row      = Inv::row('SELECT id, code, subject_table, subject_id FROM barcodes WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($row === null) {
            Response::error('not_found', "No code $id", 404);
        }
        Inv::exec('UPDATE barcodes SET deleted_at = NOW() WHERE id = :i', [':i' => $id]);
        inv_code_log((string) $row['code'], $id, (string) $row['subject_table'], (int) $row['subject_id'], 'delete', $operator, []);
        Response::ok(['id' => $id, 'status' => 'deleted']);
    });

    // ==================================================================
    // R9. GET /api/inventory/locations/{id}/contents — subtree rollup (session)
    // ==================================================================
    $router->get('/api/inventory/locations/{id}/contents', static function (array $p): void {
        $id  = Inv::idParam($p['id']);
        $loc = Inv::row(inv_location_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($loc === null) {
            Response::error('not_found', "No location $id", 404);
        }

        $cte   = inv_loc_subtree_cte();
        $locs  = Inv::rows(
            $cte . inv_location_sql() . ' WHERE id IN (SELECT id FROM loc_tree) AND deleted_at IS NULL ORDER BY name',
            [':root' => $id]
        );
        $units = Inv::rows(
            $cte . inv_component_sql()
                . ' WHERE hu.location_id IN (SELECT id FROM loc_tree) AND hu.deleted_at IS NULL ORDER BY hu.id DESC',
            [':root' => $id]
        );

        $unitRows   = array_map('inv_component_row', $units);
        $valueCents = 0;
        foreach ($unitRows as $u) {
            $valueCents += (int) ($u['unitCostCents'] ?? 0) * (int) ($u['quantity'] ?? 0);
        }

        Response::ok([
            'location'  => inv_location_row($loc),
            'path'      => inv_loc_path($id),
            'locations' => array_map('inv_location_row', $locs),
            'units'     => $unitRows,
            'unitCount' => count($unitRows),
            'valueCents' => $valueCents,
        ]);
    });

    // ==================================================================
    // R10. GET /api/inventory/projects/{id}/rollup — full project rollup (session)
    // ==================================================================
    $router->get('/api/inventory/projects/{id}/rollup', static function (array $p): void {
        $id   = Inv::idParam($p['id']);
        $proj = Inv::row(inv_project_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($proj === null) {
            Response::error('not_found', "No project $id", 404);
        }

        $trayId  = Coerce::int($proj['tray_location_id']);
        $trayRow = null;
        $trayContents = [];
        if ($trayId !== null) {
            $trayRow = Inv::row(inv_location_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $trayId]);
            if ($trayRow !== null) {
                $cte   = inv_loc_subtree_cte();
                $units = Inv::rows(
                    $cte . inv_component_sql()
                        . ' WHERE hu.location_id IN (SELECT id FROM loc_tree) AND hu.deleted_at IS NULL ORDER BY hu.id DESC',
                    [':root' => $trayId]
                );
                $trayContents = array_map('inv_component_row', $units);
            }
        }

        $allocs = Inv::rows(
            inv_allocation_sql() . ' WHERE pa.project_id = :i AND pa.deleted_at IS NULL ORDER BY pa.id',
            [':i' => $id]
        );

        Response::ok([
            'project'      => inv_project_row($proj),
            'tray'         => $trayRow !== null ? inv_location_row($trayRow) : null,
            'trayContents' => $trayContents,
            'allocations'  => array_map('inv_allocation_row', $allocs),
        ]);
    });
};

/* ========================================================================
 * InvCodes — shared constants + subject validation for the barcode routes.
 * (Storage/SQL reuse Inv:: and the inv_* helpers from routes/inventory.php,
 * which routes.php loads immediately before this file.)
 * ==================================================================== */
final class InvCodes
{
    /** Polymorphic subjects a barcode may bind to (mirrors external_refs; API-enforced). */
    public const SUBJECTS = ['hardware_units', 'locations', 'projects', 'interfaces'];

    /**
     * code_type → auto-generated tag prefix (types absent here require an
     * explicit code). MUST be a class constant, not a top-level `const`: this
     * file `return`s its route closure before its tail, so a runtime top-level
     * const statement here would never execute.
     */
    public const PREFIX = [
        'asset_tag' => 'AKO',
        'location'  => 'LOC',
        'project'   => 'PRJ',
    ];
}

/**
 * Validate subject_table ∈ SUBJECTS and that subject_id names a LIVE row in it.
 * Mirrors external_refs' polymorphic pattern (no FK; validated in the API).
 *
 * @param array<string,mixed> $b
 * @return array{0:string,1:int} [table, id]
 */
function inv_code_subject(array $b): array
{
    $table = isset($b['subject_table']) ? (string) $b['subject_table'] : '';
    if (!in_array($table, InvCodes::SUBJECTS, true)) {
        Response::error('bad_request', 'subject_table must be one of: ' . implode(', ', InvCodes::SUBJECTS), 400);
    }
    $raw = $b['subject_id'] ?? null;
    if ($raw === null || $raw === '' || !ctype_digit((string) $raw)) {
        Response::error('bad_request', 'subject_id must be a positive integer.', 400);
    }
    $id = (int) $raw;
    // $table is whitelisted above, so this interpolation is safe.
    if (Inv::row("SELECT id FROM $table WHERE id = :i AND deleted_at IS NULL", [':i' => $id]) === null) {
        Response::error('bad_request', "No live $table row $id", 400);
    }
    return [$table, $id];
}

/** SELECT fragment for barcodes → inv_code_row(). */
function inv_code_sql(): string
{
    return 'SELECT id, code, code_type, symbology, subject_table, subject_id, label_printed_at,
                   notes, source_id, created_at, updated_at
              FROM barcodes';
}

/** barcodes row → wire shape (camelCase + a stable label URL). */
function inv_code_row(array $r): array
{
    return [
        'id'             => (int) $r['id'],
        'code'           => $r['code'],
        'codeType'       => $r['code_type'],
        'symbology'      => $r['symbology'],
        'subjectTable'   => $r['subject_table'],
        'subjectId'      => (int) $r['subject_id'],
        'labelPrintedAt' => Coerce::iso($r['label_printed_at'] ?? null),
        'notes'          => $r['notes'] ?? null,
        'sourceId'       => Coerce::int($r['source_id']),
        'createdAt'      => Coerce::iso($r['created_at'] ?? null),
        'updatedAt'      => Coerce::iso($r['updated_at'] ?? null),
        'labelUrl'       => '/api/inventory/codes/' . (int) $r['id'] . '/label',
    ];
}

/** SELECT fragment for barcode_scans → inv_scanlog_row(). */
function inv_scanlog_sql(): string
{
    return 'SELECT id, code, barcode_id, subject_table, subject_id, action, operator, detail, created_at
              FROM barcode_scans';
}

/** barcode_scans row → wire shape. */
function inv_scanlog_row(array $r): array
{
    return [
        'id'           => (int) $r['id'],
        'code'         => $r['code'],
        'barcodeId'    => Coerce::int($r['barcode_id']),
        'subjectTable' => $r['subject_table'] ?? null,
        'subjectId'    => Coerce::int($r['subject_id']),
        'action'       => $r['action'],
        'operator'     => $r['operator'],
        'detail'       => Coerce::json($r['detail'] ?? null),
        'createdAt'    => Coerce::iso($r['created_at'] ?? null),
    ];
}

/**
 * Append one row to the barcode_scans audit log. BEST-EFFORT: an audit hiccup
 * must never fail the primary action, so failures are swallowed + error_log'd.
 *
 * @param array<string,mixed> $detail JSON context for history/undo
 */
function inv_code_log(string $code, ?int $barcodeId, ?string $table, ?int $subjectId, string $action, string $operator, array $detail): void
{
    try {
        $st = Sor::db()->prepare(
            'INSERT INTO barcode_scans (code, barcode_id, subject_table, subject_id, action, operator, detail, source_id)
             VALUES (:code, :bid, :st, :sid, :action, :op, :detail, :src)'
        );
        $st->execute([
            ':code'   => substr($code, 0, 128),
            ':bid'    => $barcodeId,
            ':st'     => $table,
            ':sid'    => $subjectId,
            ':action' => $action,
            ':op'     => substr($operator, 0, 64),
            ':detail' => $detail === [] ? null : json_encode($detail, JSON_UNESCAPED_SLASHES),
            ':src'    => Sor::dashboardSourceId(),
        ]);
    } catch (\Throwable $e) {
        error_log('inv_code_log soft-fail (' . $action . ' ' . $code . '): ' . $e->getMessage());
    }
}

/**
 * Next PREFIX-NNNN tag (4-digit zero-pad, widens naturally past 9999). Scans
 * BOTH barcodes.code (incl. soft-deleted — tags are never reused) AND
 * pre-existing hardware_units.asset_tag values so hand-entered tags never
 * collide. Substring offset LENGTH(:p)+2 skips "PREFIX-".
 */
function inv_next_code(string $prefix): string
{
    $n = (int) Inv::scalar(
        'SELECT GREATEST(
                  COALESCE((SELECT MAX(CAST(SUBSTRING(code, LENGTH(:p1) + 2) AS UNSIGNED))
                              FROM barcodes WHERE code LIKE CONCAT(:p2, \'-%\')), 0),
                  COALESCE((SELECT MAX(CAST(SUBSTRING(asset_tag, LENGTH(:p3) + 2) AS UNSIGNED))
                              FROM hardware_units WHERE asset_tag LIKE CONCAT(:p4, \'-%\')), 0)
                ) + 1',
        [':p1' => $prefix, ':p2' => $prefix, ':p3' => $prefix, ':p4' => $prefix]
    );
    return $prefix . '-' . str_pad((string) $n, 4, '0', STR_PAD_LEFT);
}

/**
 * Insert a barcode row with a RAW prepared statement (NOT Inv::exec, which
 * responds-and-exits on 23000 and would break the retry loop). On a unique
 * collision (SQLSTATE 23000): if the code was auto-generated, regenerate via
 * inv_next_code and retry (max 3); if caller-supplied, 409 immediately.
 * Stamps provenance. Returns the new barcode id.
 *
 * @param array{code:string,code_type:string,symbology:string,subject_table:string,subject_id:int,notes:?string,auto:bool,prefix:?string} $f
 */
function inv_code_insert(array $f): int
{
    $sql = 'INSERT INTO barcodes
                (code, code_type, symbology, subject_table, subject_id, notes,
                 source_id, asserted_kind, asserted_at)
            VALUES
                (:code, :ct, :sym, :st, :sid, :notes, :src, \'human\', NOW())';
    $code     = $f['code'];
    $auto     = !empty($f['auto']);
    $attempts = 0;

    while (true) {
        $attempts++;
        try {
            $st = Sor::db()->prepare($sql);
            $st->execute([
                ':code'  => $code,
                ':ct'    => $f['code_type'],
                ':sym'   => $f['symbology'],
                ':st'    => $f['subject_table'],
                ':sid'   => $f['subject_id'],
                ':notes' => $f['notes'] ?? null,
                ':src'   => Sor::dashboardSourceId(),
            ]);
            return (int) Sor::db()->lastInsertId();
        } catch (\PDOException $e) {
            if ((string) $e->getCode() !== '23000') {
                throw $e;
            }
            if (!$auto) {
                Response::error('conflict', 'code already in use.', 409);
            }
            if ($attempts >= 3) {
                Response::error('conflict', 'Could not allocate a unique tag after 3 attempts.', 409);
            }
            $code = inv_next_code((string) ($f['prefix'] ?? 'AKO'));
        }
    }
}

/** The code string actually stored for a barcode id (post auto-regen). */
function inv_code_stored_code(int $id): string
{
    return (string) Inv::scalar('SELECT code FROM barcodes WHERE id = :i', [':i' => $id]);
}

/** Reverse heuristic: classify a raw scanned code by its shape. */
function inv_guess_type(string $code): string
{
    $c = trim($code);
    if (preg_match('/^AKO-/i', $c) === 1) {
        return 'asset_tag';
    }
    if (preg_match('/^LOC-/i', $c) === 1) {
        return 'location';
    }
    if (preg_match('/^PRJ-/i', $c) === 1) {
        return 'project';
    }
    if (preg_match('/^\d{11,14}$/', $c) === 1) {
        return 'upc';
    }
    return 'custom';
}

/**
 * Recursive subtree CTE rooted at :root (self + descendants, depth ≤ 16 to cap
 * accidental cycles). Prefix onto inv_component_sql()/inv_location_sql() —
 * "WITH RECURSIVE loc_tree AS (...) SELECT ...".
 */
function inv_loc_subtree_cte(): string
{
    return 'WITH RECURSIVE loc_tree AS ('
         . ' SELECT id, 0 AS depth FROM locations WHERE id = :root AND deleted_at IS NULL'
         . ' UNION ALL'
         . ' SELECT l.id, t.depth + 1 FROM locations l JOIN loc_tree t ON l.parent_id = t.id'
         . ' WHERE l.deleted_at IS NULL AND t.depth < 16'
         . ') ';
}

/** Ancestor names (root → leaf) joined with " › ", ≤ 16 hops. '' if unknown. */
function inv_loc_path(int $id): string
{
    $rows = Inv::rows(
        'WITH RECURSIVE anc AS ('
      . '  SELECT id, parent_id, name, 0 AS depth FROM locations WHERE id = :id AND deleted_at IS NULL'
      . '  UNION ALL'
      . '  SELECT l.id, l.parent_id, l.name, a.depth + 1 FROM locations l JOIN anc a ON l.id = a.parent_id'
      . '   WHERE l.deleted_at IS NULL AND a.depth < 16'
      . ') SELECT name FROM anc ORDER BY depth DESC',
        [':id' => $id]
    );
    $names = array_map(static fn (array $r): string => (string) $r['name'], $rows);
    return implode(' › ', $names);
}

/** Cycle guard: 409 if $candidate appears on the parent chain above $start (≤ 32 hops). */
function inv_guard_no_cycle(int $start, int $candidate): void
{
    $cur   = $start;
    $seen  = 0;
    while ($cur !== null && $seen < 32) {
        if ($cur === $candidate) {
            Response::error('conflict', 'would create a location cycle.', 409);
        }
        $row = Inv::row('SELECT parent_id FROM locations WHERE id = :i AND deleted_at IS NULL', [':i' => $cur]);
        $cur = ($row !== null) ? Coerce::int($row['parent_id']) : null;
        $seen++;
    }
}

/** The unit's live asset-tag barcode string, else its id as a string (for audit code col). */
function inv_unit_code_label(int $unitId): string
{
    $c = Inv::scalar(
        "SELECT code FROM barcodes
          WHERE subject_table = 'hardware_units' AND subject_id = :i
            AND code_type = 'asset_tag' AND deleted_at IS NULL
          ORDER BY id DESC LIMIT 1",
        [':i' => $unitId]
    );
    return $c !== null ? (string) $c : (string) $unitId;
}

/** A location's live 'location' barcode string, else its id as a string (for audit code col). */
function inv_loc_code_label(int $locId): string
{
    $c = Inv::scalar(
        "SELECT code FROM barcodes
          WHERE subject_table = 'locations' AND subject_id = :i
            AND code_type = 'location' AND deleted_at IS NULL
          ORDER BY id DESC LIMIT 1",
        [':i' => $locId]
    );
    return $c !== null ? (string) $c : (string) $locId;
}

/**
 * The "card" payload for a barcode's subject; null if the subject row has since
 * been soft-deleted. Reuses the inventory.php row mappers verbatim.
 *
 * @return array<string,mixed>|null
 */
function inv_subject_card(string $table, int $id): ?array
{
    switch ($table) {
        case 'hardware_units':
            $row = Inv::row(inv_component_sql() . ' WHERE hu.id = :i AND hu.deleted_at IS NULL', [':i' => $id]);
            if ($row === null) {
                return null;
            }
            $card = inv_component_row($row);
            $card['codes'] = array_map(
                'inv_code_row',
                Inv::rows(inv_code_sql() . ' WHERE subject_table = \'hardware_units\' AND subject_id = :i AND deleted_at IS NULL ORDER BY id', [':i' => $id])
            );
            $proj = Inv::row(
                "SELECT p.id, p.name
                   FROM project_allocations pa
                   JOIN projects p ON p.id = pa.project_id AND p.deleted_at IS NULL
                  WHERE pa.hardware_unit_id = :i AND pa.deleted_at IS NULL
                    AND pa.allocation IN ('committed', 'installed')
                  ORDER BY pa.id DESC LIMIT 1",
                [':i' => $id]
            );
            $card['committedProject'] = $proj !== null ? ['id' => (int) $proj['id'], 'name' => $proj['name']] : null;
            return $card;

        case 'locations':
            $row = Inv::row(inv_location_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
            if ($row === null) {
                return null;
            }
            $card = inv_location_row($row);
            $card['path']      = inv_loc_path($id);
            $card['unitCount'] = (int) Inv::scalar('SELECT COUNT(*) FROM hardware_units WHERE location_id = :i AND deleted_at IS NULL', [':i' => $id]);
            return $card;

        case 'projects':
            $row = Inv::row(inv_project_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
            if ($row === null) {
                return null;
            }
            return inv_project_row($row);

        case 'interfaces':
            $row = Inv::row('SELECT id, entity_id, name, if_kind, mac FROM interfaces WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
            if ($row === null) {
                return null;
            }
            return [
                'id'       => (int) $row['id'],
                'entityId' => Coerce::int($row['entity_id']),
                'name'     => $row['name'] ?? null,
                'ifKind'   => $row['if_kind'] ?? null,
                'mac'      => $row['mac'] ?? null,
            ];
    }
    return null;
}

/**
 * Human-readable label text lines for a barcode row's subject.
 *
 * @param array<string,mixed> $codeRow a raw barcodes SELECT row
 * @return array{0:string,1:string,2:string} [line1, line2, line3]
 */
function inv_code_label_lines(array $codeRow): array
{
    $table = (string) $codeRow['subject_table'];
    $id    = (int) $codeRow['subject_id'];
    $code  = (string) $codeRow['code'];
    $line1 = '';
    $line2 = '';

    switch ($table) {
        case 'hardware_units':
            $u = Inv::row(inv_component_sql() . ' WHERE hu.id = :i', [':i' => $id]);
            if ($u !== null) {
                $line1 = trim(((string) ($u['make'] ?? '')) . ' ' . ((string) ($u['model'] ?? '')));
                $line2 = $u['serial'] !== null && $u['serial'] !== '' ? (string) $u['serial'] : (string) ($u['hw_type'] ?? '');
            }
            break;
        case 'locations':
            $l = Inv::row(inv_location_sql() . ' WHERE id = :i', [':i' => $id]);
            if ($l !== null) {
                $line1 = (string) $l['name'];
                $line2 = trim(((string) $l['loc_type']) . ($l['code'] ? ' · ' . (string) $l['code'] : ''));
            }
            break;
        case 'projects':
            $pr = Inv::row(inv_project_sql() . ' WHERE id = :i', [':i' => $id]);
            if ($pr !== null) {
                $line1 = (string) $pr['name'];
                $line2 = (string) ($pr['code'] ?? '');
            }
            break;
        case 'interfaces':
            $if = Inv::row('SELECT name, if_kind FROM interfaces WHERE id = :i', [':i' => $id]);
            if ($if !== null) {
                $line1 = (string) ($if['name'] ?? '');
                $line2 = (string) ($if['if_kind'] ?? '');
            }
            break;
    }
    if ($line1 === '') {
        $line1 = $code;
    }
    return [$line1, $line2, $code];
}

/** Clamp a label dot-dimension override to [64,2048]; fall back to $default. */
function inv_clamp_dots($raw, int $default): int
{
    if ($raw === null || $raw === '' || !ctype_digit((string) $raw)) {
        return $default;
    }
    return max(64, min(2048, (int) $raw));
}

/** Clamp a DPI override to {203,300}; fall back to $default. */
function inv_clamp_dpi($raw, int $default): int
{
    if ($raw === null || $raw === '' || !ctype_digit((string) $raw)) {
        return $default;
    }
    $n = (int) $raw;
    return in_array($n, [203, 300], true) ? $n : $default;
}
