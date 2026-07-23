<?php
declare(strict_types=1);

/**
 * Inventory management API — components, models, locations, receipts,
 * projects, project allocations, and the cross-project shopping list.
 *
 * STORAGE: unlike every other route file, this one does NOT read/write the
 * local SOLARI_DB (lib/Db.php). Inventory lives entirely in the SoR (MariaDB
 * `sor` on cesium, migration 014) — the local monitoring DB has no inventory
 * tables at all. All reads/writes go through Sor::db() (lib/Sor.php), a PDO
 * handle onto that database. Because the SoR is authoritative here (not a
 * best-effort mirror the way Sor::upsertHost()/removeHost() are for other
 * routes), a missing/unreachable SOR_DB_* is a real 503, not a swallowed
 * no-op — see Sor::db().
 *
 * PROVENANCE: every insert stamps source_id = Sor::dashboardSourceId() (the
 * fixed 'dashboard' source, id 2) — see the migration note in Sor.php.
 *
 * SOFT DELETE: every table here (hardware_units, hardware_models, locations,
 * receipts, projects, project_allocations) carries deleted_at; every read
 * filters deleted_at IS NULL and every delete endpoint SETs it rather than
 * removing the row.
 *
 * ENUM VALIDATION: hw_type/stock_status/loc_type/status/allocation are all
 * MariaDB ENUMs. Rather than hardcode the member lists here (which would
 * silently drift from the schema), Inv::requireEnum() reads the live member
 * list from information_schema.COLUMNS on first use and validates against
 * that — the schema is the single source of truth.
 *
 * READ-THROUGH-CACHE-READY (design-forward, not built here): a caching layer
 * is planned to sit between the SoR and the dashboard. Every GET handler below
 * is a pure SELECT with no write, no per-request side effect, and an output
 * shape determined solely by its query params — so it is safe to memoize by
 * (path, query string) whenever that layer lands. Do not add such side
 * effects to a GET handler.
 *
 * RBAC: every mutating endpoint (POST/PUT/DELETE) calls
 * Operator::requireOperator() first; every read is open to any authenticated
 * session (the front controller's session gate already applies to all of
 * /api/*).
 *
 * Routes:
 *   GET    /api/inventory/components            list (filters: stock_status,
 *                                                location_id, model_id, q, limit)
 *   GET    /api/inventory/components/{id}        one component
 *   POST   /api/inventory/components              create
 *   PUT    /api/inventory/components/{id}         update (partial)
 *   DELETE /api/inventory/components/{id}         soft-delete
 *
 *   GET    /api/inventory/models                 list (filters: hw_type, q)
 *   GET    /api/inventory/models/{id}             one model
 *   POST   /api/inventory/models                  create
 *
 *   GET    /api/inventory/locations               tree (roots + nested children)
 *   GET    /api/inventory/locations?parent_id=N   flat list of N's direct children
 *   GET    /api/inventory/locations/{id}           one location
 *   POST   /api/inventory/locations                create (numbered drawer/tray/bin, etc.)
 *   PUT    /api/inventory/locations/{id}           update (partial)
 *
 *   GET    /api/inventory/receipts                list (filters: vendor, q)
 *   GET    /api/inventory/receipts/{id}            one receipt (metadata only)
 *   POST   /api/inventory/receipts                 create
 *   POST   /api/inventory/receipts/{id}/scan       upload scan (multipart field "file")
 *   GET    /api/inventory/receipts/{id}/scan       stream the scan back
 *
 *   GET    /api/inventory/projects                list (filters: status, q)
 *   GET    /api/inventory/projects/{id}            one project + its allocations
 *   POST   /api/inventory/projects                 create
 *   PUT    /api/inventory/projects/{id}            update (partial)
 *   DELETE /api/inventory/projects/{id}            soft-delete (+ its allocations)
 *
 *   POST   /api/inventory/projects/{id}/allocations                add (needed/committed/installed)
 *   PUT    /api/inventory/projects/{id}/allocations/{allocId}      commit/install/return/edit
 *   DELETE /api/inventory/projects/{id}/allocations/{allocId}      cancel (soft-delete)
 *
 *   GET    /api/inventory/shopping                v_project_shopping (filter: project_id)
 *
 * project_allocations invariant: hardware_unit_id XOR hardware_model_id (an
 * allocation is either a specific owned unit, or a model still to be bought).
 * Enforced here on create/update. The DB additionally guards against
 * committing/installing a unit already committed elsewhere
 * (trg_alloc_no_double_commit_*, SIGNAL SQLSTATE 45000) — Inv::exec() catches
 * that and returns HTTP 409. That trigger's "move the unit to the project's
 * tray" side effect (trg_alloc_tray_move) only fires AFTER INSERT, so the PUT
 * allocation handler below replicates it manually when a PUT transitions a
 * row to committed/installed, keeping the two code paths behaviourally
 * identical.
 */

require_once __DIR__ . '/../lib/Sor.php';
require_once __DIR__ . '/../lib/Operator.php';
require_once __DIR__ . '/../lib/Coerce.php';

