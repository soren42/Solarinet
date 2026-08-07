<?php
declare(strict_types=1);

/**
 * test_rackwire_api.php — CONTRACT-RW.md §6: instantiate Router, register the
 * rackwire route group, assert the §2 route table and the envelope/auth
 * behaviour of dashboard/api/routes/rackwire.php.
 *
 * Response::ok()/Response::error() both exit() the process, so each simulated
 * request runs in its own `php` subprocess (tests/dashboard/_rw_worker.php),
 * driven by env vars and read back via stdout (the JSON envelope) + a
 * "STATUS:nnn" line _rw_worker.php's shutdown handler writes to stderr. The
 * DB is never real: _rw_worker.php swaps Sor's PDO for a FakePdo keyed off a
 * named scenario (see rw_worker_scenario() there).
 *
 * Pattern: tests/dashboard/test_solari_ctl.php (plain executable, nonzero
 * exit on any failure). Run: php tests/dashboard/test_rackwire_api.php
 */

$ROOT   = dirname(__DIR__, 2);
$WORKER = __DIR__ . '/_rw_worker.php';

$fail = 0;
$pass = 0;

function rw_test_ok(bool $cond, string $label, string $extra = ''): void
{
    global $pass, $fail;
    if ($cond) {
        $pass++;
        echo "  ok   - $label\n";
    } else {
        $fail++;
        echo "  FAIL - $label" . ($extra !== '' ? "  [$extra]" : '') . "\n";
    }
}

/**
 * Run one simulated request in a fresh subprocess.
 *
 * @param array<string,mixed> $env RW_METHOD, RW_PATH, RW_QUERY, RW_BODY (raw
 *     JSON string), RW_AUTH ('1' to establish a session), RW_ROLE,
 *     RW_SCENARIO (FakePdo dataset name, or 'sor-disabled' to leave Sor
 *     unconfigured and exercise the real 503 path).
 * @return array{status:int,body:array<string,mixed>|null,raw:string}
 */
