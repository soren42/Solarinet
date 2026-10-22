<?php
declare(strict_types=1);

/** Hermetic route tests for SoR network allocation review and approval. */
$root = dirname(__DIR__, 2);
$worker = __DIR__ . '/_network_allocator_worker.php';
$pass = 0;
$fail = 0;

function na_check(bool $condition, string $label, string $detail = ''): void
{
    global $pass, $fail;
    if ($condition) {
        $pass++;
        echo "  ok   - $label\n";
    } else {
        $fail++;
        echo '  FAIL - ' . $label . ($detail === '' ? '' : ' [' . $detail . ']') . "\n";
    }
}

function na_run(string $worker, array $vars): array
{
    $env = array_merge($_ENV, $vars);
    $pipes = [];
    $proc = proc_open([PHP_BINARY, $worker], [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    if (!is_resource($proc)) {
        return ['status' => 0, 'body' => null, 'stderr' => 'worker start failed'];
    }
    $stdout = stream_get_contents($pipes[1]);
    $stderr = stream_get_contents($pipes[2]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);
    preg_match('/STATUS:(\d+)/', $stderr, $m);
    $body = json_decode((string) $stdout, true);
    return ['status' => (int) ($m[1] ?? 0), 'body' => is_array($body) ? $body : null, 'stderr' => $stderr];
}

echo "[1] route registration and SoR response shape\n";
require $root . '/dashboard/api/lib/Router.php';
$router = new Router();
$register = require $root . '/dashboard/api/routes/network_allocator.php';
$register($router);
$property = (new ReflectionClass(Router::class))->getProperty('routes');
$property->setAccessible(true);
$routes = $property->getValue($router);
na_check(count($routes) === 2, 'registers one read and one approval route');

$r = na_run($worker, ['NA_SCENARIO' => 'get-overlap', 'NA_METHOD' => 'GET',
    'NA_PATH' => '/api/networks/allocator-eligibility']);
$network = $r['body']['data'][0] ?? [];
na_check($r['status'] === 200 && ($r['body']['ok'] ?? false), 'GET returns a success envelope', $r['stderr']);
na_check(($network['subnets'][0]['gateway_ip'] ?? null) === '172.31.44.1', 'normalizes the mapped IPv4 gateway');
na_check(($network['subnets'][0]['dhcp_pools'] ?? null) === '172.31.44.50–172.31.44.99', 'returns DHCP pool facts');
na_check(($network['ipv4_overlap_count'] ?? 0) === 1, 'flags overlapping segment prefixes');

echo "\n[2] operator gate and approval decisions\n";
$r = na_run($worker, ['NA_SCENARIO' => 'sor-disabled', 'NA_METHOD' => 'POST',
    'NA_PATH' => '/api/networks/1/allocator-eligibility', 'NA_BODY' => '{"eligible":true,"confirm":true}']);
na_check($r['status'] === 403 && ($r['body']['error']['code'] ?? '') === 'forbidden',
    'unauthenticated approval is denied before SoR access');

$r = na_run($worker, ['NA_SCENARIO' => 'approve', 'NA_AUTH' => '1', 'NA_METHOD' => 'POST',
    'NA_PATH' => '/api/networks/1/allocator-eligibility', 'NA_BODY' => '{"eligible":true,"confirm":true}']);
na_check($r['status'] === 200 && ($r['body']['data']['allocator_eligible'] ?? false) === true,
    'operator approval commits and returns eligible state', $r['stderr']);

$r = na_run($worker, ['NA_SCENARIO' => 'post-overlap', 'NA_AUTH' => '1', 'NA_METHOD' => 'POST',
    'NA_PATH' => '/api/networks/1/allocator-eligibility', 'NA_BODY' => '{"eligible":true,"confirm":true}']);
na_check($r['status'] === 409 && ($r['body']['error']['code'] ?? '') === 'conflict',
    'overlapping prefix blocks approval', $r['stderr']);

$r = na_run($worker, ['NA_SCENARIO' => 'post-disabled', 'NA_AUTH' => '1', 'NA_METHOD' => 'POST',
    'NA_PATH' => '/api/networks/1/allocator-eligibility', 'NA_BODY' => '{"eligible":true,"confirm":true}']);
na_check($r['status'] === 409 && ($r['body']['error']['code'] ?? '') === 'conflict',
    'disabled network blocks approval');

$r = na_run($worker, ['NA_SCENARIO' => 'post-no-subnet', 'NA_AUTH' => '1', 'NA_METHOD' => 'POST',
    'NA_PATH' => '/api/networks/1/allocator-eligibility', 'NA_BODY' => '{"eligible":true,"confirm":true}']);
na_check($r['status'] === 409 && ($r['body']['error']['code'] ?? '') === 'conflict',
    'network without one live IPv4 subnet blocks approval');

$r = na_run($worker, ['NA_SCENARIO' => 'approve', 'NA_AUTH' => '1', 'NA_METHOD' => 'POST',
    'NA_PATH' => '/api/networks/1/allocator-eligibility', 'NA_BODY' => '{"eligible":true}']);
na_check($r['status'] === 400 && ($r['body']['error']['code'] ?? '') === 'confirmation_required',
    'approval requires explicit confirmation');

if (getenv('NA_REAL_DB_TEST') === '1') {
    echo "\n[3] real disposable SoR query\n";
    $vars = ['NA_SCENARIO' => 'live-read', 'NA_METHOD' => 'GET',
        'NA_PATH' => '/api/networks/allocator-eligibility'];
    foreach (['SOR_DB_HOST', 'SOR_DB_PORT', 'SOR_DB_NAME', 'SOR_DB_USER', 'SOR_DB_PASS'] as $key) {
        $value = getenv($key);
        if ($value !== false) {
            $vars[$key] = $value;
        }
    }
    $r = na_run($worker, $vars);
    na_check($r['status'] === 200 && ($r['body']['ok'] ?? false) === true
        && is_array($r['body']['data'] ?? null),
        'GET query executes against the disposable migrated MariaDB', $r['stderr']);
}

echo "\n" . ($fail === 0 ? "All $pass network allocator API checks passed.\n" : "$fail of $pass checks failed.\n");
exit($fail === 0 ? 0 : 1);
