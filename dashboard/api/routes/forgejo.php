<?php
declare(strict_types=1);

/**
 * Git (Forgejo) read endpoint.
 *
 *   GET /api/forgejo  → { available, baseUrl, version, counts, repos, commits, prs }
 *
 * Backed by the Forgejo HTTP API (lib/Forgejo.php), configured from FORGEJO_URL /
 * FORGEJO_TOKEN. Pure read; fail-soft — a missing token or unreachable instance
 * returns { available:false, reason } with HTTP 200 so the screen can render its
 * own unavailable state rather than erroring.
 */

require_once __DIR__ . '/../lib/Forgejo.php';

return static function (Router $router): void {
    $router->get('/api/forgejo', static function (): void {
        Response::ok(Forgejo::summary());
    });
};