function rw_worker_run(string $php, string $worker, array $env): array
{
    $fullEnv = $_ENV;
    foreach ($env as $k => $v) {
        $fullEnv[$k] = (string) $v;
    }
    $desc = [1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $proc = proc_open([$php, $worker], $desc, $pipes, null, $fullEnv);
    if (!is_resource($proc)) {
        fwrite(STDERR, "FAIL: could not spawn worker\n");
        exit(1);
    }
    $stdout = stream_get_contents($pipes[1]);
    $stderr = stream_get_contents($pipes[2]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);

    $status = 0;
    if (preg_match('/STATUS:(\d+)/', $stderr, $m) === 1) {
        $status = (int) $m[1];
    }
    $decoded = json_decode((string) $stdout, true);
    return ['status' => $status, 'body' => is_array($decoded) ? $decoded : null, 'raw' => (string) $stdout];
}

$php = PHP_BINARY !== '' ? PHP_BINARY : 'php';

// Hermetic session storage: every worker that establishes a session writes its
// session file here rather than into PHP's configured save path, so the suite
// runs unchanged in a sandbox with a read-only HOME. Removed on exit.
$SESSION_DIR = sys_get_temp_dir() . '/rw-test-sessions-' . getmypid();
@mkdir($SESSION_DIR, 0700, true);
register_shutdown_function(static function () use ($SESSION_DIR): void {
    foreach (glob($SESSION_DIR . '/*') ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($SESSION_DIR);
});

$run = static fn (array $env) => rw_worker_run($php, $WORKER, $env + ['RW_SESSION_DIR' => $SESSION_DIR]);

// ===========================================================================
// [1] Route table — §2's endpoint list, registered on a fresh Router.
// ===========================================================================
echo "[1] route table\n";
{
    require $ROOT . '/dashboard/api/lib/Router.php';
    require $ROOT . '/dashboard/api/lib/Response.php';
    require $ROOT . '/dashboard/api/lib/Sor.php';
    require $ROOT . '/dashboard/api/lib/Operator.php';
    require $ROOT . '/dashboard/api/lib/Coerce.php';

    $router = new Router();
    $register = require $ROOT . '/dashboard/api/routes/rackwire.php';
    $register($router);

    $rc = new ReflectionClass(Router::class);
    $prop = $rc->getProperty('routes');
    $prop->setAccessible(true);
    /** @var array<int,array{method:string,regex:string}> $routes */
    $routes = $prop->getValue($router);

    $expected = [
        ['GET',  '/api/rackwire/plans'],
        ['GET',  '/api/rackwire/plans/{id}'],
        ['GET',  '/api/rackwire/plans/{id}/versions'],
        ['GET',  '/api/rackwire/library'],
        ['GET',  '/api/rackwire/telemetry'],
        ['GET',  '/api/rackwire/sor/interconnects'],
        ['POST', '/api/rackwire/plans'],
        ['POST', '/api/rackwire/plans/{id}/delete'],
        ['POST', '/api/rackwire/plans/{id}/versions'],
        ['POST', '/api/rackwire/plans/{id}/restore'],
        ['POST', '/api/rackwire/library/{modelId}/groups'],
        ['POST', '/api/rackwire/commit'],
    ];

    rw_test_ok(count($routes) === count($expected),
        'registers exactly the §2 route count', 'got ' . count($routes));

    // Router (lib/Router.php) only stores the compiled regex, not the source
    // pattern, so each expected route is proven registered by constructing a
    // literal probe path (placeholders -> "1") and checking some route of the
    // right method matches it via its stored regex.
    foreach ($expected as [$method, $pattern]) {
        $probe = preg_replace('/\{[a-zA-Z_][a-zA-Z0-9_]*\}/', '1', $pattern);
        $found = false;
        foreach ($routes as $r) {
            if ($r['method'] === $method && preg_match($r['regex'], $probe) === 1) {
                $found = true;
                break;
            }
        }
        rw_test_ok($found, "$method $pattern is registered");
    }
}

// ===========================================================================
// [2] Auth gating — every POST route requires an operator, before any DB
//     touch (checked with Sor left unconfigured: a 403 proves
//     Operator::requireOperator() ran first; a 503 here would mean the
//     handler reached the DB before checking auth).
// ===========================================================================
echo "\n[2] mutation auth gating (unauthenticated -> 403, no DB reached)\n";
{
    $mutations = [
        ['POST', '/api/rackwire/plans', ''],
        ['POST', '/api/rackwire/plans/5/delete', ''],
        ['POST', '/api/rackwire/plans/5/versions', ''],
        ['POST', '/api/rackwire/plans/5/restore', '{"versionId":1}'],
        ['POST', '/api/rackwire/library/3/groups', '{"groups":[]}'],
        ['POST', '/api/rackwire/commit', '{"dryRun":true}'],
    ];
    foreach ($mutations as [$method, $path, $body]) {
        $r = $run(['RW_METHOD' => $method, 'RW_PATH' => $path, 'RW_BODY' => $body, 'RW_SCENARIO' => 'sor-disabled']);
        rw_test_ok($r['status'] === 403, "$method $path -> 403 unauthenticated", 'got ' . $r['status'] . ' ' . $r['raw']);
        rw_test_ok(is_array($r['body']) && $r['body']['ok'] === false && $r['body']['error']['code'] === 'forbidden',
            "$method $path -> forbidden envelope", $r['raw']);
    }

    // A viewer role is authenticated but not privileged — still 403.
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/commit', 'RW_BODY' => '{"dryRun":true}',
               'RW_AUTH' => '1', 'RW_ROLE' => 'viewer', 'RW_SCENARIO' => 'sor-disabled']);
    rw_test_ok($r['status'] === 403, 'a viewer role is still forbidden on a mutation', 'got ' . $r['status']);
}

