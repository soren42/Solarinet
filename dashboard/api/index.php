<?php
declare(strict_types=1);

/**
 * index.php — front controller for the SolariNet dashboard API.
 *
 * Apache/nginx should rewrite all /api/* requests to this file. It boots the
 * shared foundation, registers the read routes, and dispatches.
 *
 * Example Apache rewrite (place at the dashboard/api/ docroot):
 *   RewriteEngine On
 *   RewriteCond %{REQUEST_FILENAME} !-f
 *   RewriteRule ^ index.php [QSA,L]
 *
 * To add the mutating POST endpoints and SSE bridge (§6 / §11.2), the follow-up
 * agent registers them on the same $router before dispatch (see routes.php).
 */

require_once __DIR__ . '/lib/bootstrap.php';
require_once __DIR__ . '/lib/Auth.php';
require_once __DIR__ . '/lib/Oidc.php';
require_once __DIR__ . '/lib/Operator.php';
require_once __DIR__ . '/lib/Policy.php';

$router = solari_router();

// Single authorization + CSRF chokepoint. DEFAULT-DENY: every state-changing
// method requires an operator/admin role and a same-origin CSRF token unless the
// route explicitly declares a weaker policy (e.g. ['auth'=>'public'] on login).
// This replaces the previous per-handler requireOperator() pattern, under which
// the audit found ~13 write routes silently ungated (findings F-02/F-03/F-05).
$router->setGuard([Policy::class, 'enforce']);

// Auth routes are the ONLY endpoints reachable without a session.
$registerAuth = require __DIR__ . '/routes/auth.php';
$registerAuth($router);

// Register the read (GET) routes.
$register = require __DIR__ . '/routes.php';
$register($router);

// Register the mutating (POST) routes + the SSE bridge. These route lifecycle
// mutations through solariCtl over its AF_UNIX socket (PHP holds no CA material);
// see routes_mutations.php and lib/SolariCtl.php.
$registerMut = require __DIR__ . '/routes_mutations.php';
$registerMut($router);

// ---- authentication gate -------------------------------------------------
// Everything except /api/auth/* requires an established session. Single-admin
// local mode for now (intranet only); see lib/Auth.php. This is the hard gate
// the audit flagged as missing: no endpoint (read or mutate) is reachable
// unauthenticated.
$reqPath = parse_url(solari_uri(), PHP_URL_PATH);
$reqPath = is_string($reqPath) ? $reqPath : '/';
if (strpos($reqPath, '/api/auth/') === false) {
    Auth::requireSession();   // emits 401 + exits when no valid session
}

$router->dispatch(solari_method(), solari_uri());
