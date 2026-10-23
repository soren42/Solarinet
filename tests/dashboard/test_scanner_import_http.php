<?php
declare(strict_types=1);

/* Exercise the real scanner import API over HTTP against loopback scratch SoR. */
if (getenv('SOLARI_TEST_SOR_DB') !== '1') {
    fwrite(STDERR, "SOLARI_TEST_SOR_DB=1 is required\n");
    exit(2);
}
$host = getenv('SOR_DB_HOST') ?: '';
$port = getenv('SOR_DB_PORT') ?: '3306';
$name = getenv('SOR_DB_NAME') ?: '';
if (!in_array($host, ['127.0.0.1', 'localhost'], true)
    || !preg_match('/^solarinet_[a-z0-9_]+$/', $name) || !ctype_digit($port)) {
    fwrite(STDERR, "Refusing to use a non-local or non-scratch SoR database\n");
    exit(2);
}

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

function uuidV4(): string
{
    $bytes = random_bytes(16);
    $bytes[6] = chr((ord($bytes[6]) & 0x0f) | 0x40);
    $bytes[8] = chr((ord($bytes[8]) & 0x3f) | 0x80);
    $hex = bin2hex($bytes);
    return substr($hex, 0, 8) . '-' . substr($hex, 8, 4) . '-'
        . substr($hex, 12, 4) . '-' . substr($hex, 16, 4) . '-' . substr($hex, 20);
}

function httpRequest(string $url, string $method, ?array $body = null,
                     ?string $cookie = null, ?string $csrf = null): array
{
    $host = (string) parse_url($url, PHP_URL_HOST);
    $port = (int) parse_url($url, PHP_URL_PORT);
    $origin = "http://{$host}:{$port}";
    $headers = ['Accept: application/json', 'Origin: ' . $origin];
    if ($body !== null) {
        $headers[] = 'Content-Type: application/json';
    }
    if ($cookie !== null) $headers[] = 'Cookie: ' . $cookie;
    if ($csrf !== null) $headers[] = 'X-Solari-CSRF: ' . $csrf;
    $options = ['http' => [
        'method' => $method,
        'header' => implode("\r\n", $headers),
        'content' => $body === null ? '' : json_encode($body, JSON_THROW_ON_ERROR),
        'ignore_errors' => true,
        'timeout' => 5,
    ]];
    $context = stream_context_create($options);
    $response = file_get_contents($url, false, $context);
    if (!is_string($response)) throw new RuntimeException('HTTP fixture request failed.');
    $status = 0;
    $setCookie = null;
    foreach ($http_response_header ?? [] as $header) {
        if (preg_match('/^HTTP\/\S+\s+(\d{3})/', $header, $m)) $status = (int) $m[1];
        if (stripos($header, 'Set-Cookie:') === 0
            && preg_match('/^Set-Cookie:\s*([^;]+)/i', $header, $m)) $setCookie = $m[1];
    }
    $decoded = json_decode($response, true);
    return ['status' => $status, 'body' => is_array($decoded) ? $decoded : [], 'cookie' => $setCookie];
}

function fixtureSession(string $baseUrl, string $role): array
{
    $result = httpRequest($baseUrl . '/__fixture_session?role=' . rawurlencode($role), 'GET');
    check($result['status'] === 200 && is_string($result['cookie']), 'test session fixture did not establish a cookie');
    $csrf = $result['body']['csrf'] ?? null;
    check(is_string($csrf) && preg_match('/\A[a-f0-9]{64}\z/', $csrf) === 1, 'test session fixture did not return a CSRF token');
    return [$result['cookie'], $csrf];
}

