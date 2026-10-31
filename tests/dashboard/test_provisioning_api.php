<?php
declare(strict_types=1);

/**
 * test_provisioning_api.php — /api/provisioning/* (routes/provisioning.php +
 * lib/ProvCatalog.php): authz/CSRF gating, 029-mirroring validation, clean
 * DB-error mapping, soft-delete guards, and the exact IMAGE_REFRESH /
 * PROV_STAGE / STACK_APPLY enqueue contract.
 *
 * Subprocess-per-request (tests/dashboard/_prov_worker.php) because
 * Response::ok()/error() exit. The real Policy guard and route code run; the
 * DB is a FakePdo and SolariCtl is a recorder — nothing real is contacted.
 *
 * Run: php tests/dashboard/test_provisioning_api.php   (nonzero exit on failure)
 */

$WORKER = __DIR__ . '/_prov_worker.php';
$PHP = PHP_BINARY !== '' ? PHP_BINARY : 'php';
$pass = 0;
$fail = 0;
$ALL_CTL = [];
$ANY_DIRECT = false;

function pv_ok(bool $cond, string $label, string $extra = ''): void
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

$SESSION_DIR = sys_get_temp_dir() . '/pv-test-sessions-' . getmypid();
@mkdir($SESSION_DIR, 0700, true);
register_shutdown_function(static function () use ($SESSION_DIR): void {
    foreach (glob($SESSION_DIR . '/*') ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($SESSION_DIR);
});

/**
 * @param array<string,mixed> $o method, path, body (array|string), query,
 *   auth (bool, default true), role, csrf (bool), origin, idem, db (resolvers),
 *   env (extra worker env, e.g. PV_NODES / SOLARI_CTL_SOCK)
 */
function pv(array $o): array
{
    global $PHP, $WORKER, $SESSION_DIR, $ALL_CTL, $ANY_DIRECT;
    $env = getenv();
    foreach (array_keys($env) as $k) {
        if (str_starts_with($k, 'PV_') || str_starts_with($k, 'SOR_DB_')) {
            unset($env[$k]);
        }
    }
    $body = $o['body'] ?? '';
    $env += [
        'PV_METHOD' => $o['method'] ?? 'GET',
        'PV_PATH' => $o['path'],
        'PV_QUERY' => $o['query'] ?? '',
        'PV_BODY' => is_string($body) ? $body : json_encode($body),
        'PV_ROLE' => $o['role'] ?? 'operator',
        'PV_ORIGIN' => $o['origin'] ?? 'self',
        'PV_IDEM' => $o['idem'] ?? '',
        'PV_RESOLVERS' => json_encode($o['db'] ?? []),
        'PV_CAPTURE' => '1',
        'PV_SESSION_DIR' => $SESSION_DIR,
    ];
    if ($o['auth'] ?? true) {
        $env['PV_AUTH'] = '1';
    }
    if (($o['csrf'] ?? true) === false) {
        $env['PV_CSRF'] = '0';
    }
    foreach ($o['env'] ?? [] as $k => $v) {
        $env[$k] = (string) $v;
    }
    $proc = proc_open([$PHP, $WORKER], [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    if (!is_resource($proc)) {
        fwrite(STDERR, "cannot spawn worker\n");
        exit(1);
    }
    $out = (string) stream_get_contents($pipes[1]);
    $err = (string) stream_get_contents($pipes[2]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);
    $status = preg_match('/STATUS:(\d+)/', $err, $m) === 1 ? (int) $m[1] : 0;
    $ctl = [];
    if (preg_match_all('/^CTL:(.*)$/m', $err, $mm)) {
        foreach ($mm[1] as $line) {
            $ctl[] = json_decode($line, true);
        }
    }
    array_push($ALL_CTL, ...$ctl);
    $ANY_DIRECT = $ANY_DIRECT || str_contains($err, 'CTLDIRECT:');
    $cap = preg_match('/^CAPTURE:(.*)$/m', $err, $m) === 1 ? json_decode($m[1], true) : null;
    $decoded = json_decode($out, true);
    return [
        'status' => $status,
        'body' => is_array($decoded) ? $decoded : null,
        'raw' => $out,
        'err' => $err,
        'ctl' => $ctl,
        'sql' => array_map(static fn ($e) => preg_replace('/\s+/', ' ', trim($e['sql'])), $cap['sql'] ?? []),
        'params' => array_map(static fn ($e) => $e['params'], $cap['sql'] ?? []),
        'tx' => $cap['tx'] ?? [],
        'direct' => str_contains($err, 'CTLDIRECT:'),
    ];
}

function pv_msg(array $r): string
{
    return (string) ($r['body']['error']['message'] ?? '');
}

/** True when the response body leaks nothing that looks like SQL/driver text. */
function pv_clean(array $r): bool
{
    foreach (['SQL', 'INSERT', 'UPDATE ', 'SELECT', 'Duplicate entry', 'uq_', 'ck_', 'prov_', 'PDO', 'Lock wait'] as $needle) {
        if (str_contains($r['raw'], $needle)) {
            return false;
        }
    }
    return true;
}

$P = '/api/provisioning';
$SHA = str_repeat('ab', 32);
$IMG = [
    'id' => 4, 'distro' => 'debian', 'family' => 'debian', 'release_name' => '13', 'release_label' => '13 (trixie)',
    'arch' => 'x86_64', 'boot_method' => 'ipxe', 'source_url' => 'https://deb.example/netboot/',
    'checksum_url' => null, 'version' => '13.1', 'sha256' => $SHA, 'fetched_at' => '2026-10-30 12:00:00',
    'install_template' => 'debian-preseed.cfg', 'refresh_policy' => 'point_release', 'netboot_verified' => 0,
    'notes' => null, 'updated_at' => '2026-10-30 12:00:00',
];
$IMG_UNFETCHED = ['sha256' => null, 'fetched_at' => null, 'version' => null] + $IMG;
$STACK_CORE = ['id' => 1, 'slug' => 'core', 'name' => 'Core', 'description' => null, 'locked' => 1,
    'sort_order' => 0, 'updated_at' => '2026-10-30 12:00:00'];
$STACK_WEB = ['id' => 3, 'slug' => 'web', 'name' => 'Web', 'description' => null, 'locked' => 0,
    'sort_order' => 100, 'updated_at' => '2026-10-30 12:00:00'];
$ENTITY_LIVE = ['match' => 'SELECT id FROM entities WHERE id = :id AND deleted_at IS NULL', 'rows' => [['id' => 9]]];
// POST stage: entity + its hardware model (arch precedence) and live MACs.
$ENT_Q = 'FROM entities e LEFT JOIN hardware_units hu';
$ENT_ROW = ['id' => 9, 'name' => 'neon', 'hw_model_id' => 3, 'hw_make' => 'Raspberry Pi', 'hw_model' => 'Pi 5',
    'hw_arch' => 'x86_64'];
$ENT = static fn (array $over = []) => ['match' => $ENT_Q, 'rows' => [$over + $ENT_ROW]];
$MACS = ['match' => 'SELECT mac FROM interfaces WHERE entity_id = :id AND deleted_at IS NULL AND mac IS NOT NULL',
    'rows' => [['mac' => 'aa:bb:cc:00:00:01'], ['mac' => 'AA:BB:CC:00:00:02']]];
$MAC1 = 'aa:bb:cc:00:00:01';
$IMG_Q = 'FROM prov_images WHERE id = :id AND deleted_at IS NULL';
$STACKS_LIVE = static fn (array $slugs) => ['match' => 'FROM prov_stacks WHERE deleted_at IS NULL AND slug IN',
    'rows' => array_map(static fn ($s) => ['slug' => $s], $slugs)];

$writes = [
    ['POST', "$P/images"], ['PUT', "$P/images/4"], ['DELETE', "$P/images/4"],
    ['POST', "$P/software"], ['PUT', "$P/software/5"], ['DELETE', "$P/software/5"],
    ['POST', "$P/software/5/artifacts"], ['DELETE', "$P/software/5/artifacts/7"],
    ['POST', "$P/software/5/requires"], ['DELETE', "$P/software/5/requires/6"],
    ['POST', "$P/software/5/conflicts"], ['DELETE', "$P/software/5/conflicts/6"],
    ['POST', "$P/stacks"], ['PUT', "$P/stacks/3"], ['DELETE', "$P/stacks/3"],
    ['POST', "$P/stacks/3/members"], ['PUT', "$P/stacks/3/members/5"], ['DELETE', "$P/stacks/3/members/5"],
    ['POST', "$P/stacks/3/deps"], ['DELETE', "$P/stacks/3/deps/1"],
    ['POST', "$P/image-refresh"], ['POST', "$P/stage"], ['POST', "$P/apply"],
];
$reads = ["$P/images", "$P/images/4", "$P/software", "$P/software/5", "$P/stacks", "$P/stacks/3", "$P/runs", "$P/runs/8",
    "$P/targets", "$P/log"];

// ---------------------------------------------------------------------------
echo "[1] authorization + CSRF gating\n";
foreach ($writes as [$m, $path]) {
    $r = pv(['method' => $m, 'path' => $path, 'role' => 'viewer', 'body' => ['x' => 1]]);
    pv_ok($r['status'] === 403 && $r['sql'] === [] && $r['ctl'] === [], "viewer $m $path -> 403, no SQL, no ctl",
        "status={$r['status']}");
}
foreach ($reads as $path) {
    $r = pv(['path' => $path, 'auth' => false]);
    pv_ok($r['status'] === 401 && $r['sql'] === [], "unauthenticated GET $path -> 401", "status={$r['status']}");
}
foreach ($writes as [$m, $path]) {
    $r = pv(['method' => $m, 'path' => $path, 'auth' => false, 'body' => ['x' => 1]]);
    pv_ok($r['status'] === 401 && $r['sql'] === [], "unauthenticated $m $path -> 401", "status={$r['status']}");
}
foreach ($writes as [$m, $path]) {
    $r = pv(['method' => $m, 'path' => $path, 'csrf' => false, 'body' => ['x' => 1]]);
    pv_ok($r['status'] === 403 && $r['sql'] === [] && $r['ctl'] === [], "operator $m $path without CSRF token -> 403",
        "status={$r['status']}");
}
foreach (['none', 'evil'] as $origin) {
    foreach ([['POST', "$P/stage"], ['DELETE', "$P/stacks/3"], ['POST', "$P/software"]] as [$m, $path]) {
        $r = pv(['method' => $m, 'path' => $path, 'origin' => $origin, 'body' => ['x' => 1]]);
        pv_ok($r['status'] === 403 && $r['ctl'] === [], "operator $m $path origin=$origin -> 403", "status={$r['status']}");
    }
}

// ---------------------------------------------------------------------------
echo "[2] validation -> 400 (no DB touched)\n";
$bad = [
    'bad stack slug' => ['POST', "$P/stacks", ['slug' => 'Bad_Slug', 'name' => 'X'], 'slug'],
    'slug with trailing newline' => ['POST', "$P/stacks", ['slug' => "core\n", 'name' => 'X'], 'slug'],
    'bad artifact sha256' => ['POST', "$P/software/5/artifacts", ['arch' => 'any', 'url' => 'https://x.example/a.tgz', 'sha256' => 'ABCDEF'], 'sha256'],
    "pkg name '='" => ['POST', "$P/software", ['logical_name' => 'netcat', 'item_type' => 'pkg', 'name_debian' => '='], 'name_debian'],
    'script with body and url' => ['POST', "$P/software", ['logical_name' => 'hello', 'item_type' => 'script',
        'name_debian' => '=', 'spec' => ['body' => 'echo hi', 'url' => 'https://x.example/s.sh', 'sha256' => $SHA]], 'body'],
    'script url without sha256' => ['POST', "$P/software", ['logical_name' => 'hello', 'item_type' => 'script',
        'name_debian' => '=', 'spec' => ['url' => 'https://x.example/s.sh']], 'sha256'],
    'custom build without install' => ['POST', "$P/software", ['logical_name' => 'tool', 'item_type' => 'build',
        'name_debian' => '=', 'version_pin' => '1.2.3', 'spec' => ['source' => ['tarball' => 'https://x.example/t.tgz', 'sha256' => $SHA],
        'system' => 'custom', 'build' => 'make']], 'install'],
    'build spec.commands refused' => ['POST', "$P/software", ['logical_name' => 'tool', 'item_type' => 'build',
        'name_debian' => '=', 'version_pin' => '1.2.3', 'spec' => ['commands' => ['make']]], 'commands'],
    'pi_netboot on x86_64' => ['POST', "$P/images", ['distro' => 'raspios', 'family' => 'debian', 'release_name' => 'bookworm',
        'arch' => 'x86_64', 'boot_method' => 'pi_netboot'], 'pi_netboot'],
    'role without ansible_role' => ['POST', "$P/software", ['logical_name' => 'web-role', 'item_type' => 'role', 'name_debian' => '='], 'ansible_role'],
    'ansible_role on a pkg' => ['POST', "$P/software", ['logical_name' => 'curl', 'name_debian' => 'curl', 'ansible_role' => 'web'], 'ansible_role'],
    'artifact without version_pin' => ['POST', "$P/software", ['logical_name' => 'bin', 'item_type' => 'artifact', 'name_debian' => '='], 'version_pin'],
    'image managed sha256 not writable' => ['PUT', "$P/images/4", ['sha256' => $SHA], 'managed'],
    'unknown field' => ['POST', "$P/stacks", ['slug' => 'web', 'name' => 'Web', 'colour' => 'red'], 'Unknown field'],
    'stage stacks with bad slug' => ['POST', "$P/stage", ['entity' => 9, 'image' => 4, 'stacks' => ['core', 'Web!']], 'stacks'],
    'stage 33 stacks' => ['POST', "$P/stage", ['entity' => 9, 'image' => 4,
        'stacks' => array_map(static fn ($i) => "s$i", range(1, 33))], 'stacks'],
    'stage mac missing' => ['POST', "$P/stage", ['entity' => 9, 'image' => 4, 'stacks' => ['core']], 'mac'],
    'stage mac short' => ['POST', "$P/stage", ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => 'aa:bb:cc:dd:ee'], 'mac'],
    'stage mac non-hex' => ['POST', "$P/stage", ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => 'zz:bb:cc:dd:ee:ff'], 'mac'],
    'stage mac unseparated' => ['POST', "$P/stage", ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => 'aabbccddeeff'], 'mac'],
    'stage mac not a string' => ['POST', "$P/stage", ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => 12], 'mac'],
    'stage client arch refused' => ['POST', "$P/stage", ['entity' => 9, 'image' => 4, 'stacks' => ['core'],
        'mac' => 'aa:bb:cc:00:00:01', 'arch' => 'x86_64'], 'Unknown field'],
    'apply stacks not a list' => ['POST', "$P/apply", ['entity' => 9, 'stacks' => 'core', 'dry_run' => true], 'stacks'],
    'apply empty stacks' => ['POST', "$P/apply", ['entity' => 9, 'stacks' => [], 'dry_run' => true], 'stacks'],
    'apply dry_run string' => ['POST', "$P/apply", ['entity' => 9, 'stacks' => ['core'], 'dry_run' => 'yes'], 'dry_run'],
    'apply dry_run int' => ['POST', "$P/apply", ['entity' => 9, 'stacks' => ['core'], 'dry_run' => 1], 'dry_run'],
    'apply dry_run missing' => ['POST', "$P/apply", ['entity' => 9, 'stacks' => ['core']], 'dry_run'],
    'entity id over 2^32-1' => ['POST', "$P/apply", ['entity' => 4294967296, 'stacks' => ['core'], 'dry_run' => false], 'entity'],
    'entity id zero-padded' => ['POST', "$P/image-refresh", ['image' => '04'], 'image'],
    'path id not numeric' => ['DELETE', "$P/stacks/abc", null, 'id'],
    'stack depends on itself' => ['POST', "$P/stacks/3/deps", ['depends_on_stack_id' => 3], 'itself'],
    'body is a JSON list' => ['POST', "$P/stacks", '[1,2]', 'object'],
];
foreach ($bad as $label => [$m, $path, $body, $needle]) {
    $r = pv(['method' => $m, 'path' => $path, 'body' => $body ?? '']);
    pv_ok($r['status'] === 400 && str_contains(pv_msg($r), $needle) && $r['sql'] === [] && $r['ctl'] === [],
        "$label -> 400 mentioning '$needle'", "status={$r['status']} msg=" . pv_msg($r));
}
$r = pv(['method' => 'POST', 'path' => "$P/apply", 'body' => ['entity' => 9, 'stacks' => ['core'], 'dry_run' => true],
    'idem' => 'bad key!']);
