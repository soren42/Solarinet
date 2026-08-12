<?php
declare(strict_types=1);

require_once __DIR__ . '/Response.php';
require_once __DIR__ . '/Auth.php';
require_once __DIR__ . '/Operator.php';

/**
 * Policy — the single server-side authorization + CSRF layer for the dashboard
 * API (stabilization Phase 0, findings F-02 / F-03 / F-05).
 *
 * WHY THIS EXISTS
 * ---------------
 * The previous design gated writes per-route: a handler *optionally* called
 * Operator::requireOperator(). The review found at least thirteen mutation
 * handlers that omitted it, so the "viewer" role was not actually read-only —
 * a logged-in viewer could scan, adopt, edit config, reject enrollments, etc.
 * Adding the check to each handler is exactly the pattern that let the gaps in.
 *
 * Instead this layer enforces authorization ONCE, at the Router dispatch
 * chokepoint, and is DEFAULT-DENY: any state-changing method requires an
 * operator/admin role unless the route explicitly declares a weaker policy.
 * Absence of a declaration is not permission — it is denial.
 *
 * ROUTE POLICY
 * ------------
 * A route may carry ['auth' => 'public'|'session'|'operator'|'admin']:
 *   public   — reachable with no session (login, oidc, auth/config). Never a write path.
 *   session  — any authenticated principal (reads; a few per-session writes e.g. logout).
 *   operator — operator or admin role required (the DEFAULT for every write).
 *   admin    — admin role required.
 * With no declaration: GET/HEAD/OPTIONS => 'session', everything else => 'operator'.
 *
 * CSRF / ORIGIN
 * -------------
 * Cookie-authenticated state-changing requests additionally require:
 *   - a same-origin Origin (or, absent that, a same-origin Referer); and
 *   - a session-bound synchronizer token echoed in X-Solari-CSRF.
 * SameSite=Lax remains as defense in depth, not as the authority mechanism.
 */
final class Policy
{
    /** Methods that change server state and therefore need write authz + CSRF. */
    private const WRITE_METHODS = ['POST', 'PUT', 'PATCH', 'DELETE'];

    /**
     * Resolve the effective auth level for a request. Pure (no superglobals),
     * so the default-deny guarantee is directly unit-testable.
     *
     * @param string               $method HTTP method (upper- or lower-case)
     * @param array<string,mixed>  $policy the route's declared policy (may be empty)
     * @return string one of public|session|operator|admin
     */
    public static function resolveAuth(string $method, array $policy): string
    {
        $declared = $policy['auth'] ?? null;
        if (is_string($declared) && $declared !== '') {
            return $declared;
        }
        // No declaration: default-deny for writes, session for reads.
        return self::isWrite($method) ? 'operator' : 'session';
    }

    /** True if the method changes state (needs write authorization + CSRF). */
    public static function isWrite(string $method): bool
    {
        return in_array(strtoupper($method), self::WRITE_METHODS, true);
    }

    /**
     * The guard installed on the Router: enforce the effective policy for a
     * matched route. Emits a 401/403/415 envelope and terminates on failure;
     * returns cleanly when the request is authorized.
     *
     * @param string              $method HTTP method of the matched route
     * @param string              $path   normalized request path (for messages)
     * @param array<string,mixed> $policy the route's declared policy
     */
    public static function enforce(string $method, string $path, array $policy): void
    {
        $auth = self::resolveAuth($method, $policy);

        // Public routes (login, oidc, auth/config) short-circuit: no session,
        // and never a write path, so no CSRF token can or need exist yet.
        if ($auth === 'public') {
            return;
        }

        // Everything else needs a session. index.php already gates non-/api/auth
        // paths, but enforcing here keeps the policy self-contained and also
        // covers authenticated writes under /api/auth/* (e.g. logout).
        Auth::requireSession();

        // Cookie-auth state-changing requests must prove same-origin + token.
        if (self::isWrite($method)) {
            self::enforceCsrf($method);
        }

        switch ($auth) {
            case 'session':
                return;
            case 'operator':
                Operator::requireOperator();   // 403 unless operator/admin
                return;
            case 'admin':
                Operator::requireAdmin();       // 403 unless admin
                return;
            default:
                // An unknown declared level is a bug — fail closed.
                Response::error('forbidden', "Unrecognized policy for $path.", 403);
        }
    }

