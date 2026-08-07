<?php
declare(strict_types=1);

require_once __DIR__ . '/Auth.php';

/**
 * Operator — resolves the authenticated operator identity + role for the
 * dashboard mutation layer (§6, §11.1).
 *
 * The PHP tier is responsible for authenticating the human; the C bridge then
 * enforces that destructive verbs name an authorizing operator (RBAC) and that
 * an explicit confirm accompanies irreversible ops. This helper centralises how
 * the web tier learns *who* the operator is and *what role* they hold, so the
 * mutation routes stay declarative.
 *
 * Identity + role come from the authenticated session (lib/Auth.php) — the
 * principal established at /api/auth/login. The previous design trusted
 * $_SERVER['REMOTE_USER']/['SOLARI_ROLE'], which are only meaningful behind a
 * configured auth proxy and were otherwise spoofable/absent; the session is now
 * the single source of truth, so identity cannot be forged by request headers.
 *
 * Destructive endpoints (enrollment approve, decommission) call requireOperator()
 * which fails closed (403) when the resolved role is not 'operator'/'admin'. The
 * front controller's auth gate already guarantees a session exists at all.
 */
final class Operator
{
    private const PRIVILEGED_ROLES = ['operator', 'admin'];

    /** Resolve the operator login name, or '' if not authenticated. */
    public static function name(): string
    {
        $p = Auth::current();
        return $p === null ? '' : (string) $p['username'];
    }

    /** Resolve the operator role (lower-cased), or 'viewer' if not authenticated. */
    public static function role(): string
    {
        $p = Auth::current();
        return $p === null ? 'viewer' : strtolower((string) $p['role']);
    }

    /** True if the current identity holds a privileged (write) role. */
    public static function isPrivileged(): bool
    {
        return in_array(self::role(), self::PRIVILEGED_ROLES, true);
    }

    /**
     * Gate a destructive endpoint: require a named operator AND a privileged
     * role. Emits a 403 envelope and terminates if either is missing. Returns
     * the operator name on success (for forwarding as op= to the bridge).
     */
    public static function requireOperator(): string
    {
        $name = self::name();
        if ($name === '' || !self::isPrivileged()) {
            Response::error('forbidden',
                'This action requires an authenticated operator role.', 403);
        }
        return $name;
    }

    /** Gate an admin-only endpoint and return the admin login for auditing. */
    public static function requireAdmin(): string
    {
        $name = self::name();
        if ($name === '' || self::role() !== 'admin') {
            Response::error('forbidden',
                'This action requires an authenticated admin role.', 403);
        }
        return $name;
    }
}
