<?php
declare(strict_types=1);

/**
 * routes_mutations.php — registers every MUTATING (POST) route and the SSE
 * bridge on the shared Router, mirroring the read-layer routes.php shape.
 *
 * Strict separation: PHP is a READ layer (SELECT / local config reads only).
 * EVERY mutation — lifecycle, config, rules, assets, pools — is marshalled to the
 * C server over the solariCtl AF_UNIX socket (lib/SolariCtl.php), which performs
 * the authoritative write. PHP holds no cert/CA material and writes no DB tables
 * (§11.1). Destructive endpoints (enrollment approve, decommission) require an
 * operator role + explicit confirm (lib/Operator.php) before contacting the bridge.
 *
 * @return callable(Router):void
 */

require_once __DIR__ . '/lib/Coerce.php';
require_once __DIR__ . '/lib/SolariCtl.php';
require_once __DIR__ . '/lib/Operator.php';

return static function (Router $router): void {
    $groups = [
        __DIR__ . '/routes/discovery_mut.php',     // adopt / ignore
        __DIR__ . '/routes/enrollments_mut.php',   // approve / reject
        __DIR__ . '/routes/control.php',           // provision / decommission / survey
        __DIR__ . '/routes/config.php',            // GET/POST config, POST rules/{id}
        __DIR__ . '/routes/pools.php',             // GET/POST pools (functional groups)
        __DIR__ . '/routes/assets.php',            // GET/POST monitored systems
        __DIR__ . '/routes/stream.php',            // GET /api/stream (SSE bridge)
    ];
    foreach ($groups as $file) {
        $register = require $file;
        $register($router);
    }
};