$db = new PDO(
    "mysql:host=$host;port=$port;dbname=$name;charset=utf8mb4",
    getenv('SOR_DB_USER') ?: '', getenv('SOR_DB_PASS') ?: '',
    [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC]
);
$suffix = bin2hex(random_bytes(6));
$itemId = uuidV4();
$assetId = uuidV4();
$tag = 'HTTP-SCAN-' . strtoupper($suffix);
$serial = 'HTTP-SER-' . strtoupper($suffix);
$make = 'Scanner HTTP fixture ' . $suffix;
$model = 'Temporary HTTP item ' . $suffix;
$overflowVendor = 'HTTP-PAGE-' . strtoupper($suffix);
$locationName = 'Scanner HTTP fixture ' . $suffix;
$operator = 'scanner-http-test';
$sourceBefore = $db->query("SELECT id FROM sources WHERE slug='scanner-ingest'")->fetchColumn();
$locationId = null;
$work = sys_get_temp_dir() . '/solarinet-scanner-http-' . bin2hex(random_bytes(8));
$sessions = $work . '/sessions';
$web = $work . '/web';
$server = null;
$testFailure = null;

$snapshot = [
    'schema_version' => 1,
    'exported_at' => gmdate('c'),
    'items' => [[
        'id' => $itemId, 'name' => $model, 'manufacturer' => $make, 'model' => $model,
        'category' => 'scanner-http-test', 'barcode' => 'PRODUCT-' . strtoupper($suffix),
        'notes' => '', 'created_at' => gmdate('c'), 'updated_at' => gmdate('c'),
        'version' => 1, 'archived' => 0,
    ]],
    'assets' => [[
        'id' => $assetId, 'item_id' => $itemId, 'tag' => $tag, 'serial' => $serial,
        'location' => $locationName, 'status' => 'in_stock', 'notes' => '',
        'created_at' => gmdate('c'), 'updated_at' => gmdate('c'),
        'version' => 1, 'archived' => 0,
    ]],
];

$cleanup = static function () use ($db, $itemId, $assetId, $tag, $make, $model, $operator, $overflowVendor,
                                   &$locationId, $sourceBefore): void {
    $db->prepare('DELETE FROM receipts WHERE vendor=:vendor')->execute([':vendor' => $overflowVendor]);
    $unitQuery = $db->prepare('SELECT id FROM hardware_units WHERE asset_tag=:tag');
    $unitQuery->execute([':tag' => $tag]);
    $unitId = $unitQuery->fetchColumn();
    if ($unitId !== false) {
        $barcodes = $db->prepare('SELECT id FROM barcodes WHERE subject_table="hardware_units" AND subject_id=:id');
        $barcodes->execute([':id' => $unitId]);
        $barcodeIds = array_map('intval', $barcodes->fetchAll(PDO::FETCH_COLUMN));
        if ($barcodeIds !== []) {
            $in = implode(',', $barcodeIds);
            $db->exec("DELETE FROM audit_log WHERE table_name='barcodes' AND row_id IN ($in) AND actor_label=" . $db->quote('dashboard:' . $operator));
            $db->exec("DELETE FROM barcodes WHERE id IN ($in)");
        }
        $db->prepare('DELETE FROM audit_log WHERE table_name="hardware_units" AND row_id=:id AND actor_label=:actor')
            ->execute([':id' => $unitId, ':actor' => 'dashboard:' . $operator]);
        $db->prepare('DELETE FROM hardware_units WHERE id=:id')->execute([':id' => $unitId]);
    }
    $modelQuery = $db->prepare('SELECT id FROM hardware_models WHERE make=:make AND model=:model');
    $modelQuery->execute([':make' => $make, ':model' => $model]);
    $modelId = $modelQuery->fetchColumn();
    if ($modelId !== false) {
        $db->prepare('DELETE FROM audit_log WHERE table_name="hardware_models" AND row_id=:id AND actor_label=:actor')
            ->execute([':id' => $modelId, ':actor' => 'dashboard:' . $operator]);
        $db->prepare('DELETE FROM hardware_models WHERE id=:id')->execute([':id' => $modelId]);
    }
    $sourceId = $db->query("SELECT id FROM sources WHERE slug='scanner-ingest'")->fetchColumn();
    if ($sourceId !== false) {
        $refs = $db->prepare('SELECT id FROM external_refs WHERE source_id=:source AND external_id IN (:item,:asset)');
        $refs->execute([':source' => $sourceId, ':item' => $itemId, ':asset' => $assetId]);
        $ids = array_map('intval', $refs->fetchAll(PDO::FETCH_COLUMN));
        if ($ids !== []) {
            $in = implode(',', $ids);
            $db->exec("DELETE FROM audit_log WHERE table_name='external_refs' AND row_id IN ($in) AND actor_label=" . $db->quote('dashboard:' . $operator));
            $db->exec("DELETE FROM external_refs WHERE id IN ($in)");
        }
        if ($sourceBefore === false) {
            $remaining = $db->prepare('SELECT COUNT(*) FROM external_refs WHERE source_id=:source');
            $remaining->execute([':source' => $sourceId]);
            if ((int) $remaining->fetchColumn() === 0) {
                $db->prepare('DELETE FROM audit_log WHERE table_name="sources" AND row_id=:id AND actor_label=:actor')
                    ->execute([':id' => $sourceId, ':actor' => 'dashboard:' . $operator]);
                $db->prepare('DELETE FROM sources WHERE id=:id')->execute([':id' => $sourceId]);
            }
        }
    }
    if ($locationId !== null) $db->prepare('DELETE FROM locations WHERE id=:id')->execute([':id' => $locationId]);
};