pv_ok($r['status'] === 400 && $r['ctl'] === [], 'malformed Idempotency-Key -> 400 before enqueue', "status={$r['status']}");
$r = pv(['path' => "$P/runs", 'query' => 'limit=201']);
pv_ok($r['status'] === 400 && $r['sql'] === [], 'runs limit=201 -> 400', "status={$r['status']}");
$r = pv(['path' => "$P/runs", 'query' => 'status=bogus']);
pv_ok($r['status'] === 400 && $r['sql'] === [], 'runs status=bogus -> 400', "status={$r['status']}");
$r = pv(['path' => "$P/runs", 'query' => 'image=0']);
pv_ok($r['status'] === 400 && str_starts_with(pv_msg($r), 'image') && $r['sql'] === [], 'runs image=0 -> 400', "status={$r['status']}");

// ---------------------------------------------------------------------------
echo "[3] DB error mapping (no SQL text leaked)\n";
$r = pv(['method' => 'POST', 'path' => "$P/stacks", 'body' => ['slug' => 'web', 'name' => 'Web'], 'db' => [
    ['match' => 'INSERT INTO prov_stacks', 'throw' => true, 'driverCode' => 1062, 'sqlState' => '23000',
     'message' => "SQLSTATE[23000]: Duplicate entry 'web-1' for key 'uq_prov_stacks_slug_live' INSERT INTO prov_stacks"],
]]);
pv_ok($r['status'] === 409 && pv_clean($r) && ($r['body']['error']['code'] ?? '') === 'conflict',
    'duplicate stack slug -> 409, no SQL/driver text', "status={$r['status']} raw={$r['raw']}");