// ===========================================================================
// [3] Envelope shape — success and the documented failure paths.
// ===========================================================================
echo "\n[3] envelope shape\n";
{
    // GET /plans with no rows: 200, ok:true, data: [].
    $r = $run(['RW_METHOD' => 'GET', 'RW_PATH' => '/api/rackwire/plans', 'RW_SCENARIO' => 'empty-list']);
    rw_test_ok($r['status'] === 200 && is_array($r['body']) && $r['body']['ok'] === true
        && $r['body']['data'] === [], 'empty plan list -> 200 ok:true data:[]', $r['raw']);
    rw_test_ok(isset($r['body']['ts']) && is_string($r['body']['ts']), 'success envelope carries an ISO ts', $r['raw']);

    // GET /plans with two rows: shape matches rw_plan_summary_row (no plan_json blob).
    $r = $run(['RW_METHOD' => 'GET', 'RW_PATH' => '/api/rackwire/plans', 'RW_SCENARIO' => 'plan-list-two']);
    rw_test_ok($r['status'] === 200 && count($r['body']['data'] ?? []) === 2, 'two plans listed', $r['raw']);
    $row0 = ($r['body']['data'] ?? [])[0] ?? [];
    rw_test_ok(($row0['id'] ?? null) === 1 && ($row0['name'] ?? null) === 'Rack A'
        && !array_key_exists('plan', $row0), 'list row omits the plan_json blob', json_encode($row0));

    // GET /plans/{id} not found -> 404 not_found.
    $r = $run(['RW_METHOD' => 'GET', 'RW_PATH' => '/api/rackwire/plans/999', 'RW_SCENARIO' => 'plan-not-found']);
    rw_test_ok($r['status'] === 404 && $r['body']['error']['code'] === 'not_found', 'unknown plan id -> 404 not_found', $r['raw']);

    // GET /plans/{id} found -> 200, full row including decoded plan.
    $r = $run(['RW_METHOD' => 'GET', 'RW_PATH' => '/api/rackwire/plans/5', 'RW_SCENARIO' => 'plan-found']);
    rw_test_ok($r['status'] === 200 && ($r['body']['data']['id'] ?? null) === 5
        && is_array($r['body']['data']['plan'] ?? null), 'found plan -> full row with decoded plan', $r['raw']);

    // GET /plans/{id} with a non-numeric id -> 400, never reaches the DB
    // (scenario is deliberately 'plan-not-found' so a pass here proves the
    // id validation ran before any query, not that the stub happened to help).
    $r = $run(['RW_METHOD' => 'GET', 'RW_PATH' => '/api/rackwire/plans/abc', 'RW_SCENARIO' => 'sor-disabled']);
    rw_test_ok($r['status'] === 400 && $r['body']['error']['code'] === 'bad_request',
        'non-numeric plan id -> 400 before any DB access', $r['raw']);

    // GET /telemetry with no keys -> 200, empty devices/ports, no DB at all.
    $r = $run(['RW_METHOD' => 'GET', 'RW_PATH' => '/api/rackwire/telemetry', 'RW_SCENARIO' => 'sor-disabled']);
    rw_test_ok($r['status'] === 200 && ($r['body']['data']['devices'] ?? 'missing') === []
        && ($r['body']['data']['ports'] ?? 'missing') === [], 'telemetry with no keys short-circuits to an empty frame', $r['raw']);

    // GET /library -> 200, HANDOFF §6 shape.
    $r = $run(['RW_METHOD' => 'GET', 'RW_PATH' => '/api/rackwire/library', 'RW_SCENARIO' => 'library-list']);
    $lib0 = ($r['body']['data'] ?? [])[0] ?? [];
    rw_test_ok($r['status'] === 200 && ($lib0['vendor'] ?? null) === 'Ubiquiti'
        && ($lib0['unitCount'] ?? null) === 2 && array_key_exists('groups', $lib0),
        'library row carries vendor/unitCount/groups', json_encode($lib0));

    // GET /sor/interconnects with Sor unconfigured -> 503 service_unavailable
    // (migration 020 documented as "not applied" in this failure path).
    $r = $run(['RW_METHOD' => 'GET', 'RW_PATH' => '/api/rackwire/sor/interconnects', 'RW_SCENARIO' => 'sor-disabled']);
    rw_test_ok($r['status'] === 503 && $r['body']['error']['code'] === 'service_unavailable',
        'unconfigured SoR -> 503 service_unavailable', $r['raw']);

    // Unknown path -> 404 not_found (Router's own fallback).
    $r = $run(['RW_METHOD' => 'GET', 'RW_PATH' => '/api/rackwire/nope', 'RW_SCENARIO' => 'sor-disabled']);
    rw_test_ok($r['status'] === 404, 'unknown rackwire path -> 404', $r['raw']);

    // Known path, wrong method -> 405 method_not_allowed.
    $r = $run(['RW_METHOD' => 'DELETE', 'RW_PATH' => '/api/rackwire/plans', 'RW_SCENARIO' => 'sor-disabled']);
    rw_test_ok($r['status'] === 405 && $r['body']['error']['code'] === 'method_not_allowed',
        'known path, unsupported method -> 405', $r['raw']);
}

