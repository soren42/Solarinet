<?php
declare(strict_types=1);

/**
 * Real MariaDB integration check for RackWire's SoR commit.
 *
 * Safety is deliberate: only connects to 127.0.0.1 and an explicitly selected
 * allowlisted scratch DB. Run by the scratch migration workflows, never point
 * this test at live `sor`.
 */

$host = getenv('SOR_DB_HOST') ?: '';
$name = getenv('SOR_DB_NAME') ?: '';
$testDb = getenv('RW_TEST_DB') ?: '';
$user = getenv('SOR_DB_USER') ?: '';
$dbPass = getenv('SOR_DB_PASS') ?: '';
$fixtureUser = getenv('RW_FIXTURE_DB_USER') ?: $user;
$fixturePass = getenv('RW_FIXTURE_DB_PASS') ?: $dbPass;
if (getenv('RW_REAL_DB_TEST') !== '1' || $host !== '127.0.0.1' || $name !== $testDb
    || !in_array($name, ['sor_test', 'solarinet_stage'], true) || $user === '') {
    fwrite(STDERR, "Refusing: use RW_REAL_DB_TEST=1, an allowlisted RW_TEST_DB, and loopback MariaDB.\n");
    exit(2);
}

$pdo = new PDO("mysql:host=127.0.0.1;port=" . (getenv('SOR_DB_PORT') ?: '3306') . ";dbname=$name;charset=utf8mb4", $fixtureUser, $fixturePass, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
    PDO::ATTR_EMULATE_PREPARES => false,
]);
if ($pdo->query('SELECT DATABASE()')->fetchColumn() !== $name) {
    fwrite(STDERR, "Refusing: database identity check failed.\n");
    exit(2);
}
$scratchMarker = $pdo->query("SELECT marker FROM _solarinet_test_scratch WHERE marker='solarinet-ci-scratch-v1'")->fetchColumn();
if ($scratchMarker !== 'solarinet-ci-scratch-v1') {
    fwrite(STDERR, "Refusing: disposable database sentinel is missing.\n");
    exit(2);
}

$sourceId = (int) $pdo->query("SELECT id FROM sources WHERE slug = 'manual' AND deleted_at IS NULL")->fetchColumn();
$token = bin2hex(random_bytes(5));
$nameA = "rw-it-$token-a";
$nameB = "rw-it-$token-b";
$assetA = "SI-TEST-$token-A";
$assetB = "SI-TEST-$token-B";
$label = "rw-it-$token-link";
$planName = "rw-it-$token-plan";
$modelIds = [];
$unitIds = [];
$entityIds = [];
$linkId = null;
$planId = null;
$fail = 0;
$pass = 0;

$check = static function (bool $condition, string $message) use (&$pass, &$fail): void {
    if ($condition) {
        $pass++;
        echo "  ok   - $message\n";
    } else {
        $fail++;
        echo "  FAIL - $message\n";
    }
};
$storedHash = static function (array $row): string {
    $material = json_encode([
        (string) ($row['name'] ?? ''), $row['thumbnail'] ?? null, $row['notes'] ?? null,
        (string) ($row['plan_json'] ?? ''),
    ], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR);
    return hash('sha256', $material);
};