$r = pv(['method' => 'POST', 'path' => "$P/software", 'body' => ['logical_name' => 'curl', 'name_debian' => 'curl'], 'db' => [
    ['match' => 'INSERT INTO prov_software', 'throw' => true, 'driverCode' => 1062, 'sqlState' => '23000',
     'message' => "Duplicate entry 'curl-1' for key 'uq_prov_software_name_live'"],
]]);
pv_ok($r['status'] === 409 && pv_clean($r), 'duplicate software logical_name -> 409, clean', "status={$r['status']}");
$r = pv(['method' => 'POST', 'path' => "$P/stacks", 'body' => ['slug' => 'web', 'name' => 'Web'], 'db' => [
    ['match' => 'INSERT INTO prov_stacks', 'throw' => true, 'driverCode' => 4025, 'sqlState' => '23000',
     'message' => 'CONSTRAINT `ck_prov_stacks_slug` failed for `sor`.`prov_stacks`'],
]]);
pv_ok($r['status'] === 400 && pv_clean($r), 'CHECK violation -> 400, clean', "status={$r['status']}");
$r = pv(['method' => 'DELETE', 'path' => "$P/stacks/3", 'db' => [
    ['match' => 'FROM prov_stacks WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'throw' => true,
     'driverCode' => 1205, 'sqlState' => 'HY000', 'message' => 'Lock wait timeout exceeded; try restarting transaction'],
]]);
pv_ok($r['status'] === 503 && pv_clean($r) && in_array('rollback', $r['tx'], true),
    'lock timeout -> 503 + rollback, clean', "status={$r['status']} tx=" . implode(',', $r['tx']));
$r = pv(['path' => "$P/images", 'db' => [
    ['match' => 'FROM prov_images WHERE deleted_at IS NULL', 'throw' => true, 'driverCode' => 1146, 'sqlState' => '42S02',
     'message' => "Table 'sor.prov_images' doesn't exist SELECT"],
]]);
pv_ok($r['status'] === 500 && pv_clean($r), 'unknown DB error -> 500 db_error, clean', "status={$r['status']}");

