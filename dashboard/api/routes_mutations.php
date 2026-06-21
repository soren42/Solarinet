<?php
declare(strict_types=1);

/**
 * routes_mutations.php — registers every MUTATING (POST) route and the SSE
 * bridge on the shared Router, mirroring the read-layer routes.php shape.
 *
 * All lifecycle/provisioning mutations route through solariCtl over its AF_UNIX
 * socket (lib/SolariCtl.php); PHP holds no cert/CA material and writes no
 * monitoring telemetry tables (§11.1). The only direct DB write is to the
 * dashboard's own control-plane config table alertRule (handoff §6:
 * "POST /api/rules/{ruleId} → alertRule"). Destructive endpoints
 * (enrollment approve, decommission) require an operator role + explicit confirm
 * (lib/Operator.php) before contacting the bridge.
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
        __DIR__ . '/routes/stream.php',            // GET /api/stream (SSE bridge)
    ];
    foreach ($groups as $file) {
        $register = require $file;
        $register($router);
    }
};
