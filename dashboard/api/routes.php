<?php
declare(strict_types=1);

/**
 * routes.php — registers every READ (GET) route on the shared Router.
 *
 * Each routes/*.php file returns a closure fn(Router): void that registers its
 * own routes. This file simply loads and invokes them in turn.
 *
 * EXTENSION POINT: the follow-up mutation/SSE agent should create its own
 * routes group files (e.g. routes/control.php, routes/stream.php) returning the
 * same fn(Router): void shape, and append them to the $groups list below. POST
 * handlers validate the JSON body (solari_json_body()), enforce RBAC/CSRF, and
 * route mutations through solariCtl — they never write monitoring tables.
 *
 * @param Router $router
 */

require_once __DIR__ . '/lib/Coerce.php';

return static function (Router $router): void {
    $groups = [
        __DIR__ . '/routes/summary.php',
        __DIR__ . '/routes/panel.php',
        __DIR__ . '/routes/nodes.php',
        __DIR__ . '/routes/probes.php',
        __DIR__ . '/routes/alerts.php',
        __DIR__ . '/routes/maintenance.php',
        __DIR__ . '/routes/opie.php',
        __DIR__ . '/routes/topology.php',
        __DIR__ . '/routes/gear.php',
        __DIR__ . '/routes/discovery.php',
        __DIR__ . '/routes/forgejo.php',
        __DIR__ . '/routes/ca.php',
        __DIR__ . '/routes/identity.php',
        __DIR__ . '/routes/push.php',
        __DIR__ . '/routes/provisioning.php',
        __DIR__ . '/routes/inventory.php',
        // inv_codes.php MUST load immediately after inventory.php: it reuses
        // inventory.php's `Inv` class + inv_* helpers and must never re-require
        // it (that would fatally redeclare `Inv`). See routes/inv_codes.php.
        __DIR__ . '/routes/inv_codes.php',
        __DIR__ . '/routes/dns.php',
        // rackwire.php is SoR-backed CRUD (Sor::db(), like inventory.php /
        // inv_codes.php above), not a solariCtl bridge — it registers here,
        // not in routes_mutations.php, even though it contains POST/mutation
        // routes. routes_mutations.php's doc comment describes the
        // solariCtl-only convention; inventory.php already establishes SoR
        // writes as a routes.php-loaded exception to that convention, and
        // rackwire.php follows the same precedent for the same reason (a
        // single file owns its GET+POST routes and its private helper
        // class/functions without a cross-file load-order dependency).
        __DIR__ . '/routes/rackwire.php',
    ];
    foreach ($groups as $file) {
        $register = require $file;
        $register($router);
    }
};
