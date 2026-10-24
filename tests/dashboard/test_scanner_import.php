<?php
declare(strict_types=1);

// Exercise the dashboard import planner against the disposable CI SoR schema.
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
require_once __DIR__ . '/../../dashboard/api/lib/ScannerImport.php';

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}
function throws(callable $fn, string $part): void
{
    try { $fn(); } catch (RuntimeException $e) {
        check(str_contains($e->getMessage(), $part), "unexpected conflict: {$e->getMessage()}");
        return;
    }
    throw new RuntimeException("expected import conflict containing '$part'");
}
function rejects(callable $fn, string $part): void
{
    try { $fn(); } catch (InvalidArgumentException $e) {
        check(str_contains($e->getMessage(), $part), "unexpected validation error: {$e->getMessage()}");
        return;
    }
    throw new RuntimeException("expected import validation containing '$part'");
}

$db = new PDO("mysql:host=$host;port=$port;dbname=$name;charset=utf8mb4", getenv('SOR_DB_USER') ?: '', getenv('SOR_DB_PASS') ?: '', [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
]);
$suffix = bin2hex(random_bytes(6));
$itemId = sprintf('%08x-%04x-4%03x-a%03x-%012x', random_int(0, 0xffffffff), random_int(0, 0xffff), random_int(0, 0xfff), random_int(0, 0xfff), random_int(0, 0xffffffffffff));
$assetId = sprintf('%08x-%04x-4%03x-a%03x-%012x', random_int(0, 0xffffffff), random_int(0, 0xffff), random_int(0, 0xfff), random_int(0, 0xfff), random_int(0, 0xffffffffffff));
$tag = 'SCAN-' . strtoupper($suffix);
$serial = 'SER-' . strtoupper($suffix);
$make = 'Scanner fixture ' . $suffix;
$model = 'Temporary item ' . $suffix;
$locationName = 'Scanner fixture ' . $suffix;
$operator = 'scanner-test';
$sourceBefore = $db->query("SELECT id FROM sources WHERE slug='scanner-ingest'")->fetchColumn();
$locationId = null;
$modelId = null;
$collationModelId = null;
$driftModelId = null;
$unitId = null;
$refIds = [];
$barcodeIds = [];
$snapshot = [
    'schema_version' => 1,
    'exported_at' => gmdate('c'),
    'items' => [[
        'id' => $itemId, 'name' => $model, 'manufacturer' => $make, 'model' => $model,
        'category' => 'scanner', 'barcode' => 'PRODUCT-' . strtoupper($suffix), 'notes' => '',
        'created_at' => gmdate('c'), 'updated_at' => gmdate('c'), 'version' => 1, 'archived' => 0,
    ]],
    'assets' => [[
        'id' => $assetId, 'item_id' => $itemId, 'tag' => $tag, 'serial' => $serial,
        'location' => $locationName, 'status' => 'in_stock', 'notes' => '',
        'created_at' => gmdate('c'), 'updated_at' => gmdate('c'), 'version' => 1, 'archived' => 0,
    ]],
];