    // ------------------------------------------------------------------ CSRF

    /**
     * Enforce Origin/Referer + synchronizer token on a state-changing request.
     * Terminates with 403 on any failure.
     */
    private static function enforceCsrf(string $method): void
    {
        $origin = self::requestOrigin();
        if (!self::originAllowed($origin, self::allowedOrigins())) {
            Response::error('forbidden',
                'Cross-origin or missing Origin on a state-changing request.', 403);
        }
        $sent   = (string) ($_SERVER['HTTP_X_SOLARI_CSRF'] ?? '');
        if (!self::tokenOk($sent, Auth::csrfToken())) {
            Response::error('forbidden', 'Missing or invalid CSRF token.', 403);
        }
    }

    /**
     * The origin the browser reports for this request: prefer the Origin header,
     * fall back to the origin parsed from Referer. '' when neither is present
     * (which originAllowed() then rejects for a state-changing request).
     */
    private static function requestOrigin(): string
    {
        $o = (string) ($_SERVER['HTTP_ORIGIN'] ?? '');
        if ($o !== '') {
            return self::normalizeOrigin($o);
        }
        $ref = (string) ($_SERVER['HTTP_REFERER'] ?? '');
        return $ref !== '' ? self::originOf($ref) : '';
    }

    /**
     * The set of origins accepted for state-changing requests: this request's
     * own scheme://host (same-origin — the correct default), plus any extra
     * origins configured via SOLARI_ALLOWED_ORIGINS (comma-separated) for
     * deliberately-served aliases. Tight by design: the token defends the rest.
     *
     * @return string[]
     */
    private static function allowedOrigins(): array
    {
        $out = [];
        $self = self::selfOrigin();
        if ($self !== '') {
            $out[] = $self;
        }
        $extra = getenv('SOLARI_ALLOWED_ORIGINS');
        if (is_string($extra) && $extra !== '') {
            foreach (explode(',', $extra) as $e) {
                $e = self::normalizeOrigin(trim($e));
                if ($e !== '') {
                    $out[] = $e;
                }
            }
        }
        return $out;
    }

    /** scheme://host[:port] for the current request, or '' if undeterminable. */
    private static function selfOrigin(): string
    {
        $host = (string) ($_SERVER['HTTP_HOST'] ?? '');
        if ($host === '') {
            return '';
        }
        $https  = (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off')
               || (($_SERVER['SERVER_PORT'] ?? '') === '443');
        return ($https ? 'https://' : 'http://') . strtolower($host);
    }

    /**
     * Pure origin-allowlist check. Kept static + argument-driven for testing.
     * An empty candidate never matches (a state-changing request must carry a
     * recognizable same-origin marker).
     *
     * @param string   $origin  candidate origin (already normalized)
     * @param string[] $allowed accepted origins
     */
    public static function originAllowed(string $origin, array $allowed): bool
    {
        if ($origin === '') {
            return false;
        }
        return in_array($origin, $allowed, true);
    }

    /**
     * Pure constant-time token comparison guarding empties: an unset stored or
     * sent token never validates.
     */
    public static function tokenOk(string $sent, string $stored): bool
    {
        if ($sent === '' || $stored === '') {
            return false;
        }
        return hash_equals($stored, $sent);
    }

    /** Parse scheme://host[:port] out of a full URL (e.g. a Referer). */
    private static function originOf(string $url): string
    {
        $scheme = parse_url($url, PHP_URL_SCHEME);
        $host   = parse_url($url, PHP_URL_HOST);
        if (!is_string($scheme) || !is_string($host) || $host === '') {
            return '';
        }
        $port = parse_url($url, PHP_URL_PORT);
        $o = strtolower($scheme) . '://' . strtolower($host);
        if (is_int($port)) {
            $o .= ':' . $port;
        }
        return $o;
    }

    /** Normalize an origin string to scheme://host[:port], lower-cased. */
    private static function normalizeOrigin(string $origin): string
    {
        if ($origin === '') {
            return '';
        }
        // An Origin header is already scheme://host[:port]; originOf handles both.
        return self::originOf($origin);
    }
}
