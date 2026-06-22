<?php
declare(strict_types=1);

/**
 * Auth — authentication for the SolariNet dashboard (§11.1).
 *
 * QUICK-AND-DIRTY LOCAL MODE (current pass)
 * -----------------------------------------
 * Authenticates against a JSON credential file in the project root
 * (solari-auth.json, overridable via $SOLARI_AUTH_FILE). Each user carries a
 * salted password hash in PHP's crypt() / password_hash() format — i.e. the
 * modern "$id$salt$hash" shadow form (bcrypt by default). On success the
 * principal is pinned into a server-side PHP session; the browser only ever
 * holds an opaque HttpOnly session cookie, never the credentials.
 *
 * This is intentionally minimal: it is meant for a single admin on a trusted
 * intranet. The directory-service path (LDAP/AD) is stubbed below and wired to
 * the same principal shape, so the next pass can implement
 * directoryAuthenticate() without touching the routes, the session model, or
 * Operator.php.
 *
 * Principal shape (also the shape stored in $_SESSION['solari']):
 *   [ 'username' => string, 'role' => 'admin'|'operator'|'viewer',
 *     'displayName' => string, 'source' => 'local'|'directory' ]
 */
final class Auth
{
    /** Session key holding the authenticated principal. */
    private const SESS_KEY = 'solari';

    /** @var array<string,mixed>|null cached credential store */
    private static ?array $store = null;

    // ----------------------------------------------------------------- store

    /** Absolute path to the credential file (env override, else project root). */
    public static function authFile(): string
    {
        $env = getenv('SOLARI_AUTH_FILE');
        if (is_string($env) && $env !== '') {
            return $env;
        }
        // lib/ -> api/ -> dashboard/ -> repo root
        return dirname(__DIR__, 3) . '/solari-auth.json';
    }

    /** Load + cache the credential store. Returns a safe empty shape on error. */
    public static function store(): array
    {
        if (self::$store !== null) {
            return self::$store;
        }
        $empty = ['version' => 1, 'users' => [], 'directory' => ['enabled' => false]];
        $path  = self::authFile();
        if (!is_file($path) || !is_readable($path)) {
            error_log("[solari-auth] credential file not found/readable: $path");
            return self::$store = $empty;
        }
        $raw     = (string) file_get_contents($path);
        $decoded = json_decode($raw, true);
        if (!is_array($decoded)) {
            error_log("[solari-auth] credential file is not valid JSON: $path");
            return self::$store = $empty;
        }
        return self::$store = $decoded + $empty;
    }

    // --------------------------------------------------------------- session

    /** Configure + start the session exactly once. Safe to call repeatedly. */
    public static function bootSession(): void
    {
        if (session_status() === PHP_SESSION_ACTIVE) {
            return;
        }
        $secure = (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off')
               || (($_SERVER['SERVER_PORT'] ?? '') === '443');
        session_name('solari_sess');
        session_set_cookie_params([
            'lifetime' => 0,             // session cookie
            'path'     => '/',
            'httponly' => true,          // not visible to JS
            'secure'   => $secure,       // HTTPS only when served over TLS
            'samesite' => 'Lax',
        ]);
        @session_start();
    }

    // ----------------------------------------------------------- credentials

    /**
     * Verify a username/password and, on success, return the principal.
     * Tries local users first, then the (stubbed) directory service.
     * Returns null on any failure (no distinction, to avoid user enumeration).
     */
    public static function attempt(string $username, string $password): ?array
    {
        $username = trim($username);
        if ($username === '' || $password === '') {
            return null;
        }

        $local = self::verifyLocal($username, $password);
        if ($local !== null) {
            return $local;
        }

        // Directory service (LDAP/AD) — NON-FUNCTIONAL STUB for the next pass.
        return self::directoryAuthenticate($username, $password);
    }

    /** Local credential-file check. */
    private static function verifyLocal(string $username, string $password): ?array
    {
        foreach (self::store()['users'] ?? [] as $u) {
            if (!is_array($u) || ($u['username'] ?? null) !== $username) {
                continue;
            }
            $hash = (string) ($u['passwordHash'] ?? '');
            if ($hash !== '' && password_verify($password, $hash)) {
                return [
                    'username'    => $username,
                    'role'        => strtolower((string) ($u['role'] ?? 'admin')),
                    'displayName' => (string) ($u['displayName'] ?? $username),
                    'source'      => 'local',
                ];
            }
            return null; // user matched, password did not
        }
        return null;
    }

    /**
     * Directory-service authentication — STUB.
     *
     * Wired into the same principal shape so the next pass can implement LDAP/AD
     * bind here (using the [directory] block of the credential file) without
     * changing callers. Currently always returns null (disabled). See
     * directoryConfig() for the configuration surface already in place.
     */
    public static function directoryAuthenticate(string $username, string $password): ?array
    {
        $cfg = self::directoryConfig();
        if (empty($cfg['enabled'])) {
            return null; // DS not deployed yet
        }
        // TODO(next pass): ldap_connect()/ldap_bind() against $cfg, map the
        // role attribute through $cfg['roleMap'], return the principal. Until
        // then we fail closed so a half-configured DS cannot grant access.
        error_log('[solari-auth] directory auth requested but not implemented; denying');
        return null;
    }

    /** The directory-service config block (stub surface for the next pass). */
    public static function directoryConfig(): array
    {
        $d = self::store()['directory'] ?? [];
        return is_array($d) ? $d : [];
    }

    // ------------------------------------------------------------- lifecycle

    /** Establish a session for a verified principal. Returns the principal. */
    public static function login(string $username, string $password): ?array
    {
        self::bootSession();
        $principal = self::attempt($username, $password);
        if ($principal === null) {
            return null;
        }
        session_regenerate_id(true);          // prevent fixation
        $_SESSION[self::SESS_KEY] = $principal;
        return $principal;
    }

    /** Tear down the current session. */
    public static function logout(): void
    {
        self::bootSession();
        $_SESSION = [];
        if (ini_get('session.use_cookies')) {
            $p = session_get_cookie_params();
            setcookie(session_name(), '', time() - 42000, $p['path'],
                      $p['domain'] ?? '', (bool) $p['secure'], (bool) $p['httponly']);
        }
        @session_destroy();
    }

    /** The current authenticated principal, or null. */
    public static function current(): ?array
    {
        self::bootSession();
        $p = $_SESSION[self::SESS_KEY] ?? null;
        return is_array($p) ? $p : null;
    }

    /**
     * Require an authenticated session; emit 401 and terminate if absent.
     * Returns the principal on success.
     */
    public static function requireSession(): array
    {
        $p = self::current();
        if ($p === null) {
            Response::error('unauthorized', 'Authentication required.', 401);
        }
        return $p;
    }
}
