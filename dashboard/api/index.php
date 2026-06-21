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

$router = solari_router();

// Register the read (GET) routes.
$register = require __DIR__ . '/routes.php';
$register($router);

// Register the mutating (POST) routes + the SSE bridge. These route lifecycle
// mutations through solariCtl over its AF_UNIX socket (PHP holds no CA material);
// see routes_mutations.php and lib/SolariCtl.php.
$registerMut = require __DIR__ . '/routes_mutations.php';
$registerMut($router);

$router->dispatch(solari_method(), solari_uri());
