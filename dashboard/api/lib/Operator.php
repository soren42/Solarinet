<?php
declare(strict_types=1);

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
 * Identity sources, in priority order:
 *   1. A front-end auth layer that populates $_SERVER['REMOTE_USER'] /
 *      'PHP_AUTH_USER' (Apache Basic/Negotiate/mod_auth_*).
 *   2. SOLARI_OPERATOR / SOLARI_OPERATOR_ROLE env (single-operator deployments
 *      behind a trusted reverse proxy).
 *
 * Role source:
 *   - $_SERVER['SOLARI_ROLE'] (set by the proxy from a group claim), else
 *   - SOLARI_OPERATOR_ROLE env, else 'viewer'.
 *
 * Destructive endpoints (enrollment approve, decommission) call requireOperator()
 * which fails closed (403) when the resolved role is not 'operator'/'admin'.
 */
final class Operator
{
    private const PRIVILEGED_ROLES = ['operator', 'admin'];

    /** Resolve the operator login name, or '' if none could be determined. */
    public static function name(): string
    {
        foreach (['REMOTE_USER', 'PHP_AUTH_USER'] as $k) {
            if (!empty($_SERVER[$k]) && is_string($_SERVER[$k])) {
                return $_SERVER[$k];
            }
        }
        $env = getenv('SOLARI_OPERATOR');
        return ($env === false) ? '' : $env;
    }

    /** Resolve the operator role (lower-cased), defaulting to 'viewer'. */
    public static function role(): string
    {
        if (!empty($_SERVER['SOLARI_ROLE']) && is_string($_SERVER['SOLARI_ROLE'])) {
            return strtolower($_SERVER['SOLARI_ROLE']);
        }
        $env = getenv('SOLARI_OPERATOR_ROLE');
        return ($env === false || $env === '') ? 'viewer' : strtolower($env);
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
}