$cleanup = static function () use ($db, $itemId, $assetId, $tag, $locationName, $make, $model, $operator, &$locationId, &$modelId, &$collationModelId, &$driftModelId, &$unitId, &$refIds, &$barcodeIds, $sourceBefore): void {
    $sourceId = $db->query("SELECT id FROM sources WHERE slug='scanner-ingest'")->fetchColumn();
    if ($sourceId !== false) {
        $refs = $db->prepare('SELECT id FROM external_refs WHERE source_id=:source AND external_id IN (:item,:asset)');
        $refs->execute([':source' => $sourceId, ':item' => $itemId, ':asset' => $assetId]);
        $refIds = array_unique(array_merge($refIds, array_map('intval', $refs->fetchAll(PDO::FETCH_COLUMN))));
        $units = $db->prepare('SELECT id FROM hardware_units WHERE asset_tag=:tag');
        $units->execute([':tag' => $tag]);
        $unitId = $unitId ?: ($units->fetchColumn() ?: null);
        if ($unitId) {
            $codes = $db->prepare('SELECT id FROM barcodes WHERE subject_table="hardware_units" AND subject_id=:unit');
            $codes->execute([':unit' => $unitId]);
            $barcodeIds = array_unique(array_merge($barcodeIds, array_map('intval', $codes->fetchAll(PDO::FETCH_COLUMN))));
        }
        if (!$modelId) {
            $models = $db->prepare('SELECT id FROM hardware_models WHERE make=:make AND model=:model');
            $models->execute([':make' => $make, ':model' => $model]);
            $modelId = $models->fetchColumn() ?: null;
        }
        if ($driftModelId) $db->prepare('DELETE FROM hardware_models WHERE id=:id')->execute([':id' => $driftModelId]);
        if ($collationModelId) $db->prepare('DELETE FROM hardware_models WHERE id=:id')->execute([':id' => $collationModelId]);
        foreach (['hardware_units' => $unitId, 'hardware_models' => $modelId] as $table => $id) {
            if ($id) $db->prepare('DELETE FROM audit_log WHERE table_name=:table AND row_id=:id AND actor_label=:actor')->execute([':table' => $table, ':id' => $id, ':actor' => 'dashboard:' . $operator]);
        }
        foreach (['external_refs' => $refIds, 'barcodes' => $barcodeIds] as $table => $ids) {
            foreach ($ids as $id) $db->prepare('DELETE FROM audit_log WHERE table_name=:table AND row_id=:id AND actor_label=:actor')->execute([':table' => $table, ':id' => $id, ':actor' => 'dashboard:' . $operator]);
        }
        if ($sourceBefore === false) $db->prepare('DELETE FROM audit_log WHERE table_name="sources" AND row_id=:id AND actor_label=:actor')->execute([':id' => $sourceId, ':actor' => 'dashboard:' . $operator]);
        if ($barcodeIds) $db->exec('DELETE FROM barcodes WHERE id IN (' . implode(',', array_map('intval', $barcodeIds)) . ')');
        if ($refIds) $db->exec('DELETE FROM external_refs WHERE id IN (' . implode(',', array_map('intval', $refIds)) . ')');
        if ($unitId) $db->prepare('DELETE FROM hardware_units WHERE id=:id')->execute([':id' => $unitId]);
        if ($modelId) $db->prepare('DELETE FROM hardware_models WHERE id=:id')->execute([':id' => $modelId]);
        if ($sourceBefore === false) {
            $remaining = $db->prepare('SELECT COUNT(*) FROM external_refs WHERE source_id=:source');
            $remaining->execute([':source' => $sourceId]);
            if ((int) $remaining->fetchColumn() === 0) $db->prepare('DELETE FROM sources WHERE id=:id')->execute([':id' => $sourceId]);
        }
    }
    if ($locationId) $db->prepare('DELETE FROM locations WHERE id=:id')->execute([':id' => $locationId]);
    else $db->prepare('DELETE FROM locations WHERE name=:name')->execute([':name' => $locationName]);
};