return static function (Router $router): void {

    // ==================================================================
    // components (hardware_units)
    // ==================================================================

    $router->get('/api/inventory/components', static function (): void {
        $where  = ['hu.deleted_at IS NULL', 'hm.deleted_at IS NULL'];
        $params = [];

        if (isset($_GET['stock_status']) && $_GET['stock_status'] !== '') {
            $where[]          = 'hu.stock_status = :ss';
            $params[':ss']    = Inv::requireEnum('hardware_units', 'stock_status', $_GET['stock_status'], 'stock_status');
        }
        if (isset($_GET['location_id']) && $_GET['location_id'] !== '') {
            if (!ctype_digit((string) $_GET['location_id'])) {
                Response::error('bad_request', 'location_id must be an integer.', 400);
            }
            $where[]           = 'hu.location_id = :loc';
            $params[':loc']    = (int) $_GET['location_id'];
        }
        if (isset($_GET['model_id']) && $_GET['model_id'] !== '') {
            if (!ctype_digit((string) $_GET['model_id'])) {
                Response::error('bad_request', 'model_id must be an integer.', 400);
            }
            $where[]           = 'hu.model_id = :mid';
            $params[':mid']    = (int) $_GET['model_id'];
        }
        if (isset($_GET['q']) && is_string($_GET['q']) && trim($_GET['q']) !== '') {
            $where[]          = '(hu.serial LIKE :q OR hu.asset_tag LIKE :q OR hm.make LIKE :q OR hm.model LIKE :q)';
            $params[':q']     = '%' . trim($_GET['q']) . '%';
        }

        $limit = Inv::clampLimit($_GET['limit'] ?? null, 500, 5000);
        $rows  = Inv::rows(
            inv_component_sql() . ' WHERE ' . implode(' AND ', $where) . " ORDER BY hu.id DESC LIMIT $limit",
            $params
        );
        Response::ok(array_map('inv_component_row', $rows));
    });

    $router->get('/api/inventory/components/{id}', static function (array $p): void {
        $id  = Inv::idParam($p['id']);
        $row = Inv::row(inv_component_sql() . ' WHERE hu.id = :i AND hu.deleted_at IS NULL', [':i' => $id]);
        if ($row === null) {
            Response::error('not_found', "No component $id", 404);
        }
        Response::ok(inv_component_row($row));
    });

    $router->post('/api/inventory/components', static function (): void {
        Operator::requireOperator();
        $b = solari_json_body();

        $modelId = inv_optional_fk('hardware_models', $b['model_id'] ?? null, 'model_id');
        if ($modelId === null) {
            Response::error('bad_request', 'model_id is required.', 400);
        }

        Inv::exec(
            'INSERT INTO hardware_units
                (model_id, serial, asset_tag, location_id, acquired_on, quantity, unit_cost_cents,
                 stock_status, receipt_id, notes, source_id, asserted_kind, asserted_at)
             VALUES
                (:model_id, :serial, :asset_tag, :location_id, :acquired_on, :quantity, :unit_cost_cents,
                 :stock_status, :receipt_id, :notes, :source_id, \'human\', NOW())',
            [
                ':model_id'        => $modelId,
                ':serial'          => inv_str($b, 'serial', 128),
                ':asset_tag'       => inv_str($b, 'asset_tag', 64),
                ':location_id'     => inv_optional_fk('locations', $b['location_id'] ?? null, 'location_id'),
                ':acquired_on'     => inv_date($b['acquired_on'] ?? null),
                ':quantity'        => inv_quantity($b),
                ':unit_cost_cents' => inv_cents($b, 'unit_cost_cents'),
                ':stock_status'    => isset($b['stock_status']) && $b['stock_status'] !== ''
                                        ? Inv::requireEnum('hardware_units', 'stock_status', $b['stock_status'], 'stock_status')
                                        : 'in_stock',
                ':receipt_id'      => inv_optional_fk('receipts', $b['receipt_id'] ?? null, 'receipt_id'),
                ':notes'           => inv_str($b, 'notes'),
                ':source_id'       => Sor::dashboardSourceId(),
            ]
        );
        $id = (int) Inv::lastInsertId();
        Response::ok(inv_component_row(Inv::row(inv_component_sql() . ' WHERE hu.id = :i', [':i' => $id])), 201);
    });

    $router->add('PUT', '/api/inventory/components/{id}', static function (array $p): void {
        Operator::requireOperator();
        $id = Inv::idParam($p['id']);
        if (Inv::row('SELECT id FROM hardware_units WHERE id = :i AND deleted_at IS NULL', [':i' => $id]) === null) {
            Response::error('not_found', "No component $id", 404);
        }
        $b = solari_json_body();

        $sets   = [];
        $params = [':id' => $id];

        if (array_key_exists('model_id', $b)) {
            $mid = inv_optional_fk('hardware_models', $b['model_id'], 'model_id');
            if ($mid === null) {
                Response::error('bad_request', 'model_id cannot be cleared.', 400);
            }
            $sets[] = 'model_id = :mid'; $params[':mid'] = $mid;
        }
        if (array_key_exists('serial', $b)) {
            $sets[] = 'serial = :serial'; $params[':serial'] = inv_str($b, 'serial', 128);
        }
        if (array_key_exists('asset_tag', $b)) {
            $sets[] = 'asset_tag = :atag'; $params[':atag'] = inv_str($b, 'asset_tag', 64);
        }
        if (array_key_exists('location_id', $b)) {
            $sets[] = 'location_id = :loc'; $params[':loc'] = inv_optional_fk('locations', $b['location_id'], 'location_id');
        }
        if (array_key_exists('acquired_on', $b)) {
            $sets[] = 'acquired_on = :acq'; $params[':acq'] = inv_date($b['acquired_on']);
        }
        if (array_key_exists('quantity', $b)) {
            $sets[] = 'quantity = :qty'; $params[':qty'] = inv_quantity($b);
        }
        if (array_key_exists('unit_cost_cents', $b)) {
            $sets[] = 'unit_cost_cents = :cost'; $params[':cost'] = inv_cents($b, 'unit_cost_cents');
        }
        if (array_key_exists('stock_status', $b)) {
            $sets[] = 'stock_status = :ss';
            $params[':ss'] = Inv::requireEnum('hardware_units', 'stock_status', $b['stock_status'], 'stock_status');
        }
        if (array_key_exists('receipt_id', $b)) {
            $sets[] = 'receipt_id = :rid'; $params[':rid'] = inv_optional_fk('receipts', $b['receipt_id'], 'receipt_id');
        }
        if (array_key_exists('notes', $b)) {
            $sets[] = 'notes = :notes'; $params[':notes'] = inv_str($b, 'notes');
        }
        if ($sets === []) {
            Response::error('bad_request', 'No updatable fields supplied.', 400);
        }

        Inv::exec('UPDATE hardware_units SET ' . implode(', ', $sets) . ' WHERE id = :id', $params);
        Response::ok(inv_component_row(Inv::row(inv_component_sql() . ' WHERE hu.id = :i', [':i' => $id])));
    });

    $router->add('DELETE', '/api/inventory/components/{id}', static function (array $p): void {
        Operator::requireOperator();
        $id = Inv::idParam($p['id']);
        if (Inv::row('SELECT id FROM hardware_units WHERE id = :i AND deleted_at IS NULL', [':i' => $id]) === null) {
            Response::error('not_found', "No component $id", 404);
        }
        Inv::exec('UPDATE hardware_units SET deleted_at = NOW() WHERE id = :i', [':i' => $id]);
        Response::ok(['id' => $id, 'status' => 'deleted']);
    });

    // ==================================================================
    // models (hardware_models)
    // ==================================================================

    $router->get('/api/inventory/models', static function (): void {
        $where  = ['deleted_at IS NULL'];
        $params = [];
        if (isset($_GET['hw_type']) && $_GET['hw_type'] !== '') {
            $where[]        = 'hw_type = :t';
            $params[':t']   = Inv::requireEnum('hardware_models', 'hw_type', $_GET['hw_type'], 'hw_type');
        }
        if (isset($_GET['q']) && is_string($_GET['q']) && trim($_GET['q']) !== '') {
            $where[]        = '(make LIKE :q OR model LIKE :q)';
            $params[':q']   = '%' . trim($_GET['q']) . '%';
        }
        $rows = Inv::rows(
            inv_model_sql() . ' WHERE ' . implode(' AND ', $where) . ' ORDER BY make, model',
            $params
        );
        Response::ok(array_map('inv_model_row', $rows));
    });

    $router->get('/api/inventory/models/{id}', static function (array $p): void {
        $id  = Inv::idParam($p['id']);
        $row = Inv::row(inv_model_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($row === null) {
            Response::error('not_found', "No hardware model $id", 404);
        }
        Response::ok(inv_model_row($row));
    });

    $router->post('/api/inventory/models', static function (): void {
        Operator::requireOperator();
        $b = solari_json_body();

        $make  = inv_str_required($b, 'make', 128);
        $model = inv_str_required($b, 'model', 128);
        $hwType = isset($b['hw_type']) && $b['hw_type'] !== ''
            ? Inv::requireEnum('hardware_models', 'hw_type', $b['hw_type'], 'hw_type')
            : 'other';

        Inv::exec(
            "INSERT INTO hardware_models (make, model, hw_type, notes, source_id, asserted_kind, asserted_at)
             VALUES (:make, :model, :hw_type, :notes, :src, 'human', NOW())",
            [
                ':make'    => $make,
                ':model'   => $model,
                ':hw_type' => $hwType,
                ':notes'   => inv_str($b, 'notes'),
                ':src'     => Sor::dashboardSourceId(),
            ]
        );
        $id = (int) Inv::lastInsertId();
        Response::ok(inv_model_row(Inv::row(inv_model_sql() . ' WHERE id = :i', [':i' => $id])), 201);
    });

    // ==================================================================
    // locations (tree by parent_id)
    // ==================================================================

    $router->get('/api/inventory/locations', static function (): void {
        if (isset($_GET['parent_id']) && $_GET['parent_id'] !== '') {
            if (!ctype_digit((string) $_GET['parent_id'])) {
                Response::error('bad_request', 'parent_id must be an integer.', 400);
            }
            $pid  = (int) $_GET['parent_id'];
            $rows = Inv::rows(
                inv_location_sql() . ' WHERE deleted_at IS NULL AND parent_id = :p ORDER BY name',
                [':p' => $pid]
            );
            Response::ok(array_map('inv_location_row', $rows));
        }

        $rows = Inv::rows(inv_location_sql() . ' WHERE deleted_at IS NULL ORDER BY name');
        Response::ok(inv_location_tree($rows));
    });

    $router->get('/api/inventory/locations/{id}', static function (array $p): void {
        $id  = Inv::idParam($p['id']);
        $row = Inv::row(inv_location_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($row === null) {
            Response::error('not_found', "No location $id", 404);
        }
        Response::ok(inv_location_row($row));
    });

    $router->post('/api/inventory/locations', static function (): void {
        Operator::requireOperator();
        $b = solari_json_body();

        $name = inv_str_required($b, 'name', 128);
        if (!isset($b['loc_type']) || $b['loc_type'] === '') {
            Response::error('bad_request', 'loc_type is required.', 400);
        }
        $locType  = Inv::requireEnum('locations', 'loc_type', $b['loc_type'], 'loc_type');
        $parentId = inv_optional_fk('locations', $b['parent_id'] ?? null, 'parent_id');

        Inv::exec(
            "INSERT INTO locations (parent_id, name, loc_type, code, notes, source_id, asserted_kind, asserted_at)
             VALUES (:pid, :name, :lt, :code, :notes, :src, 'human', NOW())",
            [
                ':pid'   => $parentId,
                ':name'  => $name,
                ':lt'    => $locType,
                ':code'  => inv_str($b, 'code', 32),
                ':notes' => inv_str($b, 'notes'),
                ':src'   => Sor::dashboardSourceId(),
            ]
        );
        $id = (int) Inv::lastInsertId();
        Response::ok(inv_location_row(Inv::row(inv_location_sql() . ' WHERE id = :i', [':i' => $id])), 201);
    });

    $router->add('PUT', '/api/inventory/locations/{id}', static function (array $p): void {
        Operator::requireOperator();
        $id = Inv::idParam($p['id']);
        if (Inv::row('SELECT id FROM locations WHERE id = :i AND deleted_at IS NULL', [':i' => $id]) === null) {
            Response::error('not_found', "No location $id", 404);
        }
        $b = solari_json_body();

        $sets   = [];
        $params = [':id' => $id];

        if (array_key_exists('name', $b)) {
            $name = inv_str($b, 'name', 128);
            if ($name === null) {
                Response::error('bad_request', 'name cannot be cleared.', 400);
            }
            $sets[] = 'name = :name'; $params[':name'] = $name;
        }
        if (array_key_exists('loc_type', $b)) {
            $sets[] = 'loc_type = :lt';
            $params[':lt'] = Inv::requireEnum('locations', 'loc_type', $b['loc_type'], 'loc_type');
        }
        if (array_key_exists('code', $b)) {
            $sets[] = 'code = :code'; $params[':code'] = inv_str($b, 'code', 32);
        }
        if (array_key_exists('notes', $b)) {
            $sets[] = 'notes = :notes'; $params[':notes'] = inv_str($b, 'notes');
        }
        if (array_key_exists('parent_id', $b)) {
            $pid = inv_optional_fk('locations', $b['parent_id'], 'parent_id');
            if ($pid === $id) {
                Response::error('bad_request', 'A location cannot be its own parent.', 400);
            }
            $sets[] = 'parent_id = :pid'; $params[':pid'] = $pid;
        }
        if ($sets === []) {
            Response::error('bad_request', 'No updatable fields supplied.', 400);
        }

        Inv::exec('UPDATE locations SET ' . implode(', ', $sets) . ' WHERE id = :id', $params);
        Response::ok(inv_location_row(Inv::row(inv_location_sql() . ' WHERE id = :i', [':i' => $id])));
    });

    // ==================================================================
    // receipts (purchase record + scan)
    // ==================================================================

    $router->get('/api/inventory/receipts', static function (): void {
        $where  = ['deleted_at IS NULL'];
        $params = [];
        if (isset($_GET['vendor']) && is_string($_GET['vendor']) && trim($_GET['vendor']) !== '') {
            $where[]        = 'vendor LIKE :v';
            $params[':v']   = '%' . trim($_GET['vendor']) . '%';
        }
        if (isset($_GET['q']) && is_string($_GET['q']) && trim($_GET['q']) !== '') {
            $where[]        = '(vendor LIKE :q OR order_ref LIKE :q OR notes LIKE :q)';
            $params[':q']   = '%' . trim($_GET['q']) . '%';
        }
        $rows = Inv::rows(
            inv_receipt_sql() . ' WHERE ' . implode(' AND ', $where) . ' ORDER BY purchased_on DESC, id DESC',
            $params
        );
        Response::ok(array_map('inv_receipt_row', $rows));
    });

    $router->get('/api/inventory/receipts/{id}', static function (array $p): void {
        $id  = Inv::idParam($p['id']);
        $row = Inv::row(inv_receipt_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($row === null) {
            Response::error('not_found', "No receipt $id", 404);
        }
        Response::ok(inv_receipt_row($row));
    });

    $router->post('/api/inventory/receipts', static function (): void {
        Operator::requireOperator();
        $b = solari_json_body();

        $currency = 'USD';
        if (isset($b['currency']) && is_string($b['currency']) && preg_match('/^[A-Za-z]{3}$/', $b['currency']) === 1) {
            $currency = strtoupper($b['currency']);
        }

        Inv::exec(
            "INSERT INTO receipts (vendor, purchased_on, subtotal_cents, currency, order_ref, notes, source_id, asserted_kind, asserted_at)
             VALUES (:vendor, :po, :subtotal, :cur, :ref, :notes, :src, 'human', NOW())",
            [
                ':vendor'   => inv_str($b, 'vendor', 128),
                ':po'       => inv_date($b['purchased_on'] ?? null),
                ':subtotal' => inv_cents($b, 'subtotal_cents'),
                ':cur'      => $currency,
                ':ref'      => inv_str($b, 'order_ref', 96),
                ':notes'    => inv_str($b, 'notes'),
                ':src'      => Sor::dashboardSourceId(),
            ]
        );
        $id = (int) Inv::lastInsertId();
        Response::ok(inv_receipt_row(Inv::row(inv_receipt_sql() . ' WHERE id = :i', [':i' => $id])), 201);
    });

    // POST /api/inventory/receipts/{id}/scan — multipart upload, field "file".
    $router->post('/api/inventory/receipts/{id}/scan', static function (array $p): void {
        Operator::requireOperator();
        $id      = Inv::idParam($p['id']);
        $receipt = Inv::row('SELECT id, file_path FROM receipts WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($receipt === null) {
            Response::error('not_found', "No receipt $id", 404);
        }

        if (!isset($_FILES['file']) || !is_array($_FILES['file'])) {
            Response::error('bad_request', "Multipart field 'file' is required.", 400);
        }
        $file = $_FILES['file'];
        if ((int) ($file['error'] ?? UPLOAD_ERR_NO_FILE) !== UPLOAD_ERR_OK) {
            Response::error('bad_request', 'Upload failed (PHP upload error ' . (int) ($file['error'] ?? -1) . ').', 400);
        }
        $maxBytes = 25 * 1024 * 1024; // 25 MiB cap
        if ((int) ($file['size'] ?? 0) > $maxBytes || (int) ($file['size'] ?? 0) < 1) {
            Response::error('bad_request', 'Upload must be non-empty and at most 25 MiB.', 400);
        }
        $tmpPath = (string) ($file['tmp_name'] ?? '');
        if ($tmpPath === '' || !is_uploaded_file($tmpPath)) {
            Response::error('bad_request', 'Invalid upload.', 400);
        }

        // Sniff the real MIME from content — never trust the client-supplied
        // Content-Type — and only accept image/* or application/pdf.
        $finfo = new finfo(FILEINFO_MIME_TYPE);
        $mime  = (string) $finfo->file($tmpPath);
        $ext   = inv_scan_extension($mime);
        if ($ext === null) {
            Response::error('bad_request', "Unsupported file type '$mime'; only images and PDF scans are accepted.", 400);
        }

        $uploadDir = inv_uploads_dir();
        if (!is_dir($uploadDir) && !@mkdir($uploadDir, 0750, true) && !is_dir($uploadDir)) {
            Response::error('internal_error', "Upload directory '$uploadDir' is not available on this host.", 500);
        }
        if (!is_writable($uploadDir)) {
            Response::error('internal_error', "Upload directory '$uploadDir' is not writable on this host.", 500);
        }

        // Random filename — the client's original filename is never trusted
        // for path construction.
        $destPath = $uploadDir . '/' . bin2hex(random_bytes(16)) . '.' . $ext;
        if (!move_uploaded_file($tmpPath, $destPath)) {
            Response::error('internal_error', 'Failed to store the uploaded file.', 500);
        }
        @chmod($destPath, 0640);

        $oldPath = $receipt['file_path'] ?? null;
        Inv::exec('UPDATE receipts SET file_path = :fp, file_mime = :fm WHERE id = :i', [':fp' => $destPath, ':fm' => $mime, ':i' => $id]);

        // Best-effort cleanup of the previous scan file, now orphaned.
        if (is_string($oldPath) && $oldPath !== '' && $oldPath !== $destPath && is_file($oldPath)) {
            @unlink($oldPath);
        }

        Response::ok(inv_receipt_row(Inv::row(inv_receipt_sql() . ' WHERE id = :i', [':i' => $id])));
    });

    // GET /api/inventory/receipts/{id}/scan — stream the stored scan back.
    // Deliberately bypasses the JSON envelope (Response::ok) to emit the raw
    // file with its real content-type, matching the /api/stream.php precedent
    // for handlers that own their own output framing.
    $router->get('/api/inventory/receipts/{id}/scan', static function (array $p): void {
        $id      = Inv::idParam($p['id']);
        $receipt = Inv::row('SELECT file_path, file_mime FROM receipts WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($receipt === null || empty($receipt['file_path'])) {
            Response::error('not_found', "No scan on file for receipt $id", 404);
        }
        $path = (string) $receipt['file_path'];
        if (!is_file($path) || !is_readable($path)) {
            Response::error('not_found', "Scan file is missing on this host for receipt $id", 404);
        }
        $mime = (string) ($receipt['file_mime'] ?? 'application/octet-stream');
        $ext  = inv_scan_extension($mime) ?? 'bin';

        if (!headers_sent()) {
            http_response_code(200);
            header('Content-Type: ' . $mime);
            header('Content-Length: ' . (string) filesize($path));
            header('Cache-Control: private, max-age=0, no-cache');
            header('X-Content-Type-Options: nosniff');
            header('Content-Disposition: inline; filename="receipt-' . $id . '.' . $ext . '"');
        }
        readfile($path);
        exit;
    });

    // ==================================================================
    // projects
    // ==================================================================

    $router->get('/api/inventory/projects', static function (): void {
        $where  = ['deleted_at IS NULL'];
        $params = [];
        if (isset($_GET['status']) && $_GET['status'] !== '') {
            $where[]        = 'status = :s';
            $params[':s']   = Inv::requireEnum('projects', 'status', $_GET['status'], 'status');
        }
        if (isset($_GET['q']) && is_string($_GET['q']) && trim($_GET['q']) !== '') {
            $where[]        = '(name LIKE :q OR code LIKE :q)';
            $params[':q']   = '%' . trim($_GET['q']) . '%';
        }
        $rows = Inv::rows(
            inv_project_sql() . ' WHERE ' . implode(' AND ', $where) . ' ORDER BY id DESC',
            $params
        );
        Response::ok(array_map('inv_project_row', $rows));
    });

    // GET /api/inventory/projects/{id} — project + its live allocations.
    $router->get('/api/inventory/projects/{id}', static function (array $p): void {
        $id   = Inv::idParam($p['id']);
        $proj = Inv::row(inv_project_sql() . ' WHERE id = :i AND deleted_at IS NULL', [':i' => $id]);
        if ($proj === null) {
            Response::error('not_found', "No project $id", 404);
        }
        $allocs = Inv::rows(
            inv_allocation_sql() . ' WHERE pa.project_id = :i AND pa.deleted_at IS NULL ORDER BY pa.id',
            [':i' => $id]
        );
        Response::ok([
            'project'     => inv_project_row($proj),
            'allocations' => array_map('inv_allocation_row', $allocs),
        ]);
    });

    $router->post('/api/inventory/projects', static function (): void {
        Operator::requireOperator();
        $b = solari_json_body();

        $name   = inv_str_required($b, 'name', 160);
        $status = isset($b['status']) && $b['status'] !== ''
            ? Inv::requireEnum('projects', 'status', $b['status'], 'status')
            : 'planning';

        Inv::exec(
            "INSERT INTO projects (name, code, status, tray_location_id, owner_user_id, description,
                                    started_on, target_on, completed_on, source_id, asserted_kind, asserted_at)
             VALUES (:name, :code, :status, :tray, :owner, :desc, :started, :target, :completed, :src, 'human', NOW())",
            [
                ':name'      => $name,
                ':code'      => inv_str($b, 'code', 32),
                ':status'    => $status,
                ':tray'      => inv_optional_fk('locations', $b['tray_location_id'] ?? null, 'tray_location_id'),
                ':owner'     => inv_optional_fk('users', $b['owner_user_id'] ?? null, 'owner_user_id'),
                ':desc'      => inv_str($b, 'description'),
                ':started'   => inv_date($b['started_on'] ?? null),
                ':target'    => inv_date($b['target_on'] ?? null),
                ':completed' => inv_date($b['completed_on'] ?? null),
                ':src'       => Sor::dashboardSourceId(),
            ]
        );
        $id = (int) Inv::lastInsertId();
        Response::ok(inv_project_row(Inv::row(inv_project_sql() . ' WHERE id = :i', [':i' => $id])), 201);
    });

    $router->add('PUT', '/api/inventory/projects/{id}', static function (array $p): void {
        Operator::requireOperator();
        $id = Inv::idParam($p['id']);
        if (Inv::row('SELECT id FROM projects WHERE id = :i AND deleted_at IS NULL', [':i' => $id]) === null) {
            Response::error('not_found', "No project $id", 404);
        }
        $b = solari_json_body();

        $sets   = [];
        $params = [':id' => $id];

        if (array_key_exists('name', $b)) {
            $name = inv_str($b, 'name', 160);
            if ($name === null) {
                Response::error('bad_request', 'name cannot be cleared.', 400);
            }
            $sets[] = 'name = :name'; $params[':name'] = $name;
        }
        if (array_key_exists('code', $b)) {
            $sets[] = 'code = :code'; $params[':code'] = inv_str($b, 'code', 32);
        }
        if (array_key_exists('status', $b)) {
            $sets[] = 'status = :status'; $params[':status'] = Inv::requireEnum('projects', 'status', $b['status'], 'status');
        }
        if (array_key_exists('tray_location_id', $b)) {
            $sets[] = 'tray_location_id = :tray'; $params[':tray'] = inv_optional_fk('locations', $b['tray_location_id'], 'tray_location_id');
        }
        if (array_key_exists('owner_user_id', $b)) {
            $sets[] = 'owner_user_id = :owner'; $params[':owner'] = inv_optional_fk('users', $b['owner_user_id'], 'owner_user_id');
        }
        if (array_key_exists('description', $b)) {
            $sets[] = 'description = :desc'; $params[':desc'] = inv_str($b, 'description');
        }
        foreach (['started_on' => 'started', 'target_on' => 'target', 'completed_on' => 'completed'] as $field => $ph) {
            if (array_key_exists($field, $b)) {
                $sets[] = "$field = :$ph"; $params[":$ph"] = inv_date($b[$field]);
            }
        }
        if ($sets === []) {
            Response::error('bad_request', 'No updatable fields supplied.', 400);
        }

        Inv::exec('UPDATE projects SET ' . implode(', ', $sets) . ' WHERE id = :id', $params);
        Response::ok(inv_project_row(Inv::row(inv_project_sql() . ' WHERE id = :i', [':i' => $id])));
    });

    // DELETE /api/inventory/projects/{id} — soft-delete the project AND
    // release its allocations (soft-delete them too), so units it had
    // committed/installed stop being phantom-reserved.
    $router->add('DELETE', '/api/inventory/projects/{id}', static function (array $p): void {
        Operator::requireOperator();
        $id = Inv::idParam($p['id']);
        if (Inv::row('SELECT id FROM projects WHERE id = :i AND deleted_at IS NULL', [':i' => $id]) === null) {
            Response::error('not_found', "No project $id", 404);
        }
        Inv::exec('UPDATE projects SET deleted_at = NOW() WHERE id = :i', [':i' => $id]);
        Inv::exec('UPDATE project_allocations SET deleted_at = NOW() WHERE project_id = :i AND deleted_at IS NULL', [':i' => $id]);
        Response::ok(['id' => $id, 'status' => 'deleted']);
    });

    // ==================================================================
    // project allocations (reserve / commit / install / return)
    // ==================================================================

    $router->post('/api/inventory/projects/{id}/allocations', static function (array $p): void {
        Operator::requireOperator();
        $projectId = Inv::idParam($p['id'], 'project id');
        if (Inv::row('SELECT id FROM projects WHERE id = :i AND deleted_at IS NULL', [':i' => $projectId]) === null) {
            Response::error('not_found', "No project $projectId", 404);
        }
        $b = solari_json_body();

        [$unitId, $modelId] = inv_unit_xor_model($b, true);
        $allocation = isset($b['allocation']) && $b['allocation'] !== ''
            ? Inv::requireEnum('project_allocations', 'allocation', $b['allocation'], 'allocation')
            : 'needed';

        Inv::exec(
            'INSERT INTO project_allocations (project_id, hardware_unit_id, hardware_model_id, allocation, quantity, notes, source_id)
             VALUES (:pid, :uid, :mid, :alloc, :qty, :notes, :src)',
            [
                ':pid'   => $projectId,
                ':uid'   => $unitId,
                ':mid'   => $modelId,
                ':alloc' => $allocation,
                ':qty'   => inv_quantity($b),
                ':notes' => inv_str($b, 'notes'),
                ':src'   => Sor::dashboardSourceId(),
            ]
        );
        $id = (int) Inv::lastInsertId();
        Response::ok(inv_allocation_row(Inv::row(inv_allocation_sql() . ' WHERE pa.id = :i', [':i' => $id])), 201);
    });

    // PUT .../allocations/{allocId} — edit quantity/notes, or transition
    // needed -> committed -> installed -> returned (or reassign the unit/model).
    $router->add('PUT', '/api/inventory/projects/{id}/allocations/{allocId}', static function (array $p): void {
        Operator::requireOperator();
        $projectId = Inv::idParam($p['id'], 'project id');
        $allocId   = Inv::idParam($p['allocId'], 'allocation id');

        $existing = Inv::row(
            'SELECT id, project_id, hardware_unit_id, hardware_model_id, allocation
               FROM project_allocations WHERE id = :i AND project_id = :p AND deleted_at IS NULL',
            [':i' => $allocId, ':p' => $projectId]
        );
        if ($existing === null) {
            Response::error('not_found', "No allocation $allocId on project $projectId", 404);
        }
        $b = solari_json_body();

        $sets   = [];
        $params = [':id' => $allocId];

        $nextUnitId  = $existing['hardware_unit_id']  !== null ? (int) $existing['hardware_unit_id']  : null;
        $nextModelId = $existing['hardware_model_id'] !== null ? (int) $existing['hardware_model_id'] : null;
        if (array_key_exists('hardware_unit_id', $b) || array_key_exists('hardware_model_id', $b)) {
            $merged = [
                'hardware_unit_id'  => array_key_exists('hardware_unit_id', $b)  ? $b['hardware_unit_id']  : $nextUnitId,
                'hardware_model_id' => array_key_exists('hardware_model_id', $b) ? $b['hardware_model_id'] : $nextModelId,
            ];
            [$nextUnitId, $nextModelId] = inv_unit_xor_model($merged, false);
            $sets[] = 'hardware_unit_id = :uid';  $params[':uid']  = $nextUnitId;
            $sets[] = 'hardware_model_id = :mid'; $params[':mid'] = $nextModelId;
        }

        $nextAllocation = (string) $existing['allocation'];
        if (array_key_exists('allocation', $b)) {
            $nextAllocation = Inv::requireEnum('project_allocations', 'allocation', $b['allocation'], 'allocation');
            $sets[] = 'allocation = :alloc'; $params[':alloc'] = $nextAllocation;
        }
        if (array_key_exists('quantity', $b)) {
            $sets[] = 'quantity = :qty'; $params[':qty'] = inv_quantity($b);
        }
        if (array_key_exists('notes', $b)) {
            $sets[] = 'notes = :notes'; $params[':notes'] = inv_str($b, 'notes');
        }
        if ($sets === []) {
            Response::error('bad_request', 'No updatable fields supplied.', 400);
        }

        Inv::exec('UPDATE project_allocations SET ' . implode(', ', $sets) . ' WHERE id = :id', $params);

        // trg_alloc_tray_move only fires AFTER INSERT (see migration 014); mirror
        // its "grab the tray" side effect here so PUT-driven commit/install
        // transitions behave identically to the POST-time (insert) path.
        if (in_array($nextAllocation, ['committed', 'installed'], true) && $nextUnitId !== null) {
            Inv::exec(
                "UPDATE hardware_units hu
                   JOIN projects pr ON pr.id = :pid
                    SET hu.location_id  = COALESCE(pr.tray_location_id, hu.location_id),
                        hu.stock_status = IF(:alloc = 'installed', 'deployed', 'allocated')
                 WHERE hu.id = :uid",
                [':pid' => $projectId, ':alloc' => $nextAllocation, ':uid' => $nextUnitId]
            );
        }

        Response::ok(inv_allocation_row(Inv::row(inv_allocation_sql() . ' WHERE pa.id = :i', [':i' => $allocId])));
    });

    $router->add('DELETE', '/api/inventory/projects/{id}/allocations/{allocId}', static function (array $p): void {
        Operator::requireOperator();
        $projectId = Inv::idParam($p['id'], 'project id');
        $allocId   = Inv::idParam($p['allocId'], 'allocation id');
        if (Inv::row('SELECT id FROM project_allocations WHERE id = :i AND project_id = :p AND deleted_at IS NULL',
                     [':i' => $allocId, ':p' => $projectId]) === null) {
            Response::error('not_found', "No allocation $allocId on project $projectId", 404);
        }
        Inv::exec('UPDATE project_allocations SET deleted_at = NOW() WHERE id = :i', [':i' => $allocId]);
        Response::ok(['id' => $allocId, 'status' => 'deleted']);
    });

    // ==================================================================
    // shopping list (v_project_shopping)
    // ==================================================================

    $router->get('/api/inventory/shopping', static function (): void {
        $sql    = 'SELECT project_id, project, hardware_model_id, make, model, hw_type, qty_needed FROM v_project_shopping';
        $params = [];
        if (isset($_GET['project_id']) && $_GET['project_id'] !== '') {
            if (!ctype_digit((string) $_GET['project_id'])) {
                Response::error('bad_request', 'project_id must be an integer.', 400);
            }
            $sql            .= ' WHERE project_id = :p';
            $params[':p']    = (int) $_GET['project_id'];
        }
        $sql .= ' ORDER BY project, make, model';

        $rows = Inv::rows($sql, $params);
        Response::ok(array_map(static function (array $r): array {
            return [
                'projectId'       => (int) $r['project_id'],
                'project'         => $r['project'],
                'hardwareModelId' => Coerce::int($r['hardware_model_id']),
                'make'            => $r['make'],
                'model'           => $r['model'],
                'hwType'          => $r['hw_type'],
                'qtyNeeded'       => Coerce::int($r['qty_needed']),
            ];
        }, $rows));
    });
};

/* ========================================================================
 * Inv — SoR PDO access + shared validation for the inventory routes.
 * ==================================================================== */
final class Inv
{
    /** @param array<string,mixed> $params @return array<int,array<string,mixed>> */
    public static function rows(string $sql, array $params = []): array
    {
        $st = Sor::db()->prepare($sql);
        $st->execute($params);
        return $st->fetchAll();
    }

    /** @param array<string,mixed> $params @return array<string,mixed>|null */
    public static function row(string $sql, array $params = []): ?array
    {
        $st = Sor::db()->prepare($sql);
        $st->execute($params);
        $r = $st->fetch();
        return $r === false ? null : $r;
    }

    /** @param array<string,mixed> $params */
    public static function scalar(string $sql, array $params = [])
    {
        $st = Sor::db()->prepare($sql);
        $st->execute($params);
        $v = $st->fetchColumn();
        return $v === false ? null : $v;
    }

    /**
     * Run a parameterised write. Centralises SIGNAL/constraint handling for
     * every insert/update/delete in this file: SQLSTATE 45000 is the
     * trg_alloc_no_double_commit_* guard's SIGNAL (already-committed
     * elsewhere) -> HTTP 409 with the trigger's own message; SQLSTATE 23000
     * is a unique-key/FK violation -> generic HTTP 409. Anything else
     * propagates to bootstrap's exception handler (HTTP 500).
     *
     * @param array<string,mixed> $params
     */
    public static function exec(string $sql, array $params = []): int
    {
        try {
            $st = Sor::db()->prepare($sql);
            $st->execute($params);
            return $st->rowCount();
        } catch (\PDOException $e) {
            $state = (string) $e->getCode();
            if ($state === '45000') {
                Response::error('conflict', (string) ($e->errorInfo[2] ?? $e->getMessage()), 409);
            }
            if ($state === '23000') {
                Response::error('conflict', 'Duplicate or constraint violation.', 409);
            }
            throw $e;
        }
    }

    public static function lastInsertId(): string
    {
        return Sor::db()->lastInsertId();
    }

    /** Validate a {id}-style path segment is a positive integer. */
    public static function idParam(string $raw, string $label = 'id'): int
    {
        if (preg_match('/^\d+$/', $raw) !== 1 || $raw === '0') {
            Response::error('bad_request', "$label must be a positive integer.", 400);
        }
        return (int) $raw;
    }

    /** @return array<int,string> */
    public static function enumValues(string $table, string $column): array
    {
        $type = (string) self::scalar(
            'SELECT COLUMN_TYPE FROM information_schema.COLUMNS
              WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = :t AND COLUMN_NAME = :c',
            [':t' => $table, ':c' => $column]
        );
        preg_match_all("/'((?:[^'\\\\]|\\\\.)*)'/", $type, $m);
        return array_map(static fn (string $v): string => stripslashes($v), $m[1] ?? []);
    }

    /** Validate $value is a live ENUM member of table.column; 400 if not. */
    public static function requireEnum(string $table, string $column, $value, string $label): string
    {
        $value   = (string) $value;
        $allowed = self::enumValues($table, $column);
        if ($allowed === [] || !in_array($value, $allowed, true)) {
            Response::error('bad_request', "$label must be one of: " . implode(', ', $allowed), 400);
        }
        return $value;
    }

    public static function clampLimit($raw, int $default, int $max): int
    {
        if ($raw === null || $raw === '' || !is_numeric($raw)) {
            return $default;
        }
        $n = (int) $raw;
        return $n < 1 ? $default : min($n, $max);
    }
}

/* ========================================================================
 * body-field validation helpers (shared by every create/update handler)
 * ==================================================================== */

/** Optional trimmed string field; null if absent/empty. 400 if wrong type. */
function inv_str(array $b, string $key, int $maxLen = 0): ?string
{
    if (!array_key_exists($key, $b) || $b[$key] === null || $b[$key] === '') {
        return null;
    }
    if (!is_string($b[$key])) {
        Response::error('bad_request', "$key must be a string.", 400);
    }
    $v = trim($b[$key]);
    if ($v === '') {
        return null;
    }
    return $maxLen > 0 ? substr($v, 0, $maxLen) : $v;
}

/** Required trimmed string field; 400 if absent/empty/wrong type. */
function inv_str_required(array $b, string $key, int $maxLen = 0): string
{
    $v = inv_str($b, $key, $maxLen);
    if ($v === null) {
        Response::error('bad_request', "$key is required.", 400);
    }
    return $v;
}

/** Optional 'YYYY-MM-DD' date field; null if absent/empty. 400 if malformed. */
function inv_date($v): ?string
{
    if ($v === null || $v === '') {
        return null;
    }
    if (!is_string($v) || preg_match('/^\d{4}-\d{2}-\d{2}$/', $v) !== 1) {
        Response::error('bad_request', 'Dates must be YYYY-MM-DD.', 400);
    }
    return $v;
}

/** Optional non-negative integer-cents field. 400 if present but invalid. */
function inv_cents(array $b, string $key): ?int
{
    if (!array_key_exists($key, $b) || $b[$key] === null || $b[$key] === '') {
        return null;
    }
    if (!is_numeric($b[$key]) || (int) $b[$key] < 0) {
        Response::error('bad_request', "$key must be a non-negative integer (cents).", 400);
    }
    return (int) $b[$key];
}

/** quantity field: defaults to 1 if absent; 400 if present but not a positive integer. */
function inv_quantity(array $b, int $default = 1): int
{
    if (!array_key_exists('quantity', $b) || $b['quantity'] === null || $b['quantity'] === '') {
        return $default;
    }
    if (!ctype_digit((string) $b['quantity']) || (int) $b['quantity'] < 1) {
        Response::error('bad_request', 'quantity must be a positive integer.', 400);
    }
    return (int) $b['quantity'];
}

/**
 * Optional FK: null if absent/empty; else must be a positive integer that
 * exists (and is live, deleted_at IS NULL) in $table. 400 otherwise.
 */
function inv_optional_fk(string $table, $raw, string $label): ?int
{
    if ($raw === null || $raw === '') {
        return null;
    }
    if (!ctype_digit((string) $raw)) {
        Response::error('bad_request', "$label must be an integer.", 400);
    }
    $id = (int) $raw;
    if (Inv::row("SELECT id FROM $table WHERE id = :i AND deleted_at IS NULL", [':i' => $id]) === null) {
        Response::error('bad_request', "No $table row $id", 400);
    }
    return $id;
}

/**
 * Enforce the project_allocations invariant: hardware_unit_id XOR
 * hardware_model_id. $required=true (create) means exactly one MUST be
 * given; $required=false (update, already-merged with the existing row)
 * still demands exactly one, just without re-demanding presence in $b.
 *
 * @param array<string,mixed> $b
 * @return array{0:?int,1:?int} [unitId, modelId]
 */
function inv_unit_xor_model(array $b, bool $required): array
{
    $hasUnit  = isset($b['hardware_unit_id'])  && $b['hardware_unit_id']  !== null && $b['hardware_unit_id']  !== '';
    $hasModel = isset($b['hardware_model_id']) && $b['hardware_model_id'] !== null && $b['hardware_model_id'] !== '';
    if ($hasUnit === $hasModel) {
        Response::error('bad_request', 'Exactly one of hardware_unit_id or hardware_model_id is required.', 400);
    }
    $unitId  = $hasUnit  ? inv_optional_fk('hardware_units', $b['hardware_unit_id'], 'hardware_unit_id') : null;
    $modelId = $hasModel ? inv_optional_fk('hardware_models', $b['hardware_model_id'], 'hardware_model_id') : null;
    return [$unitId, $modelId];
}

/* ========================================================================
 * shared SELECT fragments + row -> wire-shape mappers, one pair per entity
 * ==================================================================== */

function inv_component_sql(): string
{
    return "SELECT hu.id, hu.model_id, hu.serial, hu.asset_tag, hu.location_id, hu.acquired_on,
                   hu.quantity, hu.unit_cost_cents, hu.stock_status, hu.receipt_id, hu.notes,
                   hu.source_id, hu.created_at, hu.updated_at,
                   hm.make, hm.model, hm.hw_type,
                   l.name AS location_name, l.code AS location_code, l.loc_type AS location_type
              FROM hardware_units hu
              JOIN hardware_models hm ON hm.id = hu.model_id
              LEFT JOIN locations l ON l.id = hu.location_id AND l.deleted_at IS NULL";
}

function inv_component_row(array $r): array
{
    return [
        'id'            => (int) $r['id'],
        'modelId'       => (int) $r['model_id'],
        'make'          => $r['make'] ?? null,
        'model'         => $r['model'] ?? null,
        'hwType'        => $r['hw_type'] ?? null,
        'serial'        => $r['serial'],
        'assetTag'      => $r['asset_tag'],
        'locationId'    => Coerce::int($r['location_id']),
        'locationName'  => $r['location_name'] ?? null,
        'locationCode'  => $r['location_code'] ?? null,
        'acquiredOn'    => $r['acquired_on'] ?? null,
        'quantity'      => Coerce::int($r['quantity']),
        'unitCostCents' => Coerce::int($r['unit_cost_cents']),
        'stockStatus'   => $r['stock_status'],
        'receiptId'     => Coerce::int($r['receipt_id']),
        'notes'         => $r['notes'],
        'sourceId'      => Coerce::int($r['source_id']),
        'createdAt'     => Coerce::iso($r['created_at'] ?? null),
        'updatedAt'     => Coerce::iso($r['updated_at'] ?? null),
    ];
}

function inv_model_sql(): string
{
    return 'SELECT id, make, model, hw_type, notes, source_id, created_at, updated_at FROM hardware_models';
}

function inv_model_row(array $r): array
{
    return [
        'id'        => (int) $r['id'],
        'make'      => $r['make'],
        'model'     => $r['model'],
        'hwType'    => $r['hw_type'],
        'notes'     => $r['notes'],
        'sourceId'  => Coerce::int($r['source_id']),
        'createdAt' => Coerce::iso($r['created_at'] ?? null),
        'updatedAt' => Coerce::iso($r['updated_at'] ?? null),
    ];
}

function inv_location_sql(): string
{
    return 'SELECT id, parent_id, name, code, loc_type, notes, source_id, created_at, updated_at FROM locations';
}

function inv_location_row(array $r): array
{
    return [
        'id'        => (int) $r['id'],
        'parentId'  => Coerce::int($r['parent_id']),
        'name'      => $r['name'],
        'code'      => $r['code'] ?? null,
        'locType'   => $r['loc_type'],
        'notes'     => $r['notes'] ?? null,
        'sourceId'  => Coerce::int($r['source_id']),
        'createdAt' => Coerce::iso($r['created_at'] ?? null),
        'updatedAt' => Coerce::iso($r['updated_at'] ?? null),
    ];
}

/**
 * Build a parent_id -> children[] tree from a flat row set. Two passes so row
 * order never matters: first materialise every node (with an empty children
 * bucket), then link each into its parent's bucket by reference.
 *
 * @param array<int,array<string,mixed>> $rows
 * @return array<int,array<string,mixed>>
 */
function inv_location_tree(array $rows): array
{
    $byId = [];
    foreach ($rows as $r) {
        $byId[(int) $r['id']] = inv_location_row($r) + ['children' => []];
    }
    $roots = [];
    foreach ($rows as $r) {
        $id  = (int) $r['id'];
        $pid = $r['parent_id'] !== null ? (int) $r['parent_id'] : null;
        if ($pid !== null && isset($byId[$pid])) {
            $byId[$pid]['children'][] = &$byId[$id];
        } else {
            $roots[] = &$byId[$id];
        }
    }
    return $roots;
}

function inv_receipt_sql(): string
{
    return 'SELECT id, vendor, purchased_on, subtotal_cents, currency, order_ref, file_path, file_mime,
                    notes, source_id, created_at, updated_at
               FROM receipts';
}

function inv_receipt_row(array $r): array
{
    $hasScan = !empty($r['file_path']);
    return [
        'id'            => (int) $r['id'],
        'vendor'        => $r['vendor'],
        'purchasedOn'   => $r['purchased_on'] ?? null,
        'subtotalCents' => Coerce::int($r['subtotal_cents']),
        'currency'      => $r['currency'],
        'orderRef'      => $r['order_ref'],
        'notes'         => $r['notes'],
        'sourceId'      => Coerce::int($r['source_id']),
        // The server file_path is never put on the wire; expose only
        // presence + a stable fetch URL for the scan sub-resource.
        'hasScan'       => $hasScan,
        'scanMime'      => $r['file_mime'] ?? null,
        'scanUrl'       => $hasScan ? '/api/inventory/receipts/' . (int) $r['id'] . '/scan' : null,
        'createdAt'     => Coerce::iso($r['created_at'] ?? null),
        'updatedAt'     => Coerce::iso($r['updated_at'] ?? null),
    ];
}

/** MIME (sniffed via finfo) -> accepted file extension; null if unsupported. */
function inv_scan_extension(string $mime): ?string
{
    $map = [
        'application/pdf' => 'pdf',
        'image/jpeg'       => 'jpg',
        'image/png'        => 'png',
        'image/webp'       => 'webp',
        'image/gif'        => 'gif',
        'image/tiff'       => 'tiff',
        'image/heic'       => 'heic',
        'image/heif'       => 'heif',
    ];
    return $map[$mime] ?? null;
}

/** Server directory receipt scans are stored under (SOLARI_UPLOAD_DIR overrides). */
function inv_uploads_dir(): string
{
    $dir = getenv('SOLARI_UPLOAD_DIR');
    if (is_string($dir) && $dir !== '') {
        return rtrim($dir, '/') . '/receipts';
    }
    return '/var/www/solarinet/uploads/receipts';
}

function inv_project_sql(): string
{
    return "SELECT id, name, code, status, tray_location_id, owner_user_id, description,
                    started_on, target_on, completed_on, source_id, created_at, updated_at
               FROM projects";
}

function inv_project_row(array $r): array
{
    return [
        'id'             => (int) $r['id'],
        'name'           => $r['name'],
        'code'           => $r['code'],
        'status'         => $r['status'],
        'trayLocationId' => Coerce::int($r['tray_location_id']),
        'ownerUserId'    => Coerce::int($r['owner_user_id']),
        'description'    => $r['description'],
        'startedOn'      => $r['started_on'] ?? null,
        'targetOn'       => $r['target_on'] ?? null,
        'completedOn'    => $r['completed_on'] ?? null,
        'sourceId'       => Coerce::int($r['source_id']),
        'createdAt'      => Coerce::iso($r['created_at'] ?? null),
        'updatedAt'      => Coerce::iso($r['updated_at'] ?? null),
    ];
}

function inv_allocation_sql(): string
{
    return "SELECT pa.id, pa.project_id, pa.hardware_unit_id, pa.hardware_model_id, pa.allocation, pa.quantity,
                   pa.notes, pa.source_id, pa.created_at, pa.updated_at,
                   hu.serial AS unit_serial, hu.asset_tag AS unit_asset_tag, hu.stock_status AS unit_stock_status,
                   hum.make AS unit_make, hum.model AS unit_model,
                   hm.make AS model_make, hm.model AS model_model, hm.hw_type AS model_hw_type
              FROM project_allocations pa
              LEFT JOIN hardware_units hu   ON hu.id = pa.hardware_unit_id
              LEFT JOIN hardware_models hum ON hum.id = hu.model_id
              LEFT JOIN hardware_models hm  ON hm.id = pa.hardware_model_id";
}

function inv_allocation_row(?array $r): ?array
{
    if ($r === null) {
        return null;
    }
    $isUnit = $r['hardware_unit_id'] !== null;
    return [
        'id'              => (int) $r['id'],
        'projectId'       => (int) $r['project_id'],
        'hardwareUnitId'  => Coerce::int($r['hardware_unit_id']),
        'hardwareModelId' => Coerce::int($r['hardware_model_id']),
        'allocation'      => $r['allocation'],
        'quantity'        => Coerce::int($r['quantity']),
        'notes'           => $r['notes'],
        'sourceId'        => Coerce::int($r['source_id']),
        'createdAt'       => Coerce::iso($r['created_at'] ?? null),
        'updatedAt'       => Coerce::iso($r['updated_at'] ?? null),
        // Denormalised display fields: an owned unit's own make/model, else
        // the catalog model this allocation still needs to acquire.
        'make'            => $isUnit ? ($r['unit_make'] ?? null)  : ($r['model_make'] ?? null),
        'model'           => $isUnit ? ($r['unit_model'] ?? null) : ($r['model_model'] ?? null),
        'hwType'          => $isUnit ? null : ($r['model_hw_type'] ?? null),
        'unitSerial'      => $r['unit_serial'] ?? null,
        'unitAssetTag'    => $r['unit_asset_tag'] ?? null,
        'unitStockStatus' => $r['unit_stock_status'] ?? null,
    ];
}
