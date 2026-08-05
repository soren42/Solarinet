<?php
declare(strict_types=1);

/**
 * Identity (Keycloak users) read endpoint.
 *
 *   GET /api/identity  → { available, reason, realmUrl, users:[...] }
 *
 * Backed by lib/Identity.php: the `akoria` realm's users via the Keycloak
 * admin API (client_credentials service account), enriched per-user with
 * enrolled credential factors (password/otp/webauthn/webauthn-passwordless)
 * and group memberships. Pure read, read-only by contract — fail-soft; an
 * unreachable/unconfigured Keycloak degrades that section, never a 500.
 */

require_once __DIR__ . '/../lib/Identity.php';

return static function (Router $router): void {
    $router->get('/api/identity', static function (): void {
        Response::ok(Identity::users());
    });
};