try {
    $manualSource = $db->query("SELECT id FROM sources WHERE slug='manual'")->fetchColumn();
    check($manualSource !== false, 'manual provenance source is missing from SoR fixture schema');
    $db->prepare("INSERT INTO locations (name,loc_type,source_id,asserted_kind,asserted_at) VALUES (:name,'bin',:source,'human',NOW())")
        ->execute([':name' => $locationName, ':source' => $manualSource]);
    $locationId = (int) $db->lastInsertId();

    if (!mkdir($work, 0700) || !mkdir($sessions, 0700) || !mkdir($web, 0700)) {
        throw new RuntimeException('Could not create private HTTP test directories.');
    }
    $apiIndex = var_export(__DIR__ . '/../../dashboard/api/index.php', true);
    $fixtureRouter = "<?php\n"
        . "\$path = parse_url(\$_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);\n"
        . "if (\$path === '/__fixture_session') {\n"
        . " require_once " . var_export(__DIR__ . '/../../dashboard/api/lib/Auth.php', true) . ";\n"
        . "\$role = (\$_GET['role'] ?? 'operator') === 'viewer' ? 'viewer' : 'operator';\n"
        . " Auth::establishSession(['username' => 'scanner-http-test', 'role' => \$role, 'source' => 'oidc']);\n"
        . " header('Content-Type: application/json'); echo json_encode(['csrf' => Auth::csrfToken()]); exit;\n"
        . "}\n"
        . "if (is_string(\$path) && str_starts_with(\$path, '/api/')) { require " . $apiIndex . "; exit; }\n"
        . "http_response_code(404); echo '{}';\n";
    file_put_contents($work . '/router.php', $fixtureRouter, LOCK_EX);

    $socket = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
    if ($socket === false) throw new RuntimeException('Could not reserve a loopback HTTP port.');
    $socketName = stream_socket_get_name($socket, false);
    fclose($socket);
    $httpPort = (int) substr(strrchr((string) $socketName, ':'), 1);
    $serverLogPath = $work . '/php-server.log';
    $log = fopen($serverLogPath, 'ab');
    if ($log === false) throw new RuntimeException('Could not create HTTP fixture log.');
    $descriptors = [0 => ['file', '/dev/null', 'r'], 1 => $log, 2 => $log];
    // Exercise the API's full 4 MiB application cap independently of a host
    // php.ini that may set post_max_size below the supported request limit.
    $server = proc_open([PHP_BINARY, '-d', 'memory_limit=128M', '-d', 'post_max_size=5M', '-d', 'session.save_path=' . $sessions,
        '-S', '127.0.0.1:' . $httpPort, $work . '/router.php'], $descriptors, $pipes, dirname(__DIR__, 2));
    if (!is_resource($server)) throw new RuntimeException('Could not start the local PHP HTTP fixture.');
    $ready = false;
    for ($i = 0; $i < 50; $i++) {
        $probe = @fsockopen('127.0.0.1', $httpPort, $probeErrno, $probeError, 0.1);
        if (is_resource($probe)) { fclose($probe); $ready = true; break; }
        usleep(100000);
    }
    if (!$ready) {
        $state = proc_get_status($server);
        $serverLog = is_file($serverLogPath) ? trim((string) file_get_contents($serverLogPath)) : '';
        throw new RuntimeException('local PHP HTTP fixture did not start (exit '
            . (string) ($state['exitcode'] ?? -1) . '): ' . $serverLog);
    }

    $baseUrl = 'http://127.0.0.1:' . $httpPort;
    $payload = ['snapshot' => $snapshot, 'mapping' => [
        'locations' => [$locationName => $locationId],
        'categories' => ['scanner-http-test' => 'scanner'],
    ]];
    $anonymous = httpRequest($baseUrl . '/api/inventory/scanner-import/preview', 'POST', $payload);
    check($anonymous['status'] === 401, 'anonymous scanner preview was not rejected');

    [$viewerCookie, $viewerCsrf] = fixtureSession($baseUrl, 'viewer');
    $viewer = httpRequest($baseUrl . '/api/inventory/scanner-import/preview', 'POST', $payload, $viewerCookie, $viewerCsrf);
    check($viewer['status'] === 403, 'viewer session was allowed to preview inventory import');

    [$cookie, $csrf] = fixtureSession($baseUrl, 'operator');
    $legacyInventory = httpRequest($baseUrl . '/api/inventory', 'GET', null, $cookie);
    $legacyData = $legacyInventory['body']['data'] ?? [];
    check($legacyInventory['status'] === 200 && is_array($legacyData['models'] ?? null)
        && is_array($legacyData['units'] ?? null) && !isset($legacyData['page']),
        'small combined inventory read did not preserve its legacy response shape');
    $missingCsrf = httpRequest($baseUrl . '/api/inventory/scanner-import/preview', 'POST', $payload, $cookie);
    check($missingCsrf['status'] === 403, 'scanner preview accepted a missing CSRF token');
    $capacitySnapshot = $snapshot;
    $capacitySnapshot['assets'] = [];
    for ($i = 0; $i < 5000; $i++) {
        $capacitySnapshot['assets'][] = [
            'id' => uuidV4(), 'item_id' => $itemId, 'tag' => 'CAP-' . $suffix . '-' . $i,
            'serial' => 'CAP-SER-' . $suffix . '-' . $i, 'location' => $locationName,
            'status' => 'in_stock', 'notes' => '', 'created_at' => gmdate('c'),
            'updated_at' => gmdate('c'), 'version' => 1, 'archived' => 0,
        ];
    }
    $capacityPayload = ['snapshot' => $capacitySnapshot, 'mapping' => $payload['mapping']];
    $capacityBytes = strlen(json_encode($capacityPayload, JSON_THROW_ON_ERROR));
    check($capacityBytes > 1048576 && $capacityBytes < 4 * 1024 * 1024,
        '5,000-asset schema-version-1 fixture must exceed 1 MiB and fit below 4 MiB');
    $capacityPreview = httpRequest($baseUrl . '/api/inventory/scanner-import/preview', 'POST', $capacityPayload, $cookie, $csrf);
    check($capacityPreview['status'] === 200
        && ($capacityPreview['body']['data']['counts']['insert.hardware_units'] ?? 0) === 5000,
        'scanner import did not preview all 5,000 assets within the request limit (HTTP ' . $capacityPreview['status']
        . ', response ' . json_encode($capacityPreview['body'], JSON_UNESCAPED_SLASHES) . ')');
    $bulkRows = $db->prepare('SELECT COUNT(*) FROM hardware_units WHERE asset_tag LIKE :tag');
    $bulkRows->execute([':tag' => 'CAP-' . $suffix . '-%']);
    check((int) $bulkRows->fetchColumn() === 0, '5,000-asset preview wrote inventory');
    $oversizedPayload = $payload + ['contractPadding' => str_repeat('x', 4 * 1024 * 1024)];
    $oversized = httpRequest($baseUrl . '/api/inventory/scanner-import/preview', 'POST', $oversizedPayload, $cookie, $csrf);
    check($oversized['status'] === 413, 'scanner preview accepted a request larger than 4 MiB');
    $preview = httpRequest($baseUrl . '/api/inventory/scanner-import/preview', 'POST', $payload, $cookie, $csrf);
    check($preview['status'] === 200 && ($preview['body']['ok'] ?? false) === true, 'operator preview failed');
    $previewData = $preview['body']['data'] ?? [];
    $fingerprint = $previewData['fingerprint'] ?? null;
    check(is_string($fingerprint) && preg_match('/\A[a-f0-9]{64}\z/', $fingerprint) === 1, 'preview did not return a SHA-256 fingerprint');
    check(($previewData['counts']['insert.hardware_units'] ?? 0) === 1, 'preview did not plan one inventory unit');
    $count = $db->prepare('SELECT COUNT(*) FROM hardware_units WHERE asset_tag=:tag');
    $count->execute([':tag' => $tag]);
    check((int) $count->fetchColumn() === 0, 'HTTP preview wrote inventory');

    $missingFingerprint = httpRequest($baseUrl . '/api/inventory/scanner-import/apply', 'POST', $payload, $cookie, $csrf);
    check($missingFingerprint['status'] === 400, 'apply did not require the preview fingerprint');
    $anonymousApply = httpRequest($baseUrl . '/api/inventory/scanner-import/apply', 'POST', $payload);
    check($anonymousApply['status'] === 401, 'anonymous scanner apply was not rejected');
    $viewerApply = httpRequest($baseUrl . '/api/inventory/scanner-import/apply', 'POST', $payload, $viewerCookie, $viewerCsrf);
    check($viewerApply['status'] === 403, 'viewer session was allowed to apply an inventory import');
    $missingCsrfApply = httpRequest($baseUrl . '/api/inventory/scanner-import/apply', 'POST', $payload, $cookie);
    check($missingCsrfApply['status'] === 403, 'scanner apply accepted a missing CSRF token');
    $applyPayload = $payload + ['expectedFingerprint' => $fingerprint];
    $applied = httpRequest($baseUrl . '/api/inventory/scanner-import/apply', 'POST', $applyPayload, $cookie, $csrf);
    check($applied['status'] === 200 && ($applied['body']['data']['mode'] ?? '') === 'applied', 'HTTP apply failed');
    check(($applied['body']['data']['counts']['insert.hardware_units'] ?? 0) === 1, 'HTTP apply did not insert one unit');
    $type = $db->prepare('SELECT hm.hw_type FROM hardware_units hu JOIN hardware_models hm ON hm.id=hu.model_id WHERE hu.asset_tag=:tag');
    $type->execute([':tag' => $tag]);
    check($type->fetchColumn() === 'scanner', 'HTTP category mapping did not set the SoR hardware type');
    $profileModelQuery = $db->prepare('SELECT id FROM hardware_models WHERE make=:make AND model=:model AND deleted_at IS NULL');
    $profileModelQuery->execute([':make' => $make, ':model' => $model]);
    $profileModelId = (int) $profileModelQuery->fetchColumn();
    check($profileModelId > 0, 'scanner import did not create the model required for RackWire profiling');
    $profileGroups = [['k' => 'ethernet', 'label' => 'Ethernet ×8', 'glyph' => 'rj45', 'count' => 8,
        'rows' => 1, 'role' => 'lan', 'gbps' => 1, 'domain' => 'net', 'conn' => 'rj45']];
    $profileSave = httpRequest($baseUrl . '/api/rackwire/library/' . $profileModelId . '/groups', 'POST',
        ['name' => $make . ' ' . $model, 'kind' => 'switch', 'drawW' => 18, 'groups' => $profileGroups], $cookie, $csrf);
    check($profileSave['status'] === 201
        && ($profileSave['body']['data']['groups'][0]['count'] ?? null) === 8
        && ($profileSave['body']['data']['drawW'] ?? null) === 18,
        'operator could not save scanner-imported model as a RackWire device profile');
    $profileRead = httpRequest($baseUrl . '/api/rackwire/library', 'GET', null, $cookie);
    $profileRows = $profileRead['body']['data'] ?? [];
    $savedProfile = null;
    foreach ($profileRows as $row) if ((int) ($row['id'] ?? 0) === $profileModelId) $savedProfile = $row;
    check($profileRead['status'] === 200 && is_array($savedProfile)
        && ($savedProfile['groups'][0]['conn'] ?? null) === 'rj45',
        'RackWire library did not read back the scanner-imported model profile');
    $profileViewer = httpRequest($baseUrl . '/api/rackwire/library/' . $profileModelId . '/groups', 'POST',
        ['groups' => $profileGroups], $viewerCookie, $viewerCsrf);
    check($profileViewer['status'] === 403, 'viewer session was allowed to modify a RackWire model profile');
    $profileNoCsrf = httpRequest($baseUrl . '/api/rackwire/library/' . $profileModelId . '/groups', 'POST',
        ['groups' => $profileGroups], $cookie);
    check($profileNoCsrf['status'] === 403, 'RackWire profile write accepted a missing CSRF token');

    $modelTotal = (int) $db->query('SELECT COUNT(*) FROM hardware_models WHERE deleted_at IS NULL')->fetchColumn();
    $modelPage1 = httpRequest($baseUrl . '/api/inventory/models?limit=1&offset=0', 'GET', null, $cookie);
    $pageData = $modelPage1['body']['data'] ?? [];
    check($modelPage1['status'] === 200 && count($pageData['items'] ?? []) === 1,
        'paged model list did not return one item');
    check(($pageData['page']['limit'] ?? null) === 1 && ($pageData['page']['offset'] ?? null) === 0
        && ($pageData['page']['total'] ?? null) === $modelTotal,
        'paged model list returned incorrect metadata');
    check(($pageData['page']['hasMore'] ?? null) === ($modelTotal > 1),
        'paged model list reported incorrect hasMore state');
    if ($modelTotal > 1) {
        $modelPage2 = httpRequest($baseUrl . '/api/inventory/models?limit=1&offset=1', 'GET', null, $cookie);
        $items2 = $modelPage2['body']['data']['items'] ?? [];
        check($modelPage2['status'] === 200 && count($items2) === 1
            && $items2[0]['id'] !== $pageData['items'][0]['id'],
            'paged model list repeated or omitted a row at the page boundary');
    }
    $pagedLists = [
        [
            'components',
            '/api/inventory/components?limit=1&offset=0',
            'SELECT COUNT(*) FROM hardware_units hu JOIN hardware_models hm ON hm.id=hu.model_id WHERE hu.deleted_at IS NULL AND hm.deleted_at IS NULL',
        ],
        [
            'receipts',
            '/api/inventory/receipts?limit=1&offset=0',
            'SELECT COUNT(*) FROM receipts WHERE deleted_at IS NULL',
        ],
        [
            'projects',
            '/api/inventory/projects?limit=1&offset=0',
            'SELECT COUNT(*) FROM projects WHERE deleted_at IS NULL',
        ],
        [
            'locations',
            '/api/inventory/locations?flat=1&limit=1&offset=0',
            'SELECT COUNT(*) FROM locations WHERE deleted_at IS NULL',
        ],
    ];
    foreach ($pagedLists as [$listName, $path, $countSql]) {
        $expectedTotal = (int) $db->query($countSql)->fetchColumn();
        $first = httpRequest($baseUrl . $path, 'GET', null, $cookie);
        $firstData = $first['body']['data'] ?? [];
        $firstItems = $firstData['items'] ?? null;
        check($first['status'] === 200 && is_array($firstItems) && count($firstItems) <= 1,
            "paged {$listName} list did not honor its page size");
        check(($firstData['page']['total'] ?? null) === $expectedTotal
            && ($firstData['page']['hasMore'] ?? null) === ($expectedTotal > count($firstItems)),
            "paged {$listName} list returned incorrect metadata");
        if ($expectedTotal > 1) {
            $second = httpRequest($baseUrl . str_replace('offset=0', 'offset=1', $path), 'GET', null, $cookie);
            $secondItems = $second['body']['data']['items'] ?? [];
            check($second['status'] === 200 && count($secondItems) === 1
                && $secondItems[0]['id'] !== $firstItems[0]['id'],
                "paged {$listName} list repeated or omitted a row at the page boundary");
        }
    }
    $allocationPage = httpRequest($baseUrl . '/api/inventory/allocations?limit=10&offset=0', 'GET', null, $cookie);
    check($allocationPage['status'] === 200
        && is_array($allocationPage['body']['data']['items'] ?? null)
        && ($allocationPage['body']['data']['page']['total'] ?? null) === 0,
        'paged allocation list did not return a valid empty page');
    $badPage = httpRequest($baseUrl . '/api/inventory/models?limit=0&offset=0', 'GET', null, $cookie);
    check($badPage['status'] === 400, 'inventory pagination accepted a zero page size');

    $stale = httpRequest($baseUrl . '/api/inventory/scanner-import/apply', 'POST', $applyPayload, $cookie, $csrf);
    check($stale['status'] === 409, 'a preview fingerprint was reusable after SoR state changed');
    $replayPreview = httpRequest($baseUrl . '/api/inventory/scanner-import/preview', 'POST', $payload, $cookie, $csrf);
    check($replayPreview['status'] === 200, 'idempotent replay preview failed');
    $replayData = $replayPreview['body']['data'] ?? [];
    check(($replayData['counts']['reuse.hardware_units'] ?? 0) === 1, 'repeat preview did not plan an idempotent reuse');
    $replay = httpRequest($baseUrl . '/api/inventory/scanner-import/apply', 'POST',
        $payload + ['expectedFingerprint' => $replayData['fingerprint']], $cookie, $csrf);
    check($replay['status'] === 200 && ($replay['body']['data']['counts']['reuse.hardware_units'] ?? 0) === 1,
        'fingerprinted idempotent replay failed');
    $overflowReceipt = $db->prepare('INSERT INTO receipts (vendor,currency) VALUES (:vendor,\'USD\')');
    for ($i = 0; $i < 501; $i++) $overflowReceipt->execute([':vendor' => $overflowVendor]);
    $oversizedCombined = httpRequest($baseUrl . '/api/inventory', 'GET', null, $cookie);
    check($oversizedCombined['status'] === 413
        && ($oversizedCombined['body']['error']['code'] ?? '') === 'inventory_page_required',
        'combined inventory route did not reject an oversized legacy response with paging guidance');
    echo "Scanner import HTTP auth, pagination, bounded aggregate, preview, fingerprint, apply, stale-plan, and replay checks passed\n";
} catch (Throwable $e) {
    fwrite(STDERR, 'Scanner import HTTP test failed: ' . $e->getMessage() . "\n");
    $testFailure = $e;
} finally {
    if (is_resource($server)) {
        proc_terminate($server);
        proc_close($server);
    }
    $cleanup();
    if (isset($work) && is_dir($work)) {
        $iterator = new RecursiveIteratorIterator(
            new RecursiveDirectoryIterator($work, FilesystemIterator::SKIP_DOTS),
            RecursiveIteratorIterator::CHILD_FIRST
        );
        foreach ($iterator as $path) $path->isDir() ? rmdir($path->getPathname()) : unlink($path->getPathname());
        rmdir($work);
    }
}
if ($testFailure !== null) exit(1);
