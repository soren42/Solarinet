<?php
declare(strict_types=1);

// This test only runs against the disposable CI SoR schema. It intentionally
// exercises the public SoR-first mirror methods while injecting mid-operation
// SQL failures to prove the related writes and their retries are atomic.
if (getenv('SOLARI_TEST_SOR_DB') !== '1') {
    fwrite(STDERR, "SOLARI_TEST_SOR_DB=1 is required\n");
    exit(2);
}
$host = getenv('SOR_DB_HOST') ?: '';
$port = getenv('SOR_DB_PORT') ?: '3306';
$name = getenv('SOR_DB_NAME') ?: '';
if (!in_array($host, ['127.0.0.1', 'localhost'], true)
    || !preg_match('/^solarinet_[a-z0-9_]+$/', $name)
    || !ctype_digit($port)) {
    fwrite(STDERR, "Refusing to use a non-local or non-scratch SoR database\n");
    exit(2);
}

require_once __DIR__ . '/../../dashboard/api/lib/Sor.php';

if (($argv[1] ?? '') === '--upsert-worker') {
    Sor::upsertHost((string) ($argv[2] ?? ''), (string) ($argv[3] ?? ''));
    exit(0);
}

function expect(bool $ok, string $message): void
{
    if (!$ok) {
        throw new RuntimeException($message);
    }
}

function concurrentUpserts(string $ip, string $name): void
{
    $workers = [];
    for ($i = 0; $i < 4; $i++) {
        $pipes = [];
        $process = proc_open([PHP_BINARY, __FILE__, '--upsert-worker', $ip, $name], [
            0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w'],
        ], $pipes);
        if (!is_resource($process)) {
            throw new RuntimeException('Could not start SoR concurrency worker.');
        }
        fclose($pipes[0]);
        $workers[] = [$process, $pipes[1], $pipes[2]];
    }
    foreach ($workers as [$process, $stdout, $stderr]) {
        stream_get_contents($stdout);
        stream_get_contents($stderr);
        fclose($stdout);
        fclose($stderr);
        expect(proc_close($process) === 0, 'concurrent SoR upsert worker failed');
    }
}

$dsn = "mysql:host={$host};port={$port};dbname={$name};charset=utf8mb4";
$db = new PDO($dsn, getenv('SOR_DB_USER') ?: '', getenv('SOR_DB_PASS') ?: '', [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
]);
$suffix = bin2hex(random_bytes(6));
$entityName = 'f22-' . $suffix;
$ip = '';
$checkIp = $db->prepare('SELECT 1 FROM ip_addresses WHERE address = INET6_ATON(:address) AND deleted_at IS NULL');
for ($attempt = 0; $attempt < 254; $attempt++) {
    $candidate = '198.51.100.' . random_int(1, 254);
    $checkIp->execute([':address' => '::ffff:' . $candidate]);
    if ($checkIp->fetchColumn() === false) {
        $ip = $candidate;
        break;
    }
}
if ($ip === '') {
    throw new RuntimeException('No unused fixture IP address was available.');
}
$insertTrigger = 'f22_fail_ip_' . $suffix;
$deleteTrigger = 'f22_fail_entity_' . $suffix;
$outboxBeforeFailure = (int) $db->query('SELECT COALESCE(MAX(id), 0) FROM sor_outbox')->fetchColumn();
$cleanup = static function () use ($db, $entityName, $ip, $insertTrigger, $deleteTrigger): void {
    $db->exec("DROP TRIGGER IF EXISTS `{$insertTrigger}`");
    $db->exec("DROP TRIGGER IF EXISTS `{$deleteTrigger}`");
    $st = $db->prepare('SELECT id FROM entities WHERE name = :name');
    $st->execute([':name' => $entityName]);
    $id = $st->fetchColumn();
    if ($id !== false) {
        $db->prepare('DELETE FROM ip_addresses WHERE entity_id = :id')->execute([':id' => $id]);
        $db->prepare('DELETE FROM entities WHERE id = :id')->execute([':id' => $id]);
    }
};

