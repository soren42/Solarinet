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

    /** Session key holding the per-session CSRF synchronizer token. */
    private const CSRF_KEY = 'solari_csrf';
    private const CREATED_KEY = 'solari_created_at';
    private const SEEN_KEY = 'solari_seen_at';
    private const IDLE_MAX_SEC = 1800;
    private const ABSOLUTE_MAX_SEC = 28800;
    private const OIDC_MAX_SEC = 3600;

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

    /** Local password login is an explicit break-glass capability. */
    public static function localLoginEnabled(): bool
    {
        return getenv('SOLARI_LOCAL_LOGIN') === '1'
            && self::localRateDirectory() !== null;
    }

    /** Require a private writable directory before accepting password attempts. */
    private static function localRateDirectory(): ?string
    {
        $dir = getenv('SOLARI_AUTH_RATE_DIR');
        if (!is_string($dir) || $dir === '' || $dir[0] !== '/'
            || is_link($dir) || !is_dir($dir) || !is_writable($dir)) {
            return null;
        }
        $mode = fileperms($dir);
        return $mode !== false && ($mode & 0077) === 0 ? $dir : null;
    }

    /** Reserve one of five attempts per source/account in a rolling 15 minutes. */
    public static function consumeLocalAttempt(string $username, string $source): bool
    {
        $dir = self::localRateDirectory();
        if ($dir === null) return false;
        $key = hash('sha256', strtolower(trim($username)) . "\n" . $source);
        $handle = @fopen($dir . '/' . $key, 'c+');
        if ($handle === false) return false;
        try {
            if (!flock($handle, LOCK_EX)) return false;
            @chmod($dir . '/' . $key, 0600);
            $raw = stream_get_contents($handle);
            $old = json_decode(is_string($raw) ? $raw : '', true);
            if ($raw !== '' && !is_array($old)) return false;
            $now = time();
            $recent = is_array($old) ? array_values(array_filter($old,
                static fn ($t): bool => is_int($t) && $t > $now - 900 && $t <= $now)) : [];
            if (count($recent) >= 5) return false;
            $recent[] = $now;
            rewind($handle);
            if (!ftruncate($handle, 0)) return false;
            $bytes = json_encode($recent, JSON_THROW_ON_ERROR);
            if (fwrite($handle, $bytes) !== strlen($bytes) || !fflush($handle)) return false;
            return true;
        } finally {
            flock($handle, LOCK_UN);
            fclose($handle);
        }
    }

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
        if (!self::localLoginEnabled()) {
            return null;
        }
        if (!self::consumeLocalAttempt($username, (string) ($_SERVER['REMOTE_ADDR'] ?? 'unknown'))) {
            return null;
        }
        self::bootSession();
        $principal = self::attempt($username, $password);
        if ($principal === null) {
            return null;
        }
        session_regenerate_id(true);          // prevent fixation
        $_SESSION[self::SESS_KEY]  = $principal;
        $_SESSION[self::CSRF_KEY]  = self::newCsrfToken();   // fresh token per login
        $_SESSION[self::CREATED_KEY] = time();
        $_SESSION[self::SEEN_KEY] = time();
        return $principal;
    }

    /**
     * Pin an already-verified principal into a fresh session. This is the
     * externally-authenticated counterpart to login(): the caller (e.g. the
     * OIDC callback in Oidc.php) has proven the identity by other means, so we
     * only normalise the principal shape and establish the session. It shares
     * the exact session model (fixation-safe id regeneration, SESS_KEY) with
     * local login, so downstream code — Operator.php, the auth gate, whoami —
     * cannot tell the two paths apart. Returns the stored principal.
     *
     * @param array{username:string,role:string,displayName?:string,source?:string} $principal
     */
    public static function establishSession(array $principal): array
    {
        self::bootSession();
        $principal = [
            'username'    => (string) ($principal['username'] ?? ''),
            'role'        => strtolower((string) ($principal['role'] ?? 'viewer')),
            'displayName' => (string) ($principal['displayName'] ?? ($principal['username'] ?? '')),
            'source'      => (string) ($principal['source'] ?? 'directory'),
            'oidcSubject' => (string) ($principal['oidcSubject'] ?? ''),
        ];
        session_regenerate_id(true);          // prevent fixation
        $_SESSION[self::SESS_KEY]  = $principal;
        $_SESSION[self::CSRF_KEY]  = self::newCsrfToken();   // fresh token per login
        $_SESSION[self::CREATED_KEY] = time();
        $_SESSION[self::SEEN_KEY] = time();
        if ($principal['source'] === 'oidc') {
            $_SESSION['solari_oidc_role_checked_at'] = time();
        }
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
        if (!is_array($p)) return null;
        $now = time();
        $created = $_SESSION[self::CREATED_KEY] ?? null;
        $seen = $_SESSION[self::SEEN_KEY] ?? null;
        $absolute = ($p['source'] ?? '') === 'oidc'
            ? self::OIDC_MAX_SEC : self::ABSOLUTE_MAX_SEC;
        if (!is_int($created) || !is_int($seen) || $created > $now
            || $seen > $now || $now - $created >= $absolute
            || $now - $seen >= self::IDLE_MAX_SEC) {
            self::logout();
            return null;
        }
        if (($p['source'] ?? '') === 'local') {
            if (!self::localLoginEnabled()) {
                self::logout();
                return null;
            }
            // Reload each request's credential store; a deleted or downgraded
            // local account must not retain an older session role.
            $account = null;
            foreach (self::store()['users'] ?? [] as $candidate) {
                if (is_array($candidate)
                    && ($candidate['username'] ?? null) === ($p['username'] ?? null)) {
                    $account = $candidate;
                    break;
                }
            }
            if ($account === null || !is_string($account['role'] ?? null)) {
                self::logout();
                return null;
            }
            $p['role'] = strtolower($account['role']);
            if (!in_array($p['role'], ['viewer', 'operator', 'admin'], true)) {
                self::logout();
                return null;
            }
            $p['displayName'] = (string) ($account['displayName'] ?? $p['username']);
            $_SESSION[self::SESS_KEY] = $p;
        } elseif (($p['source'] ?? '') === 'oidc') {
            require_once __DIR__ . '/Oidc.php';
            if (!Oidc::revalidateCurrentSession()) {
                self::logout();
                return null;
            }
            $p = $_SESSION[self::SESS_KEY] ?? null;
            if (!is_array($p)) {
                self::logout();
                return null;
            }
        }
        $_SESSION[self::SEEN_KEY] = $now;
        return $p;
    }

    /**
     * The CSRF synchronizer token bound to the current session, or '' when no
     * principal is authenticated. Lazily mints one for sessions that predate
     * this token (e.g. a session established before the CSRF layer shipped), so
     * an already-logged-in browser is not locked out until it re-authenticates.
     * The token is opaque, HttpOnly-session-scoped, and echoed by the SPA in the
     * X-Solari-CSRF header on every state-changing request (see lib/Policy.php).
     */
    public static function csrfToken(): string
    {
        self::bootSession();
        if (self::current() === null) {
            return '';                         // no token without a principal
        }
        if (empty($_SESSION[self::CSRF_KEY]) || !is_string($_SESSION[self::CSRF_KEY])) {
            $_SESSION[self::CSRF_KEY] = self::newCsrfToken();
        }
        return (string) $_SESSION[self::CSRF_KEY];
    }

    /** Generate a fresh high-entropy CSRF token. */
    private static function newCsrfToken(): string
    {
        return bin2hex(random_bytes(32));
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
