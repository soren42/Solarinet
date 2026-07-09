<?php
declare(strict_types=1);

/**
 * Certificates (CA / PKI) read endpoint.
 *
 *   GET /api/ca  → { stepCa, roots, provisioners, nodeCerts, warnDays }
 *
 * Backed by lib/Ca.php: step-ca health/roots on chlorine (TLS verify OFF —
 * internal private root), the repo-local SolariNet mTLS CA (run/pki/ca.pem), the
 * static provisioner set, and the per-node cert inventory from the solarinet DB.
 * Pure read; fail-soft — an unreachable CA degrades that section, never a 500.
 */

require_once __DIR__ . '/../lib/Ca.php';

return static function (Router $router): void {
    $router->get('/api/ca', static function (): void {
        Response::ok(Ca::summary());
    });
};
