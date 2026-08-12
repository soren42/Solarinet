<?php
declare(strict_types=1);

/**
 * test_authz_enforce.php — exercise the LIVE Policy::enforce() path end to end
 * with real superglobals and a real session (findings F-02/03/05).
 *
 * The unit + route tests prove the decision logic and the route table; this
 * proves the enforcement side effects: a forged/cross-origin/token-less write is
 * actually rejected, a read-only role cannot write even WITH a valid CSRF token,
 * and a fully-authorized operator write is let through.
 *
 * Response::error() emits its envelope and calls exit(), so a scenario cannot be
 * unwound in-process. This file is therefore DUAL-MODE:
 *   - child  (env SOLARI_ENFORCE_SCENARIO set): set up superglobals, run enforce()
 *            once, print ENFORCE_PASS iff it returns.
 *   - driver (no env): spawn itself once per scenario via the same PHP binary and
 *            assert the observed outcome. Pure PHP — no shell harness.
 */

$API = dirname(__DIR__, 2) . '/dashboard/api';
$scenario = getenv('SOLARI_ENFORCE_SCENARIO');

// ------------------------------------------------------------------ CHILD MODE
if (is_string($scenario) && $scenario !== '') {
    require_once $API . '/lib/bootstrap.php';
    require_once $API . '/lib/Auth.php';
    require_once $API . '/lib/Operator.php';
    require_once $API . '/lib/Policy.php';

    // Same-origin baseline (no TLS under CLI → http); cross-origin case overrides.
    $_SERVER['HTTP_HOST']   = 'dash.test';
    $_SERVER['HTTP_ORIGIN'] = 'http://dash.test';

    // Start the session first, THEN inject the principal, so enforce()'s own
    // bootSession() is a no-op and this $_SESSION survives.
    Auth::bootSession();
    $tok = bin2hex(str_repeat("\x11", 32));
    $mk  = static fn(string $role): array => [
        'username' => 'tester', 'role' => $role,
        'displayName' => 'Tester', 'source' => 'local',
    ];

    $method = 'POST';
    $policy = [];   // undeclared write → default operator + CSRF

    switch ($scenario) {
        case 'write-nosession':
            break;                                              // no principal
        case 'write-crossorigin':
            $_SESSION['solari'] = $mk('operator');
            $_SESSION['solari_csrf'] = $tok;
            $_SERVER['HTTP_ORIGIN'] = 'https://evil.example.com';
            $_SERVER['HTTP_X_SOLARI_CSRF'] = $tok;
            break;
        case 'write-notoken':
            $_SESSION['solari'] = $mk('operator');
            $_SESSION['solari_csrf'] = $tok;                    // no request header
            break;
        case 'write-badtoken':
            $_SESSION['solari'] = $mk('operator');
            $_SESSION['solari_csrf'] = $tok;
            $_SERVER['HTTP_X_SOLARI_CSRF'] = 'not-the-token';
            break;
        case 'write-viewer':                                    // the F-02 core case
            $_SESSION['solari'] = $mk('viewer');
            $_SESSION['solari_csrf'] = $tok;
            $_SERVER['HTTP_X_SOLARI_CSRF'] = $tok;
            break;
        case 'write-ok-operator':
            $_SESSION['solari'] = $mk('operator');
            $_SESSION['solari_csrf'] = $tok;
            $_SERVER['HTTP_X_SOLARI_CSRF'] = $tok;
            break;
        case 'read-session':
            $method = 'GET';
            $_SESSION['solari'] = $mk('viewer');                // reads need only a session
            break;
        case 'read-nosession':
            $method = 'GET';
            break;
        case 'public-route':
            $method = 'GET';
            $policy = ['auth' => 'public'];
            break;
        default:
            fwrite(STDERR, "unknown scenario: $scenario\n");
            exit(2);
    }

    Policy::enforce($method, '/api/test', $policy);
    echo "ENFORCE_PASS\n";   // only reached when enforce() authorizes
    exit(0);
}

// ----------------------------------------------------------------- DRIVER MODE
function assert_true(bool $cond, string $message): void
{
    if (!$cond) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

/** Run one scenario in a child process; return 'PASS' or the error envelope code. */
function runScenario(string $scenario): string
{
    $php = PHP_BINARY ?: 'php';
    $cmd = escapeshellarg($php) . ' ' . escapeshellarg(__FILE__);
    $env = ['SOLARI_ENFORCE_SCENARIO' => $scenario];
    // Inherit PATH so PHP starts cleanly; override only our scenario selector.
    $descriptors = [1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $proc = proc_open($cmd, $descriptors, $pipes, null, $env + $_ENV);
    assert_true(is_resource($proc), "failed to spawn child for scenario $scenario");
    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);

    if (strpos($out, 'ENFORCE_PASS') !== false) {
        return 'PASS';
    }
    $j = json_decode(trim($out), true);
    return (is_array($j) && isset($j['error']['code'])) ? (string) $j['error']['code'] : '?';
}

$cases = [
    ['write-nosession',   'unauthorized', 'write w/o session -> 401'],
    ['write-crossorigin', 'forbidden',    'session+token but cross-origin -> 403'],
    ['write-notoken',     'forbidden',    'same-origin, missing CSRF header -> 403'],
    ['write-badtoken',    'forbidden',    'same-origin, wrong CSRF token -> 403'],
    ['write-viewer',      'forbidden',    'F-02: viewer w/ valid CSRF still blocked -> 403'],
    ['write-ok-operator', 'PASS',         'operator+same-origin+valid token -> allow'],
    ['read-session',      'PASS',         'GET with a session -> allow'],
    ['read-nosession',    'unauthorized', 'GET without a session -> 401'],
    ['public-route',      'PASS',         'public route, no session -> allow'],
];

foreach ($cases as [$scen, $expect, $label]) {
    $got = runScenario($scen);
    assert_true($got === $expect,
        "enforce[$scen]: expected '$expect', got '$got' — $label");
}

printf("authz enforce paths passed — %d live scenarios (deny + allow, incl. F-02 viewer-blocked)\n",
    count($cases));