try {
    $db->exec("CREATE TRIGGER `{$insertTrigger}` BEFORE INSERT ON ip_addresses FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'injected F-22 insert failure'");
    expect(Sor::upsertHost($ip, $entityName) === false, 'upsert did not report the injected failure to its caller');
    $st = $db->prepare('SELECT COUNT(*) FROM entities WHERE name = :name');
    $st->execute([':name' => $entityName]);
    expect((int) $st->fetchColumn() === 0, 'failed address insert left an orphan entity');
    $st = $db->prepare('SELECT COUNT(*) FROM sor_outbox WHERE id > :id');
    $st->execute([':id' => $outboxBeforeFailure]);
    expect((int) $st->fetchColumn() === 0, 'failed host insert emitted a partial SoR outbox event');
    $db->exec("DROP TRIGGER `{$insertTrigger}`");

    concurrentUpserts($ip, $entityName); // first insert and retries race on one address
    expect(Sor::upsertHost($ip, $entityName) === true, 'same-state host replay did not succeed');
    $st = $db->prepare('SELECT e.id, e.deleted_at, COUNT(ip.id) AS address_count FROM entities e LEFT JOIN ip_addresses ip ON ip.entity_id = e.id AND ip.deleted_at IS NULL WHERE e.name = :name GROUP BY e.id, e.deleted_at');
    $st->execute([':name' => $entityName]);
    $row = $st->fetch();
    expect(is_array($row) && $row['deleted_at'] === null && (int) $row['address_count'] === 1,
        'successful/retried upsert did not converge to one live entity and address');
    $entityId = (int) $row['id'];
    $st = $db->prepare('SELECT COUNT(*) FROM sor_outbox WHERE table_name = \'entities\' AND row_id = :id');
    $st->execute([':id' => $entityId]);
    expect((int) $st->fetchColumn() === 1, 'same-state host replay emitted more than one entity outbox event');
    $st = $db->prepare('SELECT id FROM ip_addresses WHERE entity_id = :id AND deleted_at IS NULL');
    $st->execute([':id' => $entityId]);
    $ipId = (int) $st->fetchColumn();
    $st = $db->prepare('SELECT COUNT(*) FROM sor_outbox WHERE table_name = \'ip_addresses\' AND row_id = :id');
    $st->execute([':id' => $ipId]);
    expect($ipId > 0 && (int) $st->fetchColumn() === 1, 'host retry produced other than one live address and insert event');

    $db->exec("CREATE TRIGGER `{$deleteTrigger}` BEFORE UPDATE ON entities FOR EACH ROW BEGIN IF OLD.id = {$entityId} AND OLD.deleted_at IS NULL AND NEW.deleted_at IS NOT NULL THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'injected F-22 remove failure'; END IF; END");
    expect(Sor::removeHost($ip) === false, 'remove did not report the injected failure to its caller');
    $st = $db->prepare('SELECT e.deleted_at AS entity_deleted, ip.deleted_at AS address_deleted FROM entities e JOIN ip_addresses ip ON ip.entity_id = e.id WHERE e.id = :id');
    $st->execute([':id' => $entityId]);
    $row = $st->fetch();
    expect(is_array($row) && $row['entity_deleted'] === null && $row['address_deleted'] === null,
        'failed entity soft-delete left the address partially removed');
    $db->exec("DROP TRIGGER `{$deleteTrigger}`");

    expect(Sor::removeHost($ip) === true, 'host removal retry did not succeed');
    expect(Sor::removeHost($ip) === true, 'replay of a completed remove did not remain idempotent');
    $st = $db->prepare('SELECT e.deleted_at AS entity_deleted, ip.deleted_at AS address_deleted FROM entities e JOIN ip_addresses ip ON ip.entity_id = e.id WHERE e.id = :id');
    $st->execute([':id' => $entityId]);
    $row = $st->fetch();
    expect(is_array($row) && $row['entity_deleted'] !== null && $row['address_deleted'] !== null,
        'successful/retried remove did not soft-delete entity and address together');

    echo "SoR host mirror transaction, rollback, concurrent retry, and idempotency checks passed\n";
} catch (Throwable $error) {
    fwrite(STDERR, 'SoR transaction test failed: ' . $error->getMessage() . "\n");
    $cleanup();
    exit(1);
} finally {
    $cleanup();
}
