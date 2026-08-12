<?php
declare(strict_types=1);

/**
 * test_authz_policy.php — unit coverage for the Phase 0 default-deny
 * authorization + CSRF layer (findings F-02 / F-03 / F-05).
 *
 * Exercises the PURE decision helpers in lib/Policy.php — the ones with no
 * superglobal dependence — so the default-deny guarantee, the origin allowlist,
 * and the constant-time token check are all directly asserted:
 *
 *   resolveAuth()   the write=>operator / read=>session default, and that an
 *                   explicit declaration wins.
 *   originAllowed() same-origin accepted, cross-origin and empty rejected.
 *   tokenOk()       matching token accepted; empty/mismatch rejected.
 *
 * Loading Policy.php pulls in Response/Auth/Operator; none of the exercised
 * methods touch a session, so the file runs standalone under the CLI SAPI.
 */

require_once __DIR__ . '/../../dashboard/api/lib/Policy.php';

function assert_true(bool $cond, string $message): void
{
    if (!$cond) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

// ---- resolveAuth: DEFAULT-DENY is the whole point of F-02 --------------------

// An undeclared write must resolve to 'operator' — this is the guarantee that
// the ~13 previously-ungated mutation routes are now covered by default.
assert_true(Policy::resolveAuth('POST', []) === 'operator',
    'undeclared POST must default to operator (default-deny for writes)');
assert_true(Policy::resolveAuth('put', []) === 'operator',
    'method match is case-insensitive: PUT still defaults to operator');
assert_true(Policy::resolveAuth('DELETE', []) === 'operator',
    'undeclared DELETE must default to operator');
assert_true(Policy::resolveAuth('PATCH', []) === 'operator',
    'undeclared PATCH must default to operator');

// An undeclared read requires a session but not operator — reads are gated,
// not open, but do not demand the write role.
assert_true(Policy::resolveAuth('GET', []) === 'session',
    'undeclared GET must default to session');
assert_true(Policy::resolveAuth('HEAD', []) === 'session',
    'undeclared HEAD must default to session');

// An explicit declaration always wins over the default.
assert_true(Policy::resolveAuth('POST', ['auth' => 'public']) === 'public',
    'explicit public on a write is honored (e.g. /api/auth/login)');
assert_true(Policy::resolveAuth('POST', ['auth' => 'session']) === 'session',
    'explicit session on a write is honored (e.g. /api/auth/logout)');
assert_true(Policy::resolveAuth('GET', ['auth' => 'admin']) === 'admin',
    'explicit admin on a read is honored');
// An empty/whitespace declaration is treated as absent (falls back to default).
assert_true(Policy::resolveAuth('POST', ['auth' => '']) === 'operator',
    'empty auth declaration falls back to the write default');

// ---- isWrite: the method classification the CSRF gate keys off ---------------

assert_true(Policy::isWrite('POST') && Policy::isWrite('put')
    && Policy::isWrite('PATCH') && Policy::isWrite('DELETE'),
    'POST/PUT/PATCH/DELETE are writes');
assert_true(!Policy::isWrite('GET') && !Policy::isWrite('HEAD')
    && !Policy::isWrite('OPTIONS'),
    'GET/HEAD/OPTIONS are not writes');

// ---- originAllowed: same-origin only, empty never matches --------------------

$allowed = ['https://dashboard.akoria.net', 'https://alias.akoria.net'];
assert_true(Policy::originAllowed('https://dashboard.akoria.net', $allowed),
    'same-origin request is allowed');
assert_true(Policy::originAllowed('https://alias.akoria.net', $allowed),
    'a configured extra origin is allowed');
assert_true(!Policy::originAllowed('https://evil.example.com', $allowed),
    'cross-origin request is rejected');
assert_true(!Policy::originAllowed('', $allowed),
    'missing Origin (empty) is rejected on a state-changing request');
assert_true(!Policy::originAllowed('http://dashboard.akoria.net', $allowed),
    'scheme mismatch (http vs https) is rejected — origin includes scheme');

// ---- tokenOk: constant-time compare that fails closed on empties -------------

$tok = bin2hex(str_repeat("\x2b", 32));   // deterministic 64-hex-char token
assert_true(Policy::tokenOk($tok, $tok),
    'matching token validates');
assert_true(!Policy::tokenOk('', $tok),
    'empty sent token never validates');
assert_true(!Policy::tokenOk($tok, ''),
    'empty stored token never validates (session predates CSRF / no principal)');
assert_true(!Policy::tokenOk('', ''),
    'two empty tokens never validate');
assert_true(!Policy::tokenOk($tok . 'x', $tok),
    'mismatched token is rejected');

echo "authz policy default-deny/CSRF tests passed\n";
