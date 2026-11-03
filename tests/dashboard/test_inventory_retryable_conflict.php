<?php
declare(strict_types=1);

// Unit test, no database: an InnoDB deadlock (1213) or lock-wait timeout
// (1205) during an inventory, location or catalog mutation must answer
// 409 conflict ("try again"), never a 500/503. Each case runs in a child
// process because Response::error() exits.
$root = dirname(__DIR__, 2);

if (($argv[1] ?? '') === '--case') {
    require_once $root . '/dashboard/api/lib/Response.php';
    require_once $root . '/dashboard/api/routes/inventory.php';
    require_once $root . '/dashboard/api/routes/location_admin.php';
    $e = new PDOException('SQLSTATE[' . $argv[4] . ']: simulated');
    $e->errorInfo = [$argv[4], (int) $argv[3], 'simulated lock failure'];
    if ($argv[2] === 'inv') {
        inv_retryable_conflict($e);
    } else {
        loc_map_exception($e);
    }
    echo 'PASSTHROUGH';
    exit(0);
}

$failures = 0;
$checks = 0;
$check = static function (bool $ok, string $message) use (&$failures, &$checks): void {
    $checks++;
    if (!$ok) {
        $failures++;
        fwrite(STDERR, "FAIL: $message\n");
    }
};
$run = static function (string $site, int $errno, string $state) use ($root): string {
    $out = [];
    exec(escapeshellarg(PHP_BINARY) . ' ' . escapeshellarg(__FILE__) . ' --case '
        . escapeshellarg($site) . ' ' . $errno . ' ' . escapeshellarg($state) . ' 2>&1', $out);
    return implode("\n", $out);
};

foreach (['inv', 'loc'] as $site) {
    foreach ([[1213, '40001'], [1205, 'HY000']] as [$errno, $state]) {
        $body = json_decode($run($site, $errno, $state), true);
        $check(($body['error']['code'] ?? null) === 'conflict'
            && str_contains((string) ($body['error']['message'] ?? ''), 'try again'),
            "$site errno $errno did not map to a retryable conflict");
    }
}
$check($run('inv', 1062, '23000') === 'PASSTHROUGH', 'a duplicate key was treated as retryable');
$dup = json_decode($run('loc', 1062, '23000'), true);
$check(($dup['error']['code'] ?? null) === 'duplicate_location', 'loc_map_exception lost its duplicate mapping');

// Every write path routes through the shared mapper: Inv::exec and
// Inv::transaction (inventory.php), loc_map_exception, and each catalog catch.
$inventory = (string) file_get_contents($root . '/dashboard/api/routes/inventory.php');
$check(substr_count($inventory, 'inv_retryable_conflict($e);') >= 2, 'Inv::exec/Inv::transaction do not map lock failures');
$catalog = (string) file_get_contents($root . '/dashboard/api/routes/product_catalog.php');
// A Throwable catch directly after a PDOException catch never sees a PDO
// error, so only first-in-chain catches are required to map.
preg_match_all('/catch\s*\(\s*(?:PDOException|Throwable)\s+\$e\s*\)\s*\{[^}]*\}/', $catalog, $catches, PREG_OFFSET_CAPTURE);
$first = [];
$prevEnd = -1;
foreach ($catches[0] as [$text, $offset]) {
    if (!($prevEnd >= 0 && trim(substr($catalog, $prevEnd, $offset - $prevEnd)) === '')) {
        $first[] = $text;
    }
    $prevEnd = $offset + strlen($text);
}
$mapped = array_filter($first, static fn(string $c): bool => str_contains($c, 'inv_retryable_conflict($e)'));
$check(count($first) >= 5 && count($mapped) === count($first),
    'a product_catalog catch block does not map lock failures (' . count($mapped) . '/' . count($first) . ')');

if ($failures > 0) {
    exit(1);
}
echo "inventory retryable-conflict mapping passed ($checks checks)\n";