// ===========================================================================
// [4] Authenticated mutations reach the (stubbed) DB and write what §2 says.
// ===========================================================================
echo "\n[4] authenticated mutations\n";
{
    // POST /plans create (no id in body) -> 201, upsert INSERT path, id from lastInsertId.
    $body = json_encode(['name' => 'New Rack', 'plan' => ['devices' => [], 'cables' => []]]);
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans', 'RW_BODY' => $body,
               'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'create-plan']);
    rw_test_ok($r['status'] === 201 && ($r['body']['data']['id'] ?? null) === 555
        && ($r['body']['data']['name'] ?? null) === 'New Rack', 'operator create -> 201 with the inserted row', $r['raw']);

    // Same route, missing name -> 400 (validated before any write).
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans',
               'RW_BODY' => json_encode(['plan' => ['devices' => [], 'cables' => []]]),
               'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'sor-disabled']);
    rw_test_ok($r['status'] === 400 && $r['body']['error']['code'] === 'bad_request',
        'create with no name -> 400 before any DB access', $r['raw']);

    // POST /plans/{id}/delete, found -> soft delete acknowledged.
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans/5/delete',
               'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'delete-found']);
    rw_test_ok($r['status'] === 200 && $r['body']['data']['deleted'] === true, 'operator delete of a live plan -> 200 deleted:true', $r['raw']);

    // POST /plans/{id}/delete, already gone -> 404.
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans/5/delete',
               'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'delete-not-found']);
    rw_test_ok($r['status'] === 404, 'operator delete of an already-deleted plan -> 404', $r['raw']);

    // POST /commit dryRun with an admin -> 200, tallies present, nothing "written"
    // (empty plan devices/cables -> created=updated=0, no skips).
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/commit',
               'RW_BODY' => json_encode(['dryRun' => true, 'plan' => ['devices' => [], 'cables' => [], 'racks' => []]]),
               'RW_AUTH' => '1', 'RW_ROLE' => 'admin', 'RW_SCENARIO' => 'commit-empty']);
    rw_test_ok($r['status'] === 200 && $r['body']['data']['dryRun'] === true
        && $r['body']['data']['created'] === 0 && $r['body']['data']['skipped'] === [],
        'admin dry-run commit of an empty plan -> tallies all zero', $r['raw']);

    // POST /plans with an id -> the UPDATE branch. Its stub matches on the
    // full "... AND source_id = :src" clause, so a write that lost its
    // ownership scoping would be an unmatched query, not a silent pass.
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans',
               'RW_BODY' => json_encode(['id' => 5, 'name' => 'Rack A2', 'plan' => ['devices' => [], 'cables' => []]]),
               'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'update-plan']);
    rw_test_ok($r['status'] === 201 && ($r['body']['data']['name'] ?? null) === 'Rack A2',
        'operator update of an owned plan -> 201, UPDATE scoped by source_id', $r['raw']);
}