// ---------------------------------------------------------------------------
echo "[4] soft-delete guards\n";
$r = pv(['method' => 'DELETE', 'path' => "$P/stacks/1", 'db' => [
    ['match' => 'FROM prov_stacks WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [$STACK_CORE]],
]]);
pv_ok($r['status'] === 409 && str_contains(pv_msg($r), 'locked') && in_array('rollback', $r['tx'], true)
    && !preg_grep('/^UPDATE/', $r['sql']), 'locked stack delete -> 409, rolled back, no UPDATE', "status={$r['status']}");
$r = pv(['method' => 'DELETE', 'path' => "$P/stacks/3", 'db' => [
    ['match' => 'FROM prov_stacks WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [$STACK_WEB]],
    ['match' => 'FROM prov_stack_deps d JOIN prov_stacks s ON s.id = d.stack_id', 'rows' => [['slug' => 'ai']]],
]]);
pv_ok($r['status'] === 409 && str_contains(pv_msg($r), 'ai') && !preg_grep('/^UPDATE/', $r['sql']),
    'stack delete with live dependent -> 409', "status={$r['status']}");
$r = pv(['method' => 'DELETE', 'path' => "$P/stacks/3", 'db' => [
    ['match' => 'FROM prov_stacks WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [$STACK_WEB]],
    ['match' => 'FROM prov_stack_deps d JOIN prov_stacks s ON s.id = d.stack_id', 'rows' => []],
    ['match' => 'UPDATE prov_stacks SET deleted_at = NOW()', 'affected' => 1],
    ['match' => 'UPDATE prov_stack_members SET deleted_at = NOW() WHERE stack_id = :id', 'affected' => 2],
    ['match' => 'UPDATE prov_stack_deps SET deleted_at = NOW() WHERE stack_id = :id', 'affected' => 1],
]]);
pv_ok($r['status'] === 200 && ($r['body']['data']['deleted'] ?? null) === true && $r['tx'] === ['begin', 'commit']
    && count(preg_grep('/^UPDATE/', $r['sql'])) === 3 && !preg_grep('/^DELETE/', $r['sql']),
    'unlocked stack delete -> soft delete of stack + members + deps in one tx', "status={$r['status']}");
$swLock = ['match' => 'FROM prov_software WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [['id' => 5]]];
$r = pv(['method' => 'DELETE', 'path' => "$P/software/5", 'db' => [
    $swLock,
    ['match' => 'FROM prov_stack_members m JOIN prov_stacks s ON s.id = m.stack_id', 'rows' => [['slug' => 'core'], ['slug' => 'web']]],
]]);
pv_ok($r['status'] === 409 && str_contains(pv_msg($r), 'core, web') && !preg_grep('/^UPDATE/', $r['sql']),
    'software delete while a live stack member -> 409', "status={$r['status']} msg=" . pv_msg($r));
$r = pv(['method' => 'DELETE', 'path' => "$P/software/5", 'db' => [
    $swLock,
    ['match' => 'FROM prov_stack_members m JOIN prov_stacks s ON s.id = m.stack_id', 'rows' => []],
    ['match' => 'FROM prov_software_requires r JOIN prov_software p ON p.id = r.software_id', 'rows' => [['logical_name' => 'nginx']]],
]]);
pv_ok($r['status'] === 409 && str_contains(pv_msg($r), 'nginx') && !preg_grep('/^UPDATE/', $r['sql']),
    'software delete while live-required -> 409', "status={$r['status']}");
$r = pv(['method' => 'DELETE', 'path' => "$P/software/5", 'db' => [
    $swLock,
    ['match' => 'FROM prov_stack_members m JOIN prov_stacks s ON s.id = m.stack_id', 'rows' => []],
    ['match' => 'FROM prov_software_requires r JOIN prov_software p ON p.id = r.software_id', 'rows' => []],
    ['match' => 'UPDATE prov_software SET deleted_at', 'affected' => 1],
    ['match' => 'UPDATE prov_software_artifacts SET deleted_at', 'affected' => 0],
    ['match' => 'UPDATE prov_software_requires SET deleted_at', 'affected' => 0],
    ['match' => 'UPDATE prov_software_conflicts SET deleted_at', 'affected' => 0],
]]);
pv_ok($r['status'] === 200 && $r['tx'] === ['begin', 'commit'] && count(preg_grep('/^UPDATE/', $r['sql'])) === 4,
    'unreferenced software delete -> soft delete + cascade links', "status={$r['status']}");
$r = pv(['method' => 'DELETE', 'path' => "$P/images/4", 'db' => [
    ['match' => 'UPDATE prov_images SET deleted_at = NOW()', 'affected' => 0],
]]);
pv_ok($r['status'] === 404, 'delete of a missing image -> 404', "status={$r['status']}");

// ---------------------------------------------------------------------------
echo "[5] catalog writes + reads\n";
$r = pv(['method' => 'POST', 'path' => "$P/stacks", 'body' => ['slug' => 'web', 'name' => 'Web', 'locked' => false], 'db' => [
    ['match' => 'INSERT INTO prov_stacks', 'affected' => 1, 'insertId' => 3],
    ['match' => 'FROM prov_stacks WHERE id = :id AND deleted_at IS NULL', 'rows' => [$STACK_WEB]],
    ['match' => 'FROM prov_stack_members m', 'rows' => []],
    ['match' => 'FROM prov_stack_deps d', 'rows' => []],
]]);
$ins = array_values(preg_grep('/^INSERT INTO prov_stacks/', $r['sql']));
pv_ok($r['status'] === 201 && ($r['body']['data']['slug'] ?? '') === 'web'
    && isset($ins[0]) && str_contains($ins[0], ":src, 'human', NOW()"),
    'create stack -> 201 with provenance (dashboard source, human)', "status={$r['status']}");
$r = pv(['method' => 'POST', 'path' => "$P/software/6/conflicts", 'body' => ['software_id' => 2], 'db' => [
    ['match' => 'FROM prov_software WHERE id = :id AND deleted_at IS NULL', 'rows' => [[
        'id' => 6, 'logical_name' => 'podman', 'item_type' => 'pkg', 'name_suse' => null, 'name_debian' => 'podman',
        'name_fedora' => null, 'applies_families' => null, 'applies_arches' => null, 'ansible_role' => null,
        'source_url' => null, 'version_pin' => null, 'spec' => null, 'notes' => null, 'updated_at' => null]]],
    ['match' => 'INSERT INTO prov_software_conflicts', 'affected' => 1, 'insertId' => 1],
    ['match' => 'FROM prov_software_artifacts', 'rows' => []],
    ['match' => 'FROM prov_software_requires r', 'rows' => []],
    ['match' => 'FROM prov_software_conflicts c', 'rows' => []],
    ['match' => 'FROM prov_stack_members m', 'rows' => []],
]]);
$idx = array_search(true, array_map(static fn ($s) => str_starts_with($s, 'INSERT INTO prov_software_conflicts'), $r['sql']), true);
$cp = $idx === false ? [] : $r['params'][$idx];
pv_ok($r['status'] === 201 && ($cp[':software_lo_id'] ?? null) === 2 && ($cp[':software_hi_id'] ?? null) === 6,
    'conflict stored canonically lo<hi', "status={$r['status']} params=" . json_encode($cp));
$r = pv(['method' => 'PUT', 'path' => "$P/images/4", 'body' => ['source_url' => 'https://deb.example/new/'], 'db' => [
    ['match' => "$IMG_Q FOR UPDATE", 'rows' => [$IMG]],
    ['match' => 'UPDATE prov_images SET', 'affected' => 1],
    ['match' => $IMG_Q, 'rows' => [$IMG_UNFETCHED]],
]]);
$upd = array_values(preg_grep('/^UPDATE prov_images SET/', $r['sql']));
pv_ok($r['status'] === 200 && isset($upd[0]) && str_contains($upd[0], 'sha256 = :sha256')
    && str_contains($upd[0], 'fetched_at = :fetched_at'),
    'changing image source_url resets version/sha256/fetched_at', "status={$r['status']}");
$r = pv(['path' => "$P/images", 'db' => [['match' => 'FROM prov_images WHERE deleted_at IS NULL', 'rows' => [$IMG, $IMG_UNFETCHED]]]]);
pv_ok($r['status'] === 200 && ($r['body']['data'][0]['fetched'] ?? null) === true
    && ($r['body']['data'][1]['fetched'] ?? null) === false, 'GET images (session) with fetched flag', "status={$r['status']}");
$r = pv(['path' => "$P/images", 'role' => 'viewer', 'db' => [['match' => 'FROM prov_images WHERE deleted_at IS NULL', 'rows' => []]]]);
pv_ok($r['status'] === 200, 'viewer may read images', "status={$r['status']}");
$r = pv(['path' => "$P/stacks/3", 'db' => [
    ['match' => 'FROM prov_stacks WHERE id = :id AND deleted_at IS NULL', 'rows' => [$STACK_WEB]],
    ['match' => 'FROM prov_stack_members m', 'rows' => [
        ['software_id' => 10, 'logical_name' => 'nginx-repo', 'item_type' => 'repo', 'item_order' => null,
         'sort_order' => 100, 'notes' => null, 'effective_order' => 100],
        ['software_id' => 11, 'logical_name' => 'nginx', 'item_type' => 'pkg', 'item_order' => null,
         'sort_order' => 100, 'notes' => null, 'effective_order' => 200]]],
    ['match' => 'JOIN prov_stacks s ON s.id = d.depends_on_stack_id', 'rows' => [['id' => 1, 'slug' => 'core']]],
    ['match' => 'JOIN prov_stacks s ON s.id = d.stack_id', 'rows' => []],
]]);
pv_ok($r['status'] === 200 && count($r['body']['data']['members'] ?? []) === 2
    && ($r['body']['data']['depends_on'][0]['slug'] ?? '') === 'core', 'GET stack detail: members + deps', "status={$r['status']}");
$r = pv(['path' => "$P/runs", 'query' => 'entity=9&mode=apply&status=failed&limit=200', 'db' => [
    ['match' => 'FROM prov_runs r', 'rows' => [['id' => '8', 'mode' => 'apply', 'entity_id' => '9', 'entity_name' => 'neon',
        'image_id' => null, 'image_label' => null, 'stack_slugs' => '["core","web"]', 'requested_by' => 'tester',
        'status' => 'failed', 'dry_run' => 1, 'detail' => 'x', 'requested_at' => '2026-10-30 12:00:00',
        'started_at' => null, 'finished_at' => null]]],
]]);
$runSql = $r['sql'][0] ?? '';
pv_ok($r['status'] === 200 && ($r['body']['data'][0]['stack_slugs'] ?? null) === ['core', 'web']
    && str_contains($runSql, 'r.entity_id = :entity') && str_contains($runSql, 'LIMIT 200'),
    'GET runs with filters + limit', "status={$r['status']}");
$r = pv(['path' => "$P/runs/8", 'db' => [['match' => 'FROM prov_runs r', 'rows' => []]]]);
pv_ok($r['status'] === 404, 'GET missing run -> 404', "status={$r['status']}");
// 031 refresh runs: image, no entity. Mixed with an old reimage row.
$runRow = ['id' => '8', 'mode' => 'reimage', 'entity_id' => '9', 'entity_name' => 'neon', 'image_id' => 4,
    'image_label' => 'debian 13 x86_64', 'stack_slugs' => '["core"]', 'requested_by' => 'tester', 'status' => 'succeeded',
    'dry_run' => 0, 'detail' => null, 'requested_at' => '2026-10-30 12:00:00', 'started_at' => null, 'finished_at' => null];
$r = pv(['path' => "$P/runs", 'role' => 'viewer', 'db' => [['match' => 'FROM prov_runs r', 'rows' => [
    ['id' => '11', 'mode' => 'refresh', 'entity_id' => null, 'entity_name' => null, 'stack_slugs' => '[]',
     'status' => 'failed', 'detail' => 'checksum mismatch', 'finished_at' => '2026-10-30 12:05:00'] + $runRow,
    $runRow,
]]]]);
$d = $r['body']['data'] ?? [];
pv_ok($r['status'] === 200 && ($d[0]['mode'] ?? '') === 'refresh' && array_key_exists('entity_id', $d[0] ?? [])
    && $d[0]['entity_id'] === null && ($d[0]['image_id'] ?? null) === 4 && ($d[0]['image_label'] ?? '') === 'debian 13 x86_64'
    && ($d[0]['status'] ?? '') === 'failed' && ($d[0]['stack_slugs'] ?? null) === []
    && ($d[1]['entity_id'] ?? null) === '9' && ($d[1]['mode'] ?? '') === 'reimage',
    'GET runs: refresh row (entity null, image label) alongside a reimage row', json_encode($d[0] ?? null));
$r = pv(['path' => "$P/runs", 'query' => 'mode=refresh&image=4', 'db' => [['match' => 'FROM prov_runs r', 'rows' => []]]]);
$runSql = $r['sql'][0] ?? '';
pv_ok($r['status'] === 200 && str_contains($runSql, 'r.mode = :mode') && str_contains($runSql, 'r.image_id = :image'),
    'GET runs ?mode=refresh&image=4 filters by mode + image', "status={$r['status']}");

// ---------------------------------------------------------------------------
echo "[5a] software list carries live conflict pairs\n";
$swRow = static fn (int $id, string $n) => ['id' => $id, 'logical_name' => $n, 'item_type' => 'pkg',
    'name_suse' => $n, 'name_debian' => $n, 'name_fedora' => $n, 'applies_families' => null, 'applies_arches' => null,
    'ansible_role' => null, 'source_url' => null, 'version_pin' => null, 'spec' => null, 'notes' => null, 'updated_at' => null];
$r = pv(['path' => "$P/software", 'role' => 'viewer', 'db' => [
    ['match' => 'FROM prov_software WHERE deleted_at IS NULL ORDER BY', 'rows' => [$swRow(3, 'exim'), $swRow(5, 'postfix'), $swRow(7, 'curl')]],
    ['match' => 'FROM prov_software_conflicts c', 'rows' => [['lo' => 3, 'hi' => 5]]],
]]);
$cw = array_column($r['body']['data'] ?? [], 'conflicts_with', 'logical_name');
pv_ok($r['status'] === 200 && ($cw['exim'] ?? null) === [5] && ($cw['postfix'] ?? null) === [3] && ($cw['curl'] ?? null) === [],
    'software list: conflicts_with is symmetric, empty when none', json_encode($cw));

echo "[5b] P4 reads: targets + helper log tail\n";
$TGT_Q = 'FROM entities e JOIN interfaces i';
$tgtRows = [
    ['id' => 9, 'name' => 'neon', 'entity_type' => 'server', 'lifecycle' => 'active', 'role' => 'compute',
     'hw_model_id' => 3, 'hw_make' => 'Raspberry Pi', 'hw_model' => 'Pi 5', 'hw_type' => 'sbc', 'hw_arch' => null, 'if_name' => 'eth0',
     'mac' => 'aa:bb:cc:00:00:01', 'is_management' => 1],
    ['id' => 9, 'name' => 'neon', 'entity_type' => 'server', 'lifecycle' => 'active', 'role' => 'compute',
     'hw_model_id' => 3, 'hw_make' => 'Raspberry Pi', 'hw_model' => 'Pi 5', 'hw_type' => 'sbc', 'hw_arch' => null, 'if_name' => 'wlan0',
     'mac' => 'aa:bb:cc:00:00:02', 'is_management' => 0],
    ['id' => 12, 'name' => 'argon', 'entity_type' => 'vm', 'lifecycle' => 'active', 'role' => null,
     'hw_model_id' => null, 'hw_make' => null, 'hw_model' => null, 'hw_type' => null, 'hw_arch' => null, 'if_name' => 'ens18',
     'mac' => 'aa:bb:cc:00:00:03', 'is_management' => 0],
];
$nodes = json_encode([['hostFqdn' => 'neon.akoria.net', 'arch' => 'aarch64'], ['hostFqdn' => 'xenon.akoria.net', 'arch' => 'amd64']]);
$r = pv(['path' => "$P/targets", 'role' => 'viewer', 'env' => ['PV_NODES' => $nodes],
    'db' => [['match' => $TGT_Q, 'rows' => $tgtRows]]]);
$d = $r['body']['data'] ?? [];
$tsql = $r['sql'][0] ?? '';
pv_ok($r['status'] === 200 && count($d) === 2 && ($d[0]['id'] ?? null) === '9'
    && count($d[0]['interfaces'] ?? []) === 2 && ($d[0]['interfaces'][0]['management'] ?? null) === true
    && ($d[0]['interfaces'][1]['management'] ?? null) === false && count($d[1]['interfaces'] ?? []) === 1,
    'GET targets groups interface rows per entity (viewer may read)', "status={$r['status']}");
pv_ok(($d[0]['arch'] ?? null) === 'arm64' && ($d[0]['arch_source'] ?? null) === 'agent'
    && array_key_exists('arch', $d[1] ?? []) && $d[1]['arch'] === null && $d[1]['arch_source'] === null,
    'targets: arch from agent node (aarch64 -> arm64) by short host; none -> null', json_encode([$d[0]['arch'] ?? 0, $d[1]['arch'] ?? 0]));
pv_ok(str_contains($tsql, 'e.deleted_at IS NULL') && str_contains($tsql, 'i.mac IS NOT NULL')
    && str_contains($tsql, "NOT IN ('retired', 'decommissioned')"),
    'targets: live, non-retired entities with a MAC only', $tsql);
// Precedence: hardware_models.arch beats the agent; agent beats unknown.
$hwRows = $tgtRows;
$hwRows[0]['hw_arch'] = $hwRows[1]['hw_arch'] = 'x86_64';   // neon: model says x86_64, agent says aarch64
$r = pv(['path' => "$P/targets", 'env' => ['PV_NODES' => $nodes], 'db' => [['match' => $TGT_Q, 'rows' => $hwRows]]]);
$d = $r['body']['data'] ?? [];
pv_ok($r['status'] === 200 && ($d[0]['arch'] ?? null) === 'x86_64' && ($d[0]['arch_source'] ?? null) === 'hardware_model'
    && ($d[0]['agent_arch'] ?? null) === 'arm64' && ($d[0]['hw_model_id'] ?? null) === 3
    && array_key_exists('hw_model_id', $d[1] ?? []) && $d[1]['hw_model_id'] === null && $d[1]['arch'] === null,
    'targets: hardware_models.arch wins over agent (agent_arch still reported); no model + no agent -> null',
    json_encode(array_map(static fn ($t) => [$t['arch'] ?? 0, $t['arch_source'] ?? 0, $t['agent_arch'] ?? 0], $d)));
$r = pv(['path' => "$P/targets", 'db' => [['match' => $TGT_Q, 'rows' => $hwRows]]]);
pv_ok($r['status'] === 200 && ($r['body']['data'][0]['arch_source'] ?? null) === 'hardware_model'
    && array_key_exists('agent_arch', $r['body']['data'][0] ?? []) && $r['body']['data'][0]['agent_arch'] === null,
    'targets: hardware-model arch still resolves with the monitoring DB down', "status={$r['status']}");
pv_ok(str_contains($tsql, 'hm.arch AS hw_arch'), 'targets SQL reads hardware_models.arch', $tsql);
$r = pv(['path' => "$P/targets", 'db' => [['match' => $TGT_Q, 'rows' => $tgtRows]]]);
pv_ok($r['status'] === 200 && count($r['body']['data'] ?? []) === 2 && array_key_exists('arch', $r['body']['data'][0] ?? []) && $r['body']['data'][0]['arch'] === null,
    'targets still list when the monitoring DB is unavailable (arch null)', "status={$r['status']}");
$r = pv(['path' => "$P/targets", 'db' => [['match' => $TGT_Q, 'throw' => true, 'message' => 'SQLSTATE[HY000] gone']]]);
pv_ok($r['status'] >= 500 && pv_clean($r), 'targets SoR failure -> 5xx without driver text', "status={$r['status']}");

$logDir = sys_get_temp_dir() . '/pv-log-' . getmypid();
@mkdir($logDir, 0700, true);
$LOGENV = ['SOLARI_CTL_SOCK' => "$logDir/solariCtl.sock"];
foreach (['', 'kind=../../etc/passwd&id=1', 'kind=PROV-STAGE&id=1'] as $q) {
    $r = pv(['path' => "$P/log", 'query' => $q, 'env' => $LOGENV]);
    pv_ok($r['status'] === 400 && str_starts_with(pv_msg($r), 'kind'), "log query '$q' -> 400 on kind", "status={$r['status']}");
}
foreach (['kind=prov-stage', 'kind=prov-stage&id=0', 'kind=prov-stage&id=..%2F1', 'kind=prov-stage&id=1.5'] as $q) {
    $r = pv(['path' => "$P/log", 'query' => $q, 'env' => $LOGENV]);
    pv_ok($r['status'] === 400 && str_starts_with(pv_msg($r), 'id'), "log query '$q' -> 400 on id", "status={$r['status']}");
}
$r = pv(['path' => "$P/log", 'query' => 'kind=stack-apply&id=77', 'env' => $LOGENV]);
pv_ok($r['status'] === 200 && ($r['body']['data']['exists'] ?? null) === false && ($r['body']['data']['log'] ?? null) === ''
    && array_key_exists('updated_at', $r['body']['data'] ?? []) && $r['body']['data']['updated_at'] === null, 'log for a run with no file -> exists:false, empty', "status={$r['status']}");
file_put_contents("$logDir/prov-stage-8.log", "fetch ok\n\x1b[32mstaged\x1b[0m\n");
$r = pv(['path' => "$P/log", 'query' => 'kind=prov-stage&id=8', 'role' => 'viewer', 'env' => $LOGENV]);
pv_ok($r['status'] === 200 && ($r['body']['data']['exists'] ?? null) === true
    && ($r['body']['data']['log'] ?? '') === "fetch ok\nstaged\n" && ($r['body']['data']['truncated'] ?? null) === false
    && $r['sql'] === [], 'log tail: ANSI colour stripped, not truncated, no SQL', json_encode($r['body']['data']['log'] ?? null));
$big = '';
for ($i = 0; strlen($big) < 70000; $i++) {
    $big .= sprintf("line %05d %s\n", $i, str_repeat('x', 40));
}
file_put_contents("$logDir/image-refresh-4.log", $big);
$r = pv(['path' => "$P/log", 'query' => 'kind=image-refresh&id=4', 'env' => $LOGENV]);
$tail = (string) ($r['body']['data']['log'] ?? '');
pv_ok($r['status'] === 200 && ($r['body']['data']['truncated'] ?? null) === true
    && ($r['body']['data']['size'] ?? 0) === strlen($big) && strlen($tail) <= 65536
    && str_starts_with($tail, 'line ') && str_ends_with($big, $tail),
    'log tail over 64 KiB: truncated, bounded, starts on a whole line', 'len=' . strlen($tail));
$secret = "$logDir/not-a-log.txt";
file_put_contents($secret, "SECRET-CONTENT\n");
symlink($secret, "$logDir/stack-apply-5.log");
$r = pv(['path' => "$P/log", 'query' => 'kind=stack-apply&id=5', 'env' => $LOGENV]);
pv_ok($r['status'] === 200 && ($r['body']['data']['exists'] ?? null) === false
    && ($r['body']['data']['log'] ?? null) === '' && !str_contains(json_encode($r['body']), 'SECRET'),
    'log at a symlinked name is not followed (exists:false, nothing read)', json_encode($r['body']['data'] ?? null));
foreach (glob("$logDir/*") ?: [] as $f) {
    @unlink($f);
}
@rmdir($logDir);

// ---------------------------------------------------------------------------
echo "[6] enqueue contract (callQueued, exact verb + args, op never in args)\n";
$r = pv(['method' => 'POST', 'path' => "$P/image-refresh", 'body' => ['image' => 4], 'idem' => 'abcdef12-key', 'db' => [
    ['match' => $IMG_Q, 'rows' => [$IMG_UNFETCHED]],
]]);
$c = $r['ctl'][0] ?? [];
pv_ok($r['status'] === 200 && count($r['ctl']) === 1 && ($c['verb'] ?? '') === 'IMAGE_REFRESH'
    && ($c['args'] ?? null) === ['image' => '4'] && ($c['op'] ?? '') === 'tester' && ($c['idem'] ?? '') === 'abcdef12-key'
    && ($r['body']['data']['status'] ?? '') === 'queued' && ($r['body']['data']['request'] ?? '') === '42',
    'IMAGE_REFRESH image=4, op bound by callQueued, idem forwarded', json_encode($c));

$stageDb = [$ENT(), $MACS, ['match' => $IMG_Q, 'rows' => [$IMG]], $STACKS_LIVE(['core', 'web'])];
// Operator-picked MAC in vendor spelling: normalised, matched against the
// SoR's (case-insensitively stored) live MACs, sent lowercase-colon.
$r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => '4', 'stacks' => ['core', 'web', 'core'],
    'mac' => ' AA-BB-CC-00-00-02 '], 'db' => $stageDb]);
$c = $r['ctl'][0] ?? [];
pv_ok($r['status'] === 200 && ($c['verb'] ?? '') === 'PROV_STAGE'
    && ($c['args'] ?? null) === ['entity' => '9', 'image' => '4', 'stacks' => 'core,web', 'mac' => 'aa:bb:cc:00:00:02',
        'arch' => 'x86_64']
    && array_key_exists('idem', $c) && $c['idem'] === null
    && ($r['body']['data']['mac'] ?? '') === 'aa:bb:cc:00:00:02' && ($r['body']['data']['arch_source'] ?? '') === 'hardware_model',
    'PROV_STAGE entity=9 image=4 stacks=core,web mac=aa:bb:cc:00:00:02 arch=x86_64 (MAC normalised, dupes collapsed)',
    json_encode($c));
$stageQ = implode("\n", $r['sql']);
pv_ok(str_contains($stageQ, 'e.deleted_at IS NULL') && str_contains($stageQ, 'hm.arch AS hw_arch'),
    'stage resolves arch from the live entity\'s hardware model', $stageQ);
// No model arch: the agent's report is used (server-side, same as targets).
$r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => $MAC1],
    'env' => ['PV_NODES' => json_encode([['hostFqdn' => 'neon.akoria.net', 'arch' => 'amd64']])],
    'db' => [$ENT(['hw_arch' => null]), $MACS, ['match' => $IMG_Q, 'rows' => [$IMG]], $STACKS_LIVE(['core'])]]);
