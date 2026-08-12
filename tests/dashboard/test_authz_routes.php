<?php
declare(strict_types=1);

/**
 * test_authz_routes.php — load EVERY dashboard route through the real
 * registration code and assert its effective auth level (findings F-02/03/05).
 *
 * The unit test (test_authz_policy.php) proves the resolver in isolation; this
 * proves it against the ACTUAL route table. It boots a real Router, runs the
 * same routes/*.php closures index.php uses, then introspects every registered
 * route and computes Policy::resolveAuth(). The hard invariant:
 *
 *   NO state-changing route (POST/PUT/PATCH/DELETE) may resolve below
 *   operator/admin, except the two intentional auth exceptions:
 *     POST /api/auth/login  -> public   (pre-session credential exchange)
 *     POST /api/auth/logout -> session  (authenticated, CSRF-protected)
 *
 * This is the guard that catches a NEW ungated write route the moment it is
 * added — the exact regression (~13 silently-ungated writes) that motivated the
 * default-deny layer. Runs standalone under the CLI SAPI (registration stores
 * closures; no handler body — hence no DB — executes).
 */

$API = dirname(__DIR__, 2) . '/dashboard/api';

// Minimal request env so bootstrap's handlers install cleanly under CLI.
$_SERVER['REQUEST_METHOD'] = 'GET';
$_SERVER['REQUEST_URI']    = '/';

require_once $API . '/lib/bootstrap.php';
require_once $API . '/lib/Auth.php';
require_once $API . '/lib/Oidc.php';
require_once $API . '/lib/Operator.php';
require_once $API . '/lib/Policy.php';

function assert_true(bool $cond, string $message): void
{
    if (!$cond) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

$router = solari_router();

// Same load order as index.php: auth, reads, mutations.
(require $API . '/routes/auth.php')($router);
(require $API . '/routes.php')($router);
(require $API . '/routes_mutations.php')($router);

// Introspect the private routes table.
$prop   = (new ReflectionClass(Router::class))->getProperty('routes');
$routes = $prop->getValue($router);
assert_true(count($routes) > 0, 'route table must not be empty');

// The ONLY writes allowed to resolve below operator/admin, by explicit design.
$writeExceptions = [
    'POST /api/auth/login'  => 'public',
    'POST /api/auth/logout' => 'session',
];

$readable = static function (string $regex): string {
    $p = $regex;
    if (strncmp($p, '#^', 2) === 0)       { $p = substr($p, 2); }
    if (substr($p, -4) === '/?$#')        { $p = substr($p, 0, -4); }
    return $p;
};

$counts = ['public' => 0, 'session' => 0, 'operator' => 0, 'admin' => 0];
$violations = [];

foreach ($routes as $r) {
    $method = $r['method'];
    $path   = $readable($r['regex']);
    $eff    = Policy::resolveAuth($method, $r['policy']);
    if (isset($counts[$eff])) { $counts[$eff]++; }

    // Sanity: resolver must only ever emit the four known levels.
    assert_true(in_array($eff, ['public', 'session', 'operator', 'admin'], true),
        "route {$method} {$path} resolved to unknown level '{$eff}'");

    if (Policy::isWrite($method) && !in_array($eff, ['operator', 'admin'], true)) {
        $key = $method . ' ' . $path;
        if (($writeExceptions[$key] ?? null) !== $eff) {
            $violations[] = "{$key} resolves to '{$eff}' (expected operator/admin, or a declared exception)";
        }
    }
}

if ($violations) {
    fwrite(STDERR, "FAIL: unauthorized write route(s) below operator/admin:\n  "
        . implode("\n  ", $violations) . "\n");
    exit(1);
}

// Belt-and-braces: every declared exception must actually still be present, so
// this test fails loudly if a route is renamed/removed and the exception rots.
foreach ($writeExceptions as $key => $lvl) {
    [$m, $path] = explode(' ', $key, 2);
    $found = false;
    foreach ($routes as $r) {
        if ($r['method'] === $m && $readable($r['regex']) === $path
            && Policy::resolveAuth($m, $r['policy']) === $lvl) {
            $found = true;
            break;
        }
    }
    assert_true($found, "expected auth exception missing/changed: {$key} => {$lvl}");
}

printf("authz route audit passed — %d routes (public=%d session=%d operator=%d admin=%d); "
    . "all writes operator/admin except login(public)+logout(session)\n",
    count($routes), $counts['public'], $counts['session'], $counts['operator'], $counts['admin']);
