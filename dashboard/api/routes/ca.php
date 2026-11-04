<?php
declare(strict_types=1);

/**
 * Certificates (CA / PKI) read endpoint.
 *
 *   GET /api/ca  → { stepCa, roots, provisioners, nodeCerts, warnDays }
 *
 * Backed by lib/Ca.php: step-ca health/roots on chlorine over peer- and
 * hostname-verified HTTPS, the repo-local SolariNet mTLS CA (run/pki/ca.pem), the
 * static provisioner set, and the per-node cert inventory from the solarinet DB.
 * Pure read; fail-soft — an unreachable CA degrades that section, never a 500.
 */

require_once __DIR__ . '/../lib/Ca.php';
require_once __DIR__ . '/../lib/ApiCache.php';

return static function (Router $router): void {
    $router->get('/api/ca', static function (): void {
        try {
            $summary = ApiCache::remember('ca-summary-v1', 30, [Ca::class, 'summary']);
        } catch (Throwable $e) {
            // Keep certificate data failure local to this panel. In particular,
            // do not let a malformed upstream response fail the dashboard boot.
            error_log('[solari-api] CA summary failed: ' . get_class($e));
            $summary = [
                'stepCa' => [
                    'url' => Ca::stepCaUrl(), 'available' => false,
                    'status' => null, 'reason' => 'Certificate data temporarily unavailable',
                ],
                'roots' => [], 'provisioners' => [], 'nodeCerts' => [],
                'crl' => ['available' => false, 'generations' => []],
                'warnDays' => 60,
            ];
        }
        Response::ok($summary);
    });
};