$c = $r['ctl'][0] ?? [];
pv_ok($r['status'] === 200 && ($c['args']['arch'] ?? '') === 'x86_64' && ($c['args']['mac'] ?? '') === $MAC1
    && ($r['body']['data']['arch_source'] ?? '') === 'agent',
    'stage: no hardware-model arch -> agent arch (amd64 -> x86_64)', "status={$r['status']} " . json_encode($c));

$archCases = [
    'model arch beats a matching agent: arm64 model vs x86_64 image -> 409' => [
        ['hw_arch' => 'arm64'], [['hostFqdn' => 'neon.akoria.net', 'arch' => 'x86_64']],
        'Image is x86_64 but target is arm64 (hardware model).'],
    'agent arch mismatch -> 409' => [
        ['hw_arch' => null], [['hostFqdn' => 'neon.akoria.net', 'arch' => 'aarch64']],
        'Image is x86_64 but target is arm64 (agent-reported).'],
    'unknown arch, model without arch -> 409 naming the model' => [
        ['hw_arch' => null], null,
        'Target CPU architecture is unknown: set the CPU architecture on hardware model Raspberry Pi Pi 5 (Inventory) or let the agent report it.'],
    'unknown arch, no hardware model -> 409' => [
        ['hw_arch' => null, 'hw_model_id' => null, 'hw_make' => null, 'hw_model' => null], [],
        'Target CPU architecture is unknown: assign neon a hardware model with a CPU architecture (Inventory) or let the agent report it.'],
];
foreach ($archCases as $label => [$over, $nodesFx, $msg]) {
    $r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => $MAC1],
        'env' => $nodesFx === null ? [] : ['PV_NODES' => json_encode($nodesFx)],
        'db' => [$ENT($over), $MACS, ['match' => $IMG_Q, 'rows' => [$IMG]], $STACKS_LIVE(['core'])]]);
    pv_ok($r['status'] === 409 && pv_msg($r) === $msg && $r['ctl'] === [], "stage $label", "status={$r['status']} msg=" . pv_msg($r));
}
$r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => 4, 'stacks' => ['core'],
    'mac' => 'aa:bb:cc:00:00:99'], 'db' => [$ENT(), $MACS, ['match' => $IMG_Q, 'rows' => [$IMG]], $STACKS_LIVE(['core'])]]);