$runRoute = static function (array $plan) use ($user, $dbPass, $testDb): array {
    $env = [
        'RW_METHOD' => 'POST',
        'RW_PATH' => '/api/rackwire/commit',
        'RW_BODY' => json_encode(['dryRun' => false, 'plan' => $plan], JSON_THROW_ON_ERROR),
        'RW_AUTH' => '1',
        'RW_ROLE' => 'operator',
        'RW_SCENARIO' => 'real-db',
        'RW_REAL_DB_TEST' => '1',
        'RW_TEST_DB' => $testDb,
        'RW_SESSION_DIR' => sys_get_temp_dir() . '/rw-real-db-' . getmypid(),
        'SOR_DB_HOST' => '127.0.0.1',
        'SOR_DB_PORT' => getenv('SOR_DB_PORT') ?: '3306',
        'SOR_DB_NAME' => $testDb,
        'SOR_DB_USER' => $user,
        'SOR_DB_PASS' => $dbPass,
    ];
    @mkdir($env['RW_SESSION_DIR'], 0700, true);
    $desc = [1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $proc = proc_open([PHP_BINARY, __DIR__ . '/_rw_worker.php'], $desc, $pipes, null, $env);
    if (!is_resource($proc)) {
        return ['status' => 0, 'body' => null, 'stderr' => 'could not start worker'];
    }
    $stdout = stream_get_contents($pipes[1]);
    $stderr = stream_get_contents($pipes[2]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);
    foreach (glob($env['RW_SESSION_DIR'] . '/*') ?: [] as $sessionFile) {
        @unlink($sessionFile);
    }
    @rmdir($env['RW_SESSION_DIR']);
    preg_match('/STATUS:(\d+)/', $stderr, $statusMatch);
    $body = json_decode($stdout, true);
    return ['status' => (int) ($statusMatch[1] ?? 0), 'body' => $body, 'stderr' => $stderr];
};
$runPlanRoute = static function (array $body, string $path = '/api/rackwire/plans') use ($user, $dbPass, $testDb): array {
    $env = [
        'RW_METHOD' => 'POST', 'RW_PATH' => $path,
        'RW_BODY' => json_encode($body, JSON_THROW_ON_ERROR),
        'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'real-db',
        'RW_REAL_DB_TEST' => '1', 'RW_TEST_DB' => $testDb,
        'RW_SESSION_DIR' => sys_get_temp_dir() . '/rw-plan-real-db-' . getmypid(),
        'SOR_DB_HOST' => '127.0.0.1', 'SOR_DB_PORT' => getenv('SOR_DB_PORT') ?: '3306',
        'SOR_DB_NAME' => $testDb, 'SOR_DB_USER' => $user, 'SOR_DB_PASS' => $dbPass,
    ];
    @mkdir($env['RW_SESSION_DIR'], 0700, true);
    $desc = [1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $proc = proc_open([PHP_BINARY, __DIR__ . '/_rw_worker.php'], $desc, $pipes, null, $env);
    if (!is_resource($proc)) return ['status' => 0, 'body' => null, 'stderr' => 'could not start worker'];
    $stdout = stream_get_contents($pipes[1]);
    $stderr = stream_get_contents($pipes[2]);
    fclose($pipes[1]); fclose($pipes[2]); proc_close($proc);
    foreach (glob($env['RW_SESSION_DIR'] . '/*') ?: [] as $sessionFile) @unlink($sessionFile);
    @rmdir($env['RW_SESSION_DIR']);
    preg_match('/STATUS:(\d+)/', $stderr, $statusMatch);
    return ['status' => (int) ($statusMatch[1] ?? 0), 'body' => json_decode($stdout, true), 'stderr' => $stderr];
};

try {
    $pdo->beginTransaction();
    foreach ([['a', $assetA, $nameA], ['b', $assetB, $nameB]] as [$suffix, $asset, $entityName]) {
        $model = $pdo->prepare(
            'INSERT INTO hardware_models (make, model, hw_type, source_id, asserted_kind)
             VALUES (:make, :model, \'switch\', :src, \'human\')'
        );
        $model->execute([':make' => 'RackWire Test', ':model' => "Model-$token-$suffix", ':src' => $sourceId]);
        $modelId = (int) $pdo->lastInsertId();
        $modelIds[] = $modelId;

        $unit = $pdo->prepare(
            'INSERT INTO hardware_units (model_id, serial, asset_tag, source_id, asserted_kind)
             VALUES (:model, :serial, :tag, :src, \'human\')'
        );
        $unit->execute([':model' => $modelId, ':serial' => "RW-$token-$suffix", ':tag' => $asset, ':src' => $sourceId]);
        $unitId = (int) $pdo->lastInsertId();
        $unitIds[] = $unitId;

        $entity = $pdo->prepare(
            'INSERT INTO entities (name, entity_type, hardware_unit_id, source_id, asserted_kind)
             VALUES (:name, \'network_gear\', :unit, :src, \'human\')'
        );
        $entity->execute([':name' => $entityName, ':unit' => $unitId, ':src' => $sourceId]);
        $entityId = (int) $pdo->lastInsertId();
        $entityIds[] = $entityId;

        $iface = $pdo->prepare(
            'INSERT INTO interfaces (entity_id, name, if_kind, source_id, asserted_kind)
             VALUES (:eid, :name, \'switch_port\', :src, \'human\')'
        );
        $iface->execute([':eid' => $entityId, ':name' => "Ethernet-$suffix", ':src' => $sourceId]);
    }
    $pdo->commit();

    $ports = [['k' => 'lan', 'n' => 'LAN', 'domain' => 'net', 'conn' => 'rj45', 'count' => 1]];
    $plan = [
        'devices' => [
            ['uid' => 'a', 'name' => $nameA, 'assetId' => $assetA,
             'ifMap' => ["Ethernet-a" => 'lan-1'], 'groups' => $ports],
            ['uid' => 'b', 'name' => $nameB, 'assetId' => $assetB,
             'ifMap' => ["Ethernet-b" => 'lan-1'], 'groups' => $ports],
        ],
        'cables' => [[
            'uid' => 'c1', 'a' => ['dev' => 'a', 'port' => 'lan-1'], 'b' => ['dev' => 'b', 'port' => 'lan-1'],
            'standard' => 'cat6a', 'label' => $label, 'lengthM' => 4.25, 'jacket' => 'blue', 'notes' => 'integration check',
        ]],
    ];

    $first = $runRoute($plan);
    if ($first['status'] !== 200) {
        fwrite(STDERR, "RackWire worker status={$first['status']}: {$first['stderr']}\n");
    }
    $check($first['status'] === 200 && ($first['body']['data']['created'] ?? null) === 1,
        'route creates a cable through the real RackWire API');

    $stmt = $pdo->prepare(
        'SELECT id, a_interface_id, b_interface_id, a_entity_id, b_entity_id, link_kind,
                label, length_m, media_standard, jacket_color, notes
           FROM interconnects WHERE label = :label AND deleted_at IS NULL'
    );
    $stmt->execute([':label' => $label]);
    $row = $stmt->fetch();
    $linkId = $row ? (int) $row['id'] : null;
    $entityIfaceIds = $pdo->prepare('SELECT id FROM interfaces WHERE entity_id = :eid AND name = :name');
    $entityIfaceIds->execute([':eid' => $entityIds[0], ':name' => 'Ethernet-a']);
    $expectedAIf = (int) $entityIfaceIds->fetchColumn();
    $entityIfaceIds->execute([':eid' => $entityIds[1], ':name' => 'Ethernet-b']);
    $expectedBIf = (int) $entityIfaceIds->fetchColumn();
    $check($row && (int) $row['a_interface_id'] === $expectedAIf && (int) $row['b_interface_id'] === $expectedBIf
        && $row['a_entity_id'] === null && $row['b_entity_id'] === null,
        'SoR row stores the two resolved interface foreign keys');
    $check($row && $row['link_kind'] === 'copper' && $row['label'] === $label
        && (float) $row['length_m'] === 4.25 && $row['media_standard'] === 'cat6a'
        && $row['jacket_color'] === 'blue' && $row['notes'] === 'integration check',
        'SoR row stores cable kind, label, length, standard, jacket, and notes');

    $second = $runRoute($plan);
    $count = (int) $pdo->query("SELECT COUNT(*) FROM interconnects WHERE label = " . $pdo->quote($label) . " AND deleted_at IS NULL")->fetchColumn();
    $check($second['status'] === 200 && ($second['body']['data']['updated'] ?? null) === 1 && $count === 1,
        'repeating the plan updates the existing cable without duplicating it');

    $plan = ['devices' => [['uid' => 'base']], 'cables' => [], 'racks' => [], 'notes' => [], 'codeMap' => new stdClass()];
    $createdPlan = $runPlanRoute(['name' => $planName, 'thumbnail' => 'fixture-thumbnail', 'notes' => 'fixture plan notes', 'plan' => $plan]);
    $planId = (int) ($createdPlan['body']['data']['id'] ?? 0) ?: null;
    $baseHash = $createdPlan['body']['data']['planHash'] ?? '';
    $check(!$pdo->inTransaction(), 'real DB plan checks run without an outer repeatable-read transaction');
    $check($createdPlan['status'] === 201 && $planId !== null && preg_match('/^[a-f0-9]{64}$/', $baseHash) === 1,
        'real plan create returns a base hash for compare-and-save');

    if ($planId !== null && $baseHash !== '') {
        $winnerPlan = ['devices' => [['uid' => 'winner']], 'cables' => [], 'racks' => [], 'notes' => [], 'codeMap' => new stdClass()];
        $winner = $runPlanRoute(['id' => $planId, 'name' => $planName, 'expectedHash' => $baseHash, 'plan' => $winnerPlan]);
        $winnerRetry = $runPlanRoute(['id' => $planId, 'name' => $planName, 'expectedHash' => $baseHash, 'plan' => $winnerPlan]);
        $stale = $runPlanRoute(['id' => $planId, 'name' => $planName, 'expectedHash' => $baseHash,
            'plan' => ['devices' => [['uid' => 'stale']], 'cables' => [], 'racks' => [], 'notes' => [], 'codeMap' => new stdClass()]]);
        $storedPlan = $pdo->prepare('SELECT name, thumbnail, notes, plan_json FROM rw_plans WHERE id = :id');
        $storedPlan->execute([':id' => $planId]);
        $storedRow = $storedPlan->fetch();
        $storedDevices = json_decode((string) ($storedRow['plan_json'] ?? ''), true)['devices'] ?? [];
        $check($winner['status'] === 201 && isset($winner['body']['data']['planHash'])
            && $storedRow !== false && $storedHash($storedRow) === ($winner['body']['data']['planHash'] ?? null)
            && $winnerRetry['status'] === 201 && ($winnerRetry['body']['data']['planHash'] ?? null) === ($winner['body']['data']['planHash'] ?? null)
            && $stale['status'] === 409 && ($stale['body']['error']['code'] ?? null) === 'plan_conflict'
            && ($winner['body']['data']['thumbnail'] ?? null) === 'fixture-thumbnail'
            && ($winner['body']['data']['notes'] ?? null) === 'fixture plan notes'
            && ($storedDevices[0]['uid'] ?? null) === 'winner',
            'real MariaDB compare-and-save preserves omitted metadata, accepts an exact retry, and rejects a stale writer ('
                . json_encode(['winnerStatus' => $winner['status'], 'winnerError' => $winner['body']['error'] ?? null,
                    'winnerRetryStatus' => $winnerRetry['status'],
                    'thumbnail' => $winner['body']['data']['thumbnail'] ?? null, 'notes' => $winner['body']['data']['notes'] ?? null,
                    'staleStatus' => $stale['status'], 'staleCode' => $stale['body']['error']['code'] ?? null,
                    'storedUid' => $storedDevices[0]['uid'] ?? null]) . ')');

        $insertVersion = $pdo->prepare('INSERT INTO rw_plan_versions (plan_id, version_name, plan_json, created_by, source_id) VALUES (:id, :name, :json, :by, :src)');
        $insertVersion->execute([':id' => $planId, ':name' => 'integration restore',
            ':json' => json_encode(['devices' => [['uid' => 'restored']], 'cables' => [], 'racks' => [], 'notes' => [], 'codeMap' => new stdClass()], JSON_THROW_ON_ERROR),
            ':by' => $user, ':src' => $sourceId]);
        $versionId = (int) $pdo->lastInsertId();
        $staleRestore = $runPlanRoute(['versionId' => $versionId, 'expectedHash' => $baseHash], '/api/rackwire/plans/' . $planId . '/restore');
        $storedPlan->execute([':id' => $planId]);
        $afterStaleRow = $storedPlan->fetch();
        $afterStaleRestore = json_decode((string) ($afterStaleRow['plan_json'] ?? ''), true)['devices'] ?? [];
        $check($staleRestore['status'] === 409 && ($staleRestore['body']['error']['code'] ?? null) === 'plan_conflict'
            && ($afterStaleRestore[0]['uid'] ?? null) === 'winner',
            'real MariaDB snapshot restore rejects a stale base without replacing the winning plan');

        $winnerHash = $winner['body']['data']['planHash'] ?? '';
        $appliedRestore = $runPlanRoute(['versionId' => $versionId, 'expectedHash' => $winnerHash], '/api/rackwire/plans/' . $planId . '/restore');
        $restoreRetry = $runPlanRoute(['versionId' => $versionId, 'expectedHash' => $winnerHash], '/api/rackwire/plans/' . $planId . '/restore');
        $storedPlan->execute([':id' => $planId]);
        $restoredRow = $storedPlan->fetch();
        $check($appliedRestore['status'] === 200 && $restoreRetry['status'] === 200
            && $restoredRow !== false && $storedHash($restoredRow) === ($appliedRestore['body']['data']['planHash'] ?? null)
            && ($appliedRestore['body']['data']['planHash'] ?? null) === ($restoreRetry['body']['data']['planHash'] ?? null)
            && (($restoreRetry['body']['data']['plan']['devices'][0]['uid'] ?? null) === 'restored'),
            'real MariaDB snapshot restore accepts an exact retry after commit');
    }
} catch (Throwable $e) {
    if ($pdo->inTransaction()) {
        $pdo->rollBack();
    }
    fwrite(STDERR, 'RackWire real-DB integration error: ' . $e->getMessage() . "\n");
    $fail++;
} finally {
    if ($pdo->inTransaction()) {
        $pdo->rollBack();
    }
    try {
        if ($linkId !== null) {
            $del = $pdo->prepare('DELETE FROM interconnects WHERE id = :id');
            $del->execute([':id' => $linkId]);
        }
        if ($planId !== null) {
            $del = $pdo->prepare('DELETE FROM rw_plans WHERE id = :id');
            $del->execute([':id' => $planId]);
        }
        foreach ($entityIds as $id) {
            $del = $pdo->prepare('DELETE FROM entities WHERE id = :id');
            $del->execute([':id' => $id]);
        }
        foreach ($unitIds as $id) {
            $del = $pdo->prepare('DELETE FROM hardware_units WHERE id = :id');
            $del->execute([':id' => $id]);
        }
        foreach ($modelIds as $id) {
            $del = $pdo->prepare('DELETE FROM hardware_models WHERE id = :id');
            $del->execute([':id' => $id]);
        }
    } catch (Throwable $e) {
        fwrite(STDERR, 'RackWire real-DB cleanup error: ' . $e->getMessage() . "\n");
        $fail++;
    }
}

echo "\n$pass passed, $fail failed\n";
exit($fail ? 1 : 0);
