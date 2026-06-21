<?php
declare(strict_types=1);

/**
 * bootstrap.php — common setup for every dashboard API request.
 *
 * Loads the shared helpers, installs a uniform exception handler that converts
 * any uncaught error into the §11.2 error envelope, and returns a fresh Router.
 * Both the read layer (this agent) and the follow-up mutation/SSE layer include
 * this file, register their routes, and call $router->dispatch(...).
 */

require_once __DIR__ . '/Response.php';
require_once __DIR__ . '/Db.php';
require_once __DIR__ . '/Router.php';

// Never leak PHP notices/HTML into a JSON body; surface them as clean errors.
error_reporting(E_ALL);
ini_set('display_errors', '0');
ini_set('log_errors', '1');

set_exception_handler(static function (Throwable $e): void {
    error_log('[solari-api] ' . $e->getMessage() . ' @ ' . $e->getFile() . ':' . $e->getLine());
    // Do not echo internal detail to the client; keep it generic + stable.
    Response::error('internal_error', 'An internal error occurred.', 500);
});

set_error_handler(static function (int $severity, string $message, string $file, int $line): bool {
    // Promote real errors to exceptions; let the @-operator suppression pass.
    if ((error_reporting() & $severity) === 0) {
        return false;
    }
    throw new ErrorException($message, 0, $severity, $file, $line);
});

/**
 * Build and return the shared Router. Callers register routes then dispatch.
 */
function solari_router(): Router
{
    return new Router();
}

/**
 * Read and JSON-decode the request body (helper for the follow-up POST layer).
 *
 * @return array<string,mixed>
 */
function solari_json_body(): array
{
    $raw = file_get_contents('php://input');
    if ($raw === false || $raw === '') {
        return [];
    }
    $decoded = json_decode($raw, true);
    if (!is_array($decoded)) {
        Response::error('bad_request', 'Request body must be a JSON object.', 400);
    }
    return $decoded;
}

/** Current request method (defaults to GET for CLI/testing). */
function solari_method(): string
{
    return $_SERVER['REQUEST_METHOD'] ?? 'GET';
}

/** Current request path/URI (defaults to "/" for CLI/testing). */
function solari_uri(): string
{
    return $_SERVER['REQUEST_URI'] ?? '/';
}
