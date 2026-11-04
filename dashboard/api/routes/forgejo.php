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
require_once __DIR__ . '/../lib/ApiCache.php';

return static function (Router $router): void {
    $router->get('/api/forgejo', static function (): void {
        try {
            $summary = ApiCache::remember('forgejo-summary-v1', 30, [Forgejo::class, 'summary']);
        } catch (Throwable $e) {
            // A read-only integration must degrade its own panel rather than
            // turning an unexpected upstream/decoder failure into an API 500.
            error_log('[solari-api] Forgejo summary failed: ' . get_class($e));
            $summary = [
                'available' => false,
                'reason' => 'Forgejo data temporarily unavailable',
                'baseUrl' => Forgejo::baseUrl(),
                'counts' => ['repos' => 0, 'openIssues' => 0, 'openPrs' => 0],
                'repos' => [], 'commits' => [], 'prs' => [],
            ];
        }
        Response::ok($summary);
    });
};