// ===========================================================================
// [5] Ownership — RackWire never overwrites a row another source asserted
//     (CONTRACT-RW §4), on its own tables as well as the SoR ones. The row
//     exists and the reads still list it, so the refusal is 409, not 404.
// ===========================================================================
echo "\n[5] source ownership on writes\n";
{
    $foreign = ['RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'plan-foreign'];

    $r = $run($foreign + ['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans',
        'RW_BODY' => json_encode(['id' => 5, 'name' => 'Hijack', 'plan' => ['devices' => [], 'cables' => []]])]);
    rw_test_ok($r['status'] === 409 && ($r['body']['error']['code'] ?? null) === 'conflict',
        'update of a foreign-owned plan -> 409 conflict', $r['raw']);

    $r = $run($foreign + ['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans/5/delete']);
    rw_test_ok($r['status'] === 409 && ($r['body']['error']['code'] ?? null) === 'conflict',
        'delete of a foreign-owned plan -> 409 conflict', $r['raw']);

    $r = $run($foreign + ['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans/5/restore',
        'RW_BODY' => json_encode(['versionId' => 1])]);
    rw_test_ok($r['status'] === 409 && ($r['body']['error']['code'] ?? null) === 'conflict',
        'restore onto a foreign-owned plan -> 409 conflict', $r['raw']);

    $r = $run($foreign + ['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans/5/versions',
        'RW_BODY' => json_encode([])]);
    rw_test_ok($r['status'] === 409 && ($r['body']['error']['code'] ?? null) === 'conflict',
        'snapshot of a foreign-owned plan -> 409 conflict', $r['raw']);

    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/library/3/groups',
        'RW_BODY' => json_encode(['groups' => []]),
        'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'groups-foreign']);
    rw_test_ok($r['status'] === 409 && ($r['body']['error']['code'] ?? null) === 'conflict',
        'groups upsert over foreign-owned port geometry -> 409 conflict', $r['raw']);
}

// ===========================================================================
// [6] Snapshot save — insert + count + cap-25 prune, one locked transaction.
// ===========================================================================
echo "\n[6] snapshot save\n";
{
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans/5/versions',
               'RW_BODY' => json_encode(['name' => 'before rewire']),
               'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'save-version']);
    rw_test_ok($r['status'] === 201 && ($r['body']['data']['id'] ?? null) === 77
        && ($r['body']['data']['name'] ?? null) === 'before rewire',
        'snapshot save -> 201 with the new version row', $r['raw']);

    // 27 on file: the prune must fire. Its DELETE has no stub in any other
    // scenario, so reaching 201 here proves the cap logic ran inside the txn.
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/plans/5/versions',
               'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'save-version-prune']);
    rw_test_ok($r['status'] === 201 && ($r['body']['data']['id'] ?? null) === 78,
        'snapshot save past the cap -> 201, oldest pruned in the same transaction', $r['raw']);
}

// ===========================================================================
// [7] Commit link_kind — resolved from the endpoint port's group `domain`
//     and `conn` (HANDOFF §6), not from the cable's standard text, per §4.
// ===========================================================================
echo "\n[7] commit link_kind from port domain\n";
{
    $ports = [
        ['k' => 'sfp', 'count' => 1, 'role' => 'lan',       'domain' => 'net',   'conn' => 'sfp'],
        ['k' => 'lan', 'count' => 1, 'role' => 'lan',       'domain' => 'net',   'conn' => 'rj45'],
        ['k' => 'iec', 'count' => 1, 'role' => 'power-in',  'domain' => 'power', 'conn' => 'iec'],
        ['k' => 'usb', 'count' => 1, 'role' => 'data',      'domain' => 'usb',   'conn' => 'usba'],
    ];
    $plan = [
        'racks'   => [],
        'devices' => [
            ['uid' => 'd1', 'name' => 'sw1', 'groups' => $ports],
            ['uid' => 'd2', 'name' => 'sw2', 'groups' => $ports],
        ],
        'cables'  => [
            ['uid' => 'c-fiber', 'a' => ['dev' => 'd1', 'port' => 'sfp-1'], 'b' => ['dev' => 'd2', 'port' => 'sfp-1'],
             'standard' => 'om3', 'category' => 'OM3 duplex LC + SFP+ SR'],
            ['uid' => 'c-copper', 'a' => ['dev' => 'd1', 'port' => 'lan-1'], 'b' => ['dev' => 'd2', 'port' => 'lan-1'],
             'standard' => 'cat6', 'category' => 'Cat6 U/UTP'],
            ['uid' => 'c-power', 'a' => ['dev' => 'd1', 'port' => 'iec-1'], 'b' => ['dev' => 'd2', 'port' => 'iec-1'],
             'standard' => 'c13c14', 'category' => 'IEC C13→C14 (18 AWG)'],
            ['uid' => 'c-usb', 'a' => ['dev' => 'd1', 'port' => 'usb-1'], 'b' => ['dev' => 'd2', 'port' => 'usb-1'],
             'standard' => 'usb3a', 'category' => 'USB 3.2 Gen1 Type-A'],
            ['uid' => 'c-unknown', 'a' => ['dev' => 'd1', 'port' => 'zzz-9'], 'b' => ['dev' => 'd2', 'port' => 'zzz-9'],
             'standard' => 'cat6', 'category' => 'Cat6 U/UTP'],
        ],
    ];
    $r = $run(['RW_METHOD' => 'POST', 'RW_PATH' => '/api/rackwire/commit',
               'RW_BODY' => json_encode(['dryRun' => true, 'plan' => $plan]),
               'RW_AUTH' => '1', 'RW_ROLE' => 'operator', 'RW_SCENARIO' => 'commit-cables']);
    $d = $r['body']['data'] ?? [];
    $committed = implode(' | ', $d['committed'] ?? []);
    $skipped   = implode(' | ', $d['skipped'] ?? []);

    rw_test_ok($r['status'] === 200 && ($d['created'] ?? null) === 4,
        'four committable cables, the usb one skipped', $r['raw']);
    rw_test_ok(str_contains($committed, 'c-fiber: fiber') && str_contains($committed, 'port domain net'),
        'sfp/OM3 endpoint -> fiber from the port domain', $committed);
    rw_test_ok(str_contains($committed, 'c-copper: copper'),
        'rj45 endpoint -> copper from the port domain', $committed);
    rw_test_ok(str_contains($committed, 'c-power: power'),
        'iec endpoint -> power from the port domain', $committed);
    rw_test_ok(str_contains($skipped, 'c-usb') && str_contains($skipped, 'no SoR link_kind'),
        'usb endpoint -> skipped with a domain reason', $skipped);
    rw_test_ok(str_contains($committed, 'c-unknown: copper')
        && str_contains($committed, 'no endpoint port resolved'),
        'unresolvable port -> text heuristic, labelled as the fallback', $committed);
}

echo "\n" . $pass . " passed, " . $fail . " failed\n";
exit($fail ? 1 : 0);