pv_ok($r['status'] === 409 && pv_msg($r) === 'MAC aa:bb:cc:00:00:99 is not a live interface of neon.' && $r['ctl'] === [],
    'stage MAC not on a live interface of the entity -> 409', "status={$r['status']} msg=" . pv_msg($r));

$r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => $MAC1], 'db' => [
    $ENT(), $MACS, ['match' => $IMG_Q, 'rows' => [$IMG_UNFETCHED]],
]]);
pv_ok($r['status'] === 409 && pv_msg($r) === 'image not fetched — refresh it first' && $r['ctl'] === [],
    'stage with unfetched image -> 409 "image not fetched — refresh it first"', "status={$r['status']} msg=" . pv_msg($r));
$r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => $MAC1], 'db' => [
    $ENT(), $MACS, ['match' => $IMG_Q, 'rows' => [['sha256' => $SHA, 'fetched_at' => null] + $IMG]],
]]);
pv_ok($r['status'] === 409 && $r['ctl'] === [], 'stage with sha256 but no fetched_at -> 409', "status={$r['status']}");
$r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => $MAC1], 'db' => [
    $ENT(), $MACS, ['match' => $IMG_Q, 'rows' => [['boot_method' => 'media'] + $IMG]],
]]);
pv_ok($r['status'] === 409 && str_contains(pv_msg($r), 'ipxe') && $r['ctl'] === [], 'stage non-ipxe image -> 409', "status={$r['status']}");
$r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => $MAC1], 'db' => [
    $ENT(), ['match' => 'FROM interfaces WHERE entity_id', 'rows' => []],
]]);
pv_ok($r['status'] === 409 && str_contains(pv_msg($r), 'MAC') && $r['ctl'] === [], 'stage entity without a live MAC -> 409', "status={$r['status']}");
$r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => 4, 'stacks' => ['core'], 'mac' => $MAC1], 'db' => [
    ['match' => $ENT_Q, 'rows' => []],
]]);
pv_ok($r['status'] === 404 && $r['ctl'] === [], 'stage unknown/deleted entity -> 404', "status={$r['status']}");
$r = pv(['method' => 'POST', 'path' => "$P/stage", 'body' => ['entity' => 9, 'image' => 4, 'stacks' => ['core', 'gone'], 'mac' => $MAC1], 'db' => [
    $ENT(), $MACS, ['match' => $IMG_Q, 'rows' => [$IMG]], $STACKS_LIVE(['core']),
]]);
pv_ok($r['status'] === 404 && str_contains(pv_msg($r), 'gone') && $r['ctl'] === [], 'stage unknown stack -> 404 naming it', "status={$r['status']}");
$r = pv(['method' => 'POST', 'path' => "$P/image-refresh", 'body' => ['image' => 4], 'db' => [['match' => $IMG_Q, 'rows' => []]]]);
pv_ok($r['status'] === 404 && $r['ctl'] === [], 'image-refresh unknown image -> 404', "status={$r['status']}");

