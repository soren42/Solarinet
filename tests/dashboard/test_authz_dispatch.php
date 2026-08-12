<?php
declare(strict_types=1);

/**
 * test_authz_dispatch.php — prove the guard is actually WIRED INTO the request
 * path (findings F-02/03/05; closes a cross-lab-review gap).
 *
 * The other authz tests exercise Policy in isolation and enforce() directly.
 * None of them dispatch through the real Router, so a regression that removed
 * setGuard() or the guard invocation in dispatch() would leave them all green.
 * This test drives Router::dispatch() end to end with a spy guard and a sentinel
 * handler, asserting:
 *   - the guard runs BEFORE the handler on a matched route;
 *   - a denying guard prevents the handler from ever running (default-deny);
 *   - the guard receives the ACTUAL request method — including for a wildcard
 *     ('*') route, the regression class where a write would otherwise be
 *     classified as a read and skip write-authz + CSRF (Router.php guard call).
 *
 * Uses a spy guard rather than Policy::enforce() so we test the WIRING here and
 * the enforcement SEMANTICS in test_authz_enforce.php — separately, on purpose.
 */

$API = dirname(__DIR__, 2) . '/dashboard/api';
require_once $API . '/lib/Router.php';

function assert_true(bool $cond, string $message): void
{
    if (!$cond) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

/** A guard that throws instead of exiting, so the test can observe a denial. */
class GuardDenied extends RuntimeException {}

// ---- 1. guard runs, in order, and receives the real method/path/policy -------

$events = [];                       // ordered log of what happened
$router = new Router();
$router->setGuard(static function (string $method, string $path, array $policy) use (&$events): void {
    $events[] = ['guard', $method, $path, $policy];   // allow (return) — records only
});
$router->post('/api/thing/{id}', static function (array $p) use (&$events): void {
    $events[] = ['handler', $p['id']];
}, ['auth' => 'operator']);

$router->dispatch('POST', '/api/thing/42');

assert_true(count($events) === 2, 'guard + handler should both run on an allowed request');
assert_true($events[0][0] === 'guard' && $events[1][0] === 'handler',
    'guard must run BEFORE the handler');
assert_true($events[0][1] === 'POST', 'guard receives the request method');
assert_true($events[0][2] === '/api/thing/42', 'guard receives the matched path');
assert_true($events[0][3] === ['auth' => 'operator'], 'guard receives the route policy');
assert_true($events[1][1] === '42', 'handler still receives its decoded path params');

// ---- 2. a denying guard blocks the handler entirely (default-deny) -----------

$reached = false;
$router2 = new Router();
$router2->setGuard(static function (string $m, string $p, array $pol): void {
    throw new GuardDenied('denied');            // stand-in for Response::error()+exit
});
$router2->post('/api/thing/{id}', static function (array $p) use (&$reached): void {
    $reached = true;                            // must NOT run
}, ['auth' => 'operator']);

$denied = false;
try {
    $router2->dispatch('POST', '/api/thing/7');
} catch (GuardDenied $e) {
    $denied = true;
}
assert_true($denied, 'a denying guard must propagate (handler unreachable)');
assert_true($reached === false, 'handler MUST NOT run when the guard denies');

// ---- 3. wildcard route: guard still sees the ACTUAL write method --------------
// The regression fix: a '*' route matches every verb; the guard must be told the
// browser's method (POST here), not the route's registered '*', or writes to a
// wildcard route would be treated as reads and skip write-authz + CSRF.

$seenMethod = null;
$router3 = new Router();
$router3->setGuard(static function (string $method, string $p, array $pol) use (&$seenMethod): void {
    $seenMethod = $method;
});
$router3->add('*', '/api/wild', static function (array $p): void {}, []);
$router3->dispatch('POST', '/api/wild');
assert_true($seenMethod === 'POST',
    "wildcard route must pass the real request method to the guard, got '"
    . var_export($seenMethod, true) . "'");

echo "authz dispatch wiring tests passed — guard runs before handler, blocks denials, sees real method (incl. wildcard)\n";