try {
    $manualSource = (int) $db->query("SELECT id FROM sources WHERE slug='manual'")->fetchColumn();
    $db->prepare("INSERT INTO locations (name,loc_type,source_id,asserted_kind,asserted_at) VALUES (:name,'bin',:source,'human',NOW())")
        ->execute([':name' => $locationName, ':source' => $manualSource]);
    $locationId = (int) $db->lastInsertId();
    $mapping = ['locations' => [$locationName => $locationId]];
    $uppercaseUuid = $snapshot;
    $uppercaseUuid['items'][0]['id'] = strtoupper($itemId);
    $uppercaseUuid['assets'][0]['item_id'] = strtoupper($itemId);
    rejects(static fn() => ScannerImport::run($db, $operator, $uppercaseUuid, [], false), 'lowercase scanner item UUID');
    $caseVariant = $snapshot;
    $caseVariant['assets'][0]['location'] = strtolower($locationName);
    throws(static fn() => ScannerImport::run($db, $operator, $caseVariant, [], false), 'No unique live SoR location');
    $foldedMake = 'COLLATION-' . strtoupper($suffix);
    $foldedModel = 'MODEL-' . strtoupper($suffix);
    $db->prepare("INSERT INTO hardware_models (make,model,hw_type,source_id,asserted_kind,asserted_at) VALUES (:make,:model,'other',:source,'human',NOW())")
        ->execute([':make' => $foldedMake, ':model' => $foldedModel, ':source' => $manualSource]);
    $collationModelId = (int) $db->lastInsertId();
    $collationSnapshot = $snapshot;
    $collationItemId = sprintf('%08x-%04x-4%03x-a%03x-%012x', random_int(0, 0xffffffff), random_int(0, 0xffff), random_int(0, 0xfff), random_int(0, 0xfff), random_int(0, 0xffffffffffff));
    $collationSnapshot['items'][0]['id'] = $collationItemId;
    $collationSnapshot['items'][0]['manufacturer'] = strtolower($foldedMake);
    $collationSnapshot['items'][0]['model'] = strtolower($foldedModel);
    $collationSnapshot['items'][0]['name'] = strtolower($foldedModel);
    $collationSnapshot['assets'][0]['id'] = sprintf('%08x-%04x-4%03x-a%03x-%012x', random_int(0, 0xffffffff), random_int(0, 0xffff), random_int(0, 0xfff), random_int(0, 0xfff), random_int(0, 0xffffffffffff));
    $collationSnapshot['assets'][0]['item_id'] = $collationItemId;
    $collationSnapshot['assets'][0]['tag'] = 'COLLATION-TAG-' . strtoupper($suffix);
    $collationSnapshot['assets'][0]['serial'] = '';
    $collationPreview = ScannerImport::run($db, $operator, $collationSnapshot, $mapping, false);
    check(($collationPreview['counts']['reuse.hardware_models'] ?? 0) === 1,
        'planner index did not preserve case-insensitive SoR make/model matching');
    $preview = ScannerImport::run($db, $operator, $snapshot, $mapping, false);
    check($preview['mode'] === 'preview' && ($preview['counts']['insert.hardware_units'] ?? 0) === 1, 'preview did not plan one unit');
    check(preg_match('/\A[a-f0-9]{64}\z/', $preview['fingerprint']) === 1, 'preview omitted its plan fingerprint');
    check((int) $db->query("SELECT COUNT(*) FROM hardware_units WHERE asset_tag=" . $db->quote($tag))->fetchColumn() === 0, 'preview wrote inventory');

    $db->prepare("INSERT INTO hardware_models (make,model,hw_type,source_id,asserted_kind,asserted_at) VALUES (:make,:model,'other',:source,'human',NOW())")
        ->execute([':make' => $make, ':model' => $model, ':source' => $manualSource]);
    $driftModelId = (int) $db->lastInsertId();
    throws(static fn() => ScannerImport::run($db, $operator, $snapshot, $mapping, true, null, 'error', $preview['fingerprint']), 'changed since preview');
    $db->prepare('DELETE FROM hardware_models WHERE id=:id')->execute([':id' => $driftModelId]);
    $driftModelId = null;

    $applied = ScannerImport::run($db, $operator, $snapshot, $mapping, true, null, 'error', $preview['fingerprint']);
    check($applied['mode'] === 'applied' && ($applied['counts']['insert.hardware_units'] ?? 0) === 1, 'apply did not create one unit');
    $st = $db->prepare('SELECT hu.id,hu.model_id,hu.location_id,hu.stock_status FROM hardware_units hu WHERE hu.asset_tag=:tag');
    $st->execute([':tag' => $tag]);
    $unit = $st->fetch();
    check(is_array($unit) && (int) $unit['location_id'] === $locationId && $unit['stock_status'] === 'in_stock', 'unit fields were not imported correctly');
    $unitId = (int) $unit['id'];
    $modelId = (int) $unit['model_id'];
    $replay = ScannerImport::run($db, $operator, $snapshot, $mapping, true);
    check(($replay['counts']['reuse.hardware_units'] ?? 0) === 1 && ($replay['counts']['insert.hardware_units'] ?? 0) === 0, 'same snapshot was not idempotent');

    $changed = $snapshot;
    $changed['assets'][0]['notes'] = 'changed after import';
    throws(static fn() => ScannerImport::run($db, $operator, $changed, $mapping, true), 'differs from SoR state');
    $st = $db->prepare('SELECT notes FROM hardware_units WHERE id=:id');
    $st->execute([':id' => $unitId]);
    check(strpos((string) $st->fetchColumn(), 'changed after import') === false, 'conflicting replay overwrote existing SoR inventory');

    $collision = $snapshot;
    $collisionItem = sprintf('%08x-%04x-4%03x-a%03x-%012x', random_int(0, 0xffffffff), random_int(0, 0xffff), random_int(0, 0xfff), random_int(0, 0xfff), random_int(0, 0xffffffffffff));
    $collision['items'][0]['id'] = $collisionItem;
    $collision['items'][0]['model'] .= ' collision';
    $collision['items'][0]['name'] .= ' collision';
    $collision['assets'][0]['item_id'] = $collisionItem;
    $collision['assets'][0]['id'] = sprintf('%08x-%04x-4%03x-a%03x-%012x', random_int(0, 0xffffffff), random_int(0, 0xffff), random_int(0, 0xfff), random_int(0, 0xfff), random_int(0, 0xffffffffffff));
    throws(static fn() => ScannerImport::run($db, $operator, $collision, $mapping, true), 'collides with existing SoR inventory');
    $st = $db->prepare('SELECT COUNT(*) FROM hardware_models WHERE make=:make AND model=:model');
    $st->execute([':make' => $make, ':model' => $model . ' collision']);
    check((int) $st->fetchColumn() === 0, 'failed collision left a partial catalog model');

    $barcode = $db->prepare('SELECT id FROM barcodes WHERE subject_table="hardware_units" AND subject_id=:id');
    $barcode->execute([':id' => $unitId]);
    $barcodeIds = array_map('intval', $barcode->fetchAll(PDO::FETCH_COLUMN));
    $refs = $db->prepare('SELECT id FROM external_refs WHERE source_id=(SELECT id FROM sources WHERE slug="scanner-ingest") AND external_id IN (:item,:asset)');
    $refs->execute([':item' => $itemId, ':asset' => $assetId]);
    $refIds = array_map('intval', $refs->fetchAll(PDO::FETCH_COLUMN));
    echo "Scanner import preview, apply, idempotent replay, and no-overwrite checks passed\n";
} catch (Throwable $e) {
    fwrite(STDERR, 'Scanner import test failed: ' . $e->getMessage() . "\n");
    $cleanup();
    exit(1);
} finally {
    $cleanup();
}