foreach ([true => '1', false => '0'] as $dry => $wire) {
    $r = pv(['method' => 'POST', 'path' => "$P/apply", 'body' => ['entity' => '9', 'stacks' => ['web'], 'dry_run' => (bool) $dry],
        'db' => [$ENTITY_LIVE, $STACKS_LIVE(['web'])]]);
    $c = $r['ctl'][0] ?? [];
    pv_ok($r['status'] === 200 && ($c['verb'] ?? '') === 'STACK_APPLY'
        && ($c['args'] ?? null) === ['entity' => '9', 'stacks' => 'web', 'dryRun' => $wire]
        && ($r['body']['data']['dry_run'] ?? null) === (bool) $dry,
        "STACK_APPLY entity=9 stacks=web dryRun=$wire", json_encode($c));
}
$opInArgs = false;
foreach ($ALL_CTL as $call) {
    if (!is_array($call['args'] ?? null) || array_key_exists('op', $call['args'])) {
        $opInArgs = true;
    }
}
pv_ok(count($ALL_CTL) === 5 && !$opInArgs && !$ANY_DIRECT,
    'across the whole run: 5 enqueues, none with op in args, no direct SolariCtl::call/send', 'ctl=' . count($ALL_CTL));

// Static: provisioning.php never calls SolariCtl::call/send directly.
$src = (string) file_get_contents(dirname(__DIR__, 2) . '/dashboard/api/routes/provisioning.php');
pv_ok(preg_match('/SolariCtl::(?:call|send)\s*\(/', $src) === 0, 'routes/provisioning.php has no direct SolariCtl::call/send');
pv_ok(preg_match_all("/SolariCtl::callQueued\(\s*'[A-Z_]+',\s*\\\$args,\s*\\\$op,/", $src) === 3,
    'each enqueue passes the session principal as callQueued $operator');

echo "\n$pass passed, $fail failed\n";
exit($fail === 0 ? 0 : 1);
