<?php
declare(strict_types=1);

require_once __DIR__ . '/Auth.php';
require_once __DIR__ . '/Response.php';

/**
 * Oidc — OpenID Connect authorization-code login for the SolariNet dashboard.
 *
 * ADDITIVE, CONFIG-GATED SSO
 * --------------------------
 * This sits alongside the existing local (solari-auth.json / bcrypt) login and
 * the stubbed directory path in Auth.php. It is completely inert unless the
 * `oidc` block of the credential file has `enabled: true` and the minimum config
 * (issuer, clientId, clientSecret, redirectUri). When disabled or misconfigured
 * the dashboard behaves exactly as before — the login screen only offers the
 * local username/password form.
 *
 * FLOW (confidential client, authorization code)
 *   1. Browser hits GET /api/auth/oidc/login. We stash a fresh state+nonce in
 *      the PHP session and 302 to the Keycloak authorization endpoint.
 *   2. Keycloak authenticates the user (AD-federated) and redirects back to
 *      GET /api/auth/oidc/callback?code=...&state=...
 *   3. We verify state, exchange the code for tokens at the token endpoint,
 *      validate the ID token (signature via JWKS, iss/aud/exp/nonce), map the
 *      claims to a dashboard operator + role, and pin the principal and token
 *      refresh data into the same server-side session the local login uses.
 *   4. We 302 back to the dashboard; the SPA re-boots authenticated.
 *   5. Authenticated requests refresh group-derived roles from HTTPS UserInfo
 *      at most every 60 seconds, rotating access tokens when required.
 *
 * TLS: Keycloak here uses a self-signed cert. All server-side calls (discovery,
 * token, JWKS) verify the peer against `oidc.caBundle` (a PEM CA file). A dev-
 * only `oidc.insecureSkipVerify` escape hatch exists but defaults to false and
 * is loudly logged; never enable it in production.
 *
 * No new Composer/vendored dependency: discovery + token exchange use PHP
 * streams (the curl extension is not required), and ID-token signatures are
 * verified with ext-openssl. Only the RS256/384/512 family (Keycloak's default
 * realm signing algorithm) is supported.
 */
final class Oidc
{
    /** Session keys for the in-flight transaction (state/nonce). */
    private const SESS_TX = 'solari_oidc_tx';
    private const MAX_STATE_AGE_SEC = 600;
    private const ROLE_RECHECK_SEC = 60;
    private const MAX_HTTP_BODY_BYTES = 1048576;

    /** Accept a one-use callback only within ten minutes of its login request. */
    public static function validTransaction(array $tx, string $state, int $now): bool
    {
        $saved = $tx['state'] ?? null;
        $nonce = $tx['nonce'] ?? null;
        $started = $tx['ts'] ?? null;
        return is_string($saved) && $saved !== '' && $state !== ''
            && is_string($nonce) && $nonce !== ''
            && is_int($started) && $started <= $now
            && $now - $started <= self::MAX_STATE_AGE_SEC
            && hash_equals($saved, $state);
    }

    // ------------------------------------------------------------------ config

    /** The `oidc` config block, merged over safe defaults. */
    public static function config(): array
    {
        $store = Auth::store();
        $c = (isset($store['oidc']) && is_array($store['oidc'])) ? $store['oidc'] : [];
        return $c + [
            'enabled'            => false,
            'issuer'             => '',
            'allowedEndpointOrigins' => [], // explicit HTTPS origins for split-host IdPs
            'clientId'           => '',
            'clientSecret'       => '',
            'redirectUri'        => '',
            'caBundle'           => '',        // PEM file trusted for the IdP's TLS cert
            'insecureSkipVerify' => false,     // DEV ONLY — never true in production
            'scopes'             => 'openid profile email',
            'buttonLabel'        => 'Sign in with SSO',
            'adminGroups'        => ['domain admins'],
            'operatorGroups'     => [],
            'defaultRole'        => 'viewer',  // least-privileged existing role
            'postLoginRedirect'  => '/',
        ];
    }

    /**
     * True only when SSO is switched on AND the mandatory fields are present.
     * A half-filled block reads as "off" so the dashboard falls back to local
     * login rather than offering a button that cannot work.
     */
    public static function isEnabled(): bool
    {
        $c = self::config();
        return !empty($c['enabled'])
            && is_string($c['issuer'])       && $c['issuer'] !== ''
            && is_string($c['clientId'])     && $c['clientId'] !== ''
            && is_string($c['clientSecret']) && $c['clientSecret'] !== ''
            && is_string($c['redirectUri'])  && $c['redirectUri'] !== '';
    }

    /** The label shown on the SSO button (safe to expose unauthenticated). */
    public static function buttonLabel(): string
    {
        $c = self::config();
        $l = (string) ($c['buttonLabel'] ?? '');
        return $l !== '' ? $l : 'Sign in with SSO';
    }

    // ------------------------------------------------------------- begin login

    /**
     * Start the code flow: stash state+nonce in the session and redirect the
     * browser to the IdP's authorization endpoint. Never returns.
     */
    public static function beginLogin(): void
    {
        if (!self::isEnabled()) {
            self::fail('OIDC is not enabled.');
        }
        Auth::bootSession();

        $disc = self::discovery();
        $authEp = (string) ($disc['authorization_endpoint'] ?? '');
        if ($authEp === '' || !self::endpointAllowedForConfig($authEp, self::config())) {
            self::fail('Authorization endpoint missing from discovery document.');
        }

        $c     = self::config();
        $state = self::randomToken();
        $nonce = self::randomToken();
        $_SESSION[self::SESS_TX] = ['state' => $state, 'nonce' => $nonce, 'ts' => time()];

        $q = http_build_query([
            'response_type' => 'code',
            'client_id'     => (string) $c['clientId'],
            'redirect_uri'  => (string) $c['redirectUri'],
            'scope'         => (string) $c['scopes'],
            'state'         => $state,
            'nonce'         => $nonce,
        ]);
        self::redirect($authEp . (strpos($authEp, '?') === false ? '?' : '&') . $q);
    }

    // ---------------------------------------------------------------- callback

    /**
     * Handle the IdP redirect: verify state, exchange the code, validate the ID
     * token, establish the dashboard session, then redirect into the app. Never
     * returns.
     */
    public static function handleCallback(array $query): void
    {
        if (!self::isEnabled()) {
            self::fail('OIDC is not enabled.');
        }
        Auth::bootSession();

        // Surface an IdP-side error (e.g. access_denied) cleanly.
        if (isset($query['error'])) {
            self::fail('SSO provider returned: ' . (string) $query['error']);
        }

        $tx = $_SESSION[self::SESS_TX] ?? null;
        unset($_SESSION[self::SESS_TX]);           // one-shot
        $code  = (string) ($query['code'] ?? '');
        $state = (string) ($query['state'] ?? '');
        if ($code === '' || !is_array($tx)
            || !self::validTransaction($tx, $state, time())) {
            self::fail('Invalid or expired SSO login attempt. Please try again.');
        }

        $tokens    = self::exchangeAndVerify($code, (string) $tx['nonce']);
        $claims    = $tokens['claims'];
        $principal = self::principalFromClaims($claims);
        $principal['oidcSubject'] = (string) $claims['sub'];
        Auth::establishSession($principal);
        $_SESSION['solari_oidc_access_token'] = $tokens['accessToken'];
        $_SESSION['solari_oidc_refresh_token'] = $tokens['refreshToken'];
        $_SESSION['solari_oidc_access_expires_at'] = $tokens['accessExpiresAt'];
        $_SESSION['solari_oidc_role_checked_at'] = time();

        $c = self::config();
        self::redirect((string) ($c['postLoginRedirect'] ?: '/'));
    }

    /** Exchange the auth code, validate its ID token, and retain server-side refresh data. */
    private static function exchangeAndVerify(string $code, string $nonce): array
    {
        $c    = self::config();
        $disc = self::discovery();
        $tokenEp = (string) ($disc['token_endpoint'] ?? '');
        if ($tokenEp === '') {
            self::fail('Token endpoint missing from discovery document.');
        }

        $resp = self::httpForm($tokenEp, [
            'grant_type'    => 'authorization_code',
            'code'          => $code,
            'redirect_uri'  => (string) $c['redirectUri'],
            'client_id'     => (string) $c['clientId'],
            'client_secret' => (string) $c['clientSecret'],
        ]);
        $tok = json_decode($resp, true);
        if (!is_array($tok) || empty($tok['id_token']) || empty($tok['access_token'])
            || !is_string($tok['access_token']) || !self::isSafeHeaderToken($tok['access_token'])
            || !is_string($tok['token_type'] ?? null)
            || strcasecmp($tok['token_type'], 'Bearer') !== 0) {
            error_log('[solari-oidc] token endpoint returned no id_token: ' . substr($resp, 0, 300));
            self::fail('SSO token exchange failed.');
        }

        $expiresIn = $tok['expires_in'] ?? null;
        if (!is_int($expiresIn) || $expiresIn < 1 || $expiresIn > 86400) {
            self::fail('SSO access-token lifetime is invalid.');
        }
        $refresh = $tok['refresh_token'] ?? '';
        if (!is_string($refresh) || strlen($refresh) > 16384
            || preg_match('/[\x00-\x1F\x7F]/', $refresh)) {
            self::fail('SSO refresh token is invalid.');
        }
        return [
            'claims' => self::verifyIdToken((string) $tok['id_token'], $nonce),
            'accessToken' => $tok['access_token'],
            'refreshToken' => $refresh,
            'accessExpiresAt' => time() + $expiresIn,
        ];
    }

    // --------------------------------------------------------- token validation

    /**
     * Validate a Keycloak ID token: RS* signature against the JWKS, plus the
     * iss / aud / exp / nonce claims. Returns the decoded claim set on success;
     * fails closed (redirect to login with an error) otherwise.
     */
    private static function verifyIdToken(string $jwt, string $expectedNonce): array
    {
        $parts = explode('.', $jwt);
        if (count($parts) !== 3) {
            self::fail('Malformed ID token.');
        }
        [$h64, $p64, $s64] = $parts;
        $header  = json_decode(self::b64uDecode($h64), true);
        $claims  = json_decode(self::b64uDecode($p64), true);
        $sig     = self::b64uDecode($s64);
        if (!is_array($header) || !is_array($claims)) {
            self::fail('Unreadable ID token.');
        }

        $alg = (string) ($header['alg'] ?? '');
        $algo = [
            'RS256' => OPENSSL_ALGO_SHA256,
            'RS384' => OPENSSL_ALGO_SHA384,
            'RS512' => OPENSSL_ALGO_SHA512,
        ][$alg] ?? null;
        if ($algo === null) {
            error_log("[solari-oidc] unsupported ID-token alg: $alg");
            self::fail('Unsupported SSO token signature algorithm.');
        }

        $pem = self::jwkPemForKid((string) ($header['kid'] ?? ''));
        $ok  = openssl_verify($h64 . '.' . $p64, $sig, $pem, $algo);
        if ($ok !== 1) {
            self::fail('ID token signature verification failed.');
        }

        $claimError = self::claimError($claims, self::config(), $expectedNonce, time());
        if ($claimError !== null) self::fail($claimError);

        return $claims;
    }

    /** Return the first OIDC claim defect; pure so negative cases are testable. */
    public static function claimError(array $claims, array $config,
                                      string $expectedNonce, int $now): ?string
    {
        $issuer = $claims['iss'] ?? null;
        if (!is_string($issuer) || $issuer === ''
            || !hash_equals(rtrim((string) ($config['issuer'] ?? ''), '/'), rtrim($issuer, '/'))) {
            return 'ID token issuer mismatch.';
        }
        $clientId = $config['clientId'] ?? null;
        $aud = $claims['aud'] ?? null;
        if (!is_string($clientId) || $clientId === ''
            || !(is_string($aud) ? $aud === $clientId
                : (is_array($aud) && in_array($clientId, $aud, true)))) {
            return 'ID token audience mismatch.';
        }
        $azp = $claims['azp'] ?? null;
        if ((is_array($aud) && count($aud) > 1 && $azp === null)
            || ($azp !== null && $azp !== $clientId)) {
            return 'ID token authorized party mismatch.';
        }
        if (!is_string($claims['sub'] ?? null) || $claims['sub'] === '') {
            return 'ID token subject missing.';
        }
        $exp = $claims['exp'] ?? null;
        $iat = $claims['iat'] ?? null;
        if (!is_int($exp) || !is_int($iat) || $exp <= $iat
            || $now >= $exp + 60 || $iat > $now + 60) {
            return 'ID token lifetime invalid.';
        }
        if (array_key_exists('nbf', $claims)
            && (!is_int($claims['nbf']) || $now + 60 < $claims['nbf'])) {
            return 'ID token not yet valid.';
        }
        $nonce = $claims['nonce'] ?? null;
        if ($expectedNonce === '' || !is_string($nonce)
            || !hash_equals($expectedNonce, $nonce)) {
            return 'ID token nonce mismatch.';
        }
        return null;
    }

    /** Refresh OIDC group-derived roles from the provider's UserInfo endpoint. */
    public static function revalidateCurrentSession(): bool
    {
        if (!self::isEnabled()) return false;
        $principal = $_SESSION['solari'] ?? null;
        if (!is_array($principal) || ($principal['source'] ?? '') !== 'oidc'
            || !is_string($principal['oidcSubject'] ?? null)
            || $principal['oidcSubject'] === '') {
            return false;
        }
        $now = time();
        $checked = $_SESSION['solari_oidc_role_checked_at'] ?? null;
        if (is_int($checked) && $checked <= $now && $now - $checked < self::ROLE_RECHECK_SEC) {
            return true;
        }
        if (!empty(self::config()['insecureSkipVerify'])) {
            self::setSessionRole('viewer', $now);
            return true;
        }

        $expiresAt = $_SESSION['solari_oidc_access_expires_at'] ?? 0;
        if (!is_int($expiresAt) || $expiresAt <= $now + 10) {
            $refresh = $_SESSION['solari_oidc_refresh_token'] ?? '';
            if (!is_string($refresh) || $refresh === '' || !self::refreshAccessToken($refresh)) {
                self::setSessionRole('viewer', $now);
                return true;
            }
        }

        $discovery = self::discovery(true, 3);
        $endpoint = is_array($discovery) ? ($discovery['userinfo_endpoint'] ?? null) : null;
        $access = $_SESSION['solari_oidc_access_token'] ?? '';
        if (!is_string($endpoint) || !self::isHttpsEndpointUrl($endpoint)
            || !is_string($access) || !self::isSafeHeaderToken($access)) {
            self::setSessionRole('viewer', $now);
            return true;
        }
        $body = self::http($endpoint, 'GET', null, ['Authorization: Bearer ' . $access], true, 3);
        $claims = $body === '' ? null : json_decode($body, true);
        if (!is_array($claims)) {
            self::setSessionRole('viewer', $now);
            return true;
        }
        if (!is_string($claims['sub'] ?? null)
            || !hash_equals($principal['oidcSubject'], $claims['sub'])) {
            error_log('[solari-oidc] UserInfo subject changed; revoking dashboard session');
            return false;
        }
        $principal['role'] = self::roleForClaims($claims, self::config());
        if (is_string($claims['name'] ?? null) && $claims['name'] !== '') {
            $principal['displayName'] = $claims['name'];
        }
        $_SESSION['solari'] = $principal;
        $_SESSION['solari_oidc_role_checked_at'] = $now;
        return true;
    }

    private static function refreshAccessToken(string $refreshToken): bool
    {
        if (strlen($refreshToken) > 16384 || preg_match('/[\x00-\x1F\x7F]/', $refreshToken)) return false;
        $discovery = self::discovery(true, 3);
        $endpoint = is_array($discovery) ? ($discovery['token_endpoint'] ?? null) : null;
        if (!is_string($endpoint) || !self::isHttpsEndpointUrl($endpoint)) return false;
        $c = self::config();
        $body = self::http($endpoint, 'POST', http_build_query([
            'grant_type' => 'refresh_token',
            'refresh_token' => $refreshToken,
            'client_id' => (string) $c['clientId'],
            'client_secret' => (string) $c['clientSecret'],
        ]), ['Content-Type: application/x-www-form-urlencoded'], true, 3);
        $tokens = $body === '' ? null : json_decode($body, true);
        if (!is_array($tokens) || !is_string($tokens['access_token'] ?? null)
            || $tokens['access_token'] === '' || strlen($tokens['access_token']) > 16384
            || !self::isSafeHeaderToken($tokens['access_token'])) return false;
        if (!is_string($tokens['token_type'] ?? null)
            || strcasecmp($tokens['token_type'], 'Bearer') !== 0) return false;
        $expiresIn = $tokens['expires_in'] ?? null;
        if (!is_int($expiresIn) || $expiresIn < 1 || $expiresIn > 86400) return false;
        $rotated = $tokens['refresh_token'] ?? $refreshToken;
        if ($rotated === '') $rotated = $refreshToken;
        if (!is_string($rotated) || $rotated === '' || strlen($rotated) > 16384
            || preg_match('/[\x00-\x1F\x7F]/', $rotated)) return false;
        $_SESSION['solari_oidc_access_token'] = $tokens['access_token'];
        $_SESSION['solari_oidc_refresh_token'] = $rotated;
        $_SESSION['solari_oidc_access_expires_at'] = time() + $expiresIn;
        return true;
    }

    private static function setSessionRole(string $role, int $checkedAt): void
    {
        $principal = $_SESSION['solari'] ?? null;
        if (is_array($principal)) {
            $principal['role'] = $role;
            $_SESSION['solari'] = $principal;
        }
        $_SESSION['solari_oidc_role_checked_at'] = $checkedAt;
    }

    private static function isHttpsEndpointUrl(string $url): bool
    {
        $parts = parse_url($url);
        return is_array($parts) && ($parts['scheme'] ?? '') === 'https'
            && is_string($parts['host'] ?? null) && $parts['host'] !== ''
            && !isset($parts['user']) && !isset($parts['pass']);
    }

    /** Origin comparison prevents discovery metadata from exfiltrating secrets/tokens. */
    public static function endpointAllowedForConfig(string $url, array $config): bool
    {
        $target = self::httpsOrigin($url);
        if ($target === null) return false;
        $issuerOrigin = self::httpsOrigin((string) ($config['issuer'] ?? ''));
        if ($issuerOrigin === null) return false;
        $configured = $config['allowedEndpointOrigins'] ?? [];
        if (!is_array($configured)) return false;
        $allowed = [$issuerOrigin];
        foreach ($configured as $origin) {
            if (!is_string($origin)) return false;
            $parsed = self::httpsOrigin($origin);
            // Allowlist entries are origins, never URLs with paths, queries, or fragments.
            $parts = parse_url($origin);
            if ($parsed === null || !is_array($parts)
                || isset($parts['path']) && $parts['path'] !== ''
                || isset($parts['query']) || isset($parts['fragment'])) return false;
            $allowed[] = $parsed;
        }
        return in_array($target, array_unique($allowed), true);
    }

    private static function httpsOrigin(string $url): ?string
    {
        $parts = parse_url($url);
        if (!is_array($parts) || strtolower((string) ($parts['scheme'] ?? '')) !== 'https'
            || !is_string($parts['host'] ?? null) || $parts['host'] === ''
            || isset($parts['user']) || isset($parts['pass'])) return null;
        $host = strtolower($parts['host']);
        $port = isset($parts['port']) ? (int) $parts['port'] : 443;
        if ($port < 1 || $port > 65535) return null;
        return 'https://' . (str_contains($host, ':') ? '[' . $host . ']' : $host)
            . ($port === 443 ? '' : ':' . $port);
    }

    private static function isSafeHeaderToken(string $token): bool
    {
        return $token !== '' && strlen($token) <= 16384
            && preg_match('/[\x00-\x1F\x7F]/', $token) !== 1;
    }

    /** Find the JWKS key with the given kid and return it as a PEM public key. */
    private static function jwkPemForKid(string $kid): string
    {
        $disc = self::discovery();
        $jwksUri = (string) ($disc['jwks_uri'] ?? '');
        if ($jwksUri === '') {
            self::fail('JWKS URI missing from discovery document.');
        }
        $jwks = json_decode(self::httpGet($jwksUri), true);
        $keys = (is_array($jwks) && isset($jwks['keys']) && is_array($jwks['keys'])) ? $jwks['keys'] : [];

        foreach ($keys as $k) {
            if (!is_array($k) || ($k['kty'] ?? '') !== 'RSA') {
                continue;
            }
            if ($kid !== '' && (string) ($k['kid'] ?? '') !== $kid) {
                continue;
            }
            if (empty($k['n']) || empty($k['e'])) {
                continue;
            }
            return self::rsaJwkToPem((string) $k['n'], (string) $k['e']);
        }
        self::fail('No matching signing key found in JWKS.');
    }

    // ------------------------------------------------------------ claim mapping

    /**
     * Map validated ID-token claims onto the existing dashboard principal shape.
     * AD "domain admins" (via the `groups` claim or realm roles) -> admin; an
     * optional operatorGroups list -> operator; everyone else -> defaultRole
     * (the least-privileged existing role, viewer).
     */
    public static function principalFromClaims(array $claims): array
    {
        $c = self::config();

        $username = (string) ($claims['preferred_username']
            ?? $claims['email'] ?? $claims['sub'] ?? '');
        if ($username === '') {
            self::fail('SSO identity carried no username claim.');
        }
        $display = (string) ($claims['name'] ?? $claims['preferred_username'] ?? $username);

        $role = self::roleForClaims($claims, $c);

        return [
            'username'    => $username,
            'role'        => $role,
            'displayName' => $display,
            'source'      => 'oidc',
            'oidcSubject' => (string) ($claims['sub'] ?? ''),
        ];
    }

    /** Compute authorization role from one validated provider claim set. */
    public static function roleForClaims(array $claims, array $config): string
    {
        $memberships = self::gatherGroups($claims);
        $role = strtolower((string) ($config['defaultRole'] ?: 'viewer'));
        if (!in_array($role, ['viewer', 'operator', 'admin'], true)) $role = 'viewer';
        if (self::intersectsCI($memberships, (array) ($config['operatorGroups'] ?? []))) {
            $role = 'operator';
        }
        if (self::intersectsCI($memberships, (array) ($config['adminGroups'] ?? []))) {
            $role = 'admin';   // admin wins over operator
        }
        return $role;
    }

    /** Collect group/role names from the common Keycloak claim shapes. */
    private static function gatherGroups(array $claims): array
    {
        $out = [];
        if (isset($claims['groups']) && is_array($claims['groups'])) {
            $out = array_merge($out, $claims['groups']);
        }
        if (isset($claims['realm_access']['roles']) && is_array($claims['realm_access']['roles'])) {
            $out = array_merge($out, $claims['realm_access']['roles']);
        }
        // Normalise: strip a leading "/" that Keycloak group paths carry.
        return array_map(static fn($g) => ltrim((string) $g, '/'), $out);
    }

    /** Case-insensitive set intersection test. */
    private static function intersectsCI(array $have, array $want): bool
    {
        if (!$have || !$want) {
            return false;
        }
        $haveL = array_map(static fn($v) => strtolower(trim((string) $v)), $have);
        foreach ($want as $w) {
            if (in_array(strtolower(trim((string) $w)), $haveL, true)) {
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------- HTTP + discovery

    /** Fetch + cache the OIDC discovery document for this request. */
    private static function discovery(bool $softFailure = false, int $timeout = 10): array
    {
        static $cache = null;
        if ($cache !== null) {
            return $cache;
        }
        $c   = self::config();
        $url = rtrim((string) $c['issuer'], '/') . '/.well-known/openid-configuration';
        $body = self::httpGet($url, $softFailure, $timeout);
        if ($softFailure && $body === '') return [];
        $doc = json_decode($body, true);
        if (!is_array($doc) || empty($doc['authorization_endpoint'])) {
            if ($softFailure) return [];
            error_log("[solari-oidc] discovery failed at $url");
            self::fail('Could not reach the SSO provider (discovery).');
        }
        $issuer = (string) ($c['issuer'] ?? '');
        if (!is_string($doc['issuer'] ?? null)
            || !hash_equals(rtrim($issuer, '/'), rtrim($doc['issuer'], '/'))) {
            if ($softFailure) return [];
            self::fail('SSO discovery issuer does not match the configured issuer.');
        }
        foreach (['authorization_endpoint', 'token_endpoint', 'jwks_uri'] as $field) {
            if (!is_string($doc[$field] ?? null)
                || !self::endpointAllowedForConfig($doc[$field], $c)) {
                if ($softFailure) return [];
                self::fail('SSO discovery contains an endpoint outside the configured HTTPS origin allowlist.');
            }
        }
        if (isset($doc['userinfo_endpoint'])
            && (!is_string($doc['userinfo_endpoint'])
                || !self::endpointAllowedForConfig($doc['userinfo_endpoint'], $c))) {
            if ($softFailure) return [];
            self::fail('SSO UserInfo endpoint is outside the configured HTTPS origin allowlist.');
        }
        return $cache = $doc;
    }

    /** HTTP GET with TLS verification against the configured CA bundle. */
    private static function httpGet(string $url, bool $softFailure = false, int $timeout = 10): string
    {
        return self::http($url, 'GET', null, [], $softFailure, $timeout);
    }

    /** HTTP POST of a urlencoded form body. */
    private static function httpForm(string $url, array $fields): string
    {
        return self::http($url, 'POST', http_build_query($fields), [
            'Content-Type: application/x-www-form-urlencoded',
        ]);
    }

    /**
     * Minimal PHP-streams HTTP client. Verifies the peer TLS certificate against
     * oidc.caBundle (system trust store when unset). The insecureSkipVerify
     * escape hatch is DEV ONLY and loudly logged.
     */
    private static function http(string $url, string $method, ?string $body, array $headers,
                                 bool $softFailure = false, int $timeout = 10): string
    {
        if (!self::endpointAllowedForConfig($url, self::config())) {
            error_log('[solari-oidc] refusing an endpoint outside the configured HTTPS origin allowlist');
            if ($softFailure) return '';
            self::fail('SSO provider endpoint is not on the configured HTTPS origin allowlist.');
            return '';
        }
        $c   = self::config();
        $ssl = ['verify_peer' => true, 'verify_peer_name' => true, 'allow_self_signed' => false];

        $ca = (string) ($c['caBundle'] ?? '');
        if ($ca !== '') {
            if (!is_readable($ca)) {
                error_log("[solari-oidc] caBundle not readable: $ca");
                self::fail('SSO CA bundle is not readable on the server.');
            }
            $ssl['cafile'] = $ca;
        }
        if (!empty($c['insecureSkipVerify'])) {
            // DEV FALLBACK — disables TLS verification. Never enable in prod.
            error_log('[solari-oidc] WARNING: TLS verification DISABLED (oidc.insecureSkipVerify=true)');
            $ssl = ['verify_peer' => false, 'verify_peer_name' => false, 'allow_self_signed' => true];
        }

        $hdr = array_merge(['Accept: application/json'], $headers);
        $ctx = stream_context_create([
            'http' => [
                'method'        => $method,
                'header'        => implode("\r\n", $hdr),
                'content'       => $body,
                'timeout'       => max(1, min(30, $timeout)),
                'follow_location' => 0,
                'max_redirects' => 0,
                'ignore_errors' => true,   // read body on 4xx/5xx too
            ],
            'ssl' => $ssl,
        ]);

        $stream = @fopen($url, 'rb', false, $ctx);
        if ($stream === false) {
            $err = error_get_last();
            error_log('[solari-oidc] HTTP ' . $method . ' failed for ' . $url . ': '
                . ($err['message'] ?? 'unknown'));
            if ($softFailure) return '';
            self::fail('Could not reach the SSO provider.');
            return '';
        }
        $statusLine = $http_response_header[0] ?? '';
        if (!is_string($statusLine)
            || !preg_match('/^HTTP\/\S+ ([1-5][0-9]{2})(?:\s|$)/', $statusLine, $match)
            || (int) $match[1] < 200 || (int) $match[1] >= 300) {
            fclose($stream);
            error_log('[solari-oidc] HTTP ' . $method . ' returned an unsuccessful response');
            if ($softFailure) return '';
            self::fail('SSO provider returned an unsuccessful response.');
            return '';
        }
        $resp = stream_get_contents($stream, self::MAX_HTTP_BODY_BYTES + 1);
        fclose($stream);
        if ($resp === false || strlen($resp) > self::MAX_HTTP_BODY_BYTES) {
            error_log('[solari-oidc] HTTP ' . $method . ' response exceeded the configured size limit');
            if ($softFailure) return '';
            self::fail('SSO provider response exceeded the configured size limit.');
            return '';
        }
        return $resp;
    }

    // --------------------------------------------------------------- JWK -> PEM

    /**
     * Build a PEM SubjectPublicKeyInfo (RSA) from a JWK's base64url n/e, so
     * ext-openssl can verify with it. No phpseclib dependency.
     */
    private static function rsaJwkToPem(string $n64, string $e64): string
    {
        $modulus  = self::derUint(self::b64uDecode($n64));
        $exponent = self::derUint(self::b64uDecode($e64));
        $rsaKey   = self::derSeq($modulus . $exponent);
        // AlgorithmIdentifier { rsaEncryption, NULL } — fixed bytes.
        $algId    = hex2bin('300d06092a864886f70d0101010500');
        $spki     = self::derSeq($algId . self::derBitString($rsaKey));

        return "-----BEGIN PUBLIC KEY-----\n"
            . chunk_split(base64_encode($spki), 64, "\n")
            . "-----END PUBLIC KEY-----\n";
    }

    /** DER length prefix (definite form). */
    private static function derLen(int $len): string
    {
        if ($len < 0x80) {
            return chr($len);
        }
        $bytes = '';
        while ($len > 0) {
            $bytes = chr($len & 0xFF) . $bytes;
            $len >>= 8;
        }
        return chr(0x80 | strlen($bytes)) . $bytes;
    }

    /** DER INTEGER (unsigned): prepend 0x00 if the high bit is set. */
    private static function derUint(string $bytes): string
    {
        $bytes = ltrim($bytes, "\x00");
        if ($bytes === '') {
            $bytes = "\x00";
        }
        if (ord($bytes[0]) & 0x80) {
            $bytes = "\x00" . $bytes;
        }
        return "\x02" . self::derLen(strlen($bytes)) . $bytes;
    }

    /** DER SEQUENCE wrapper. */
    private static function derSeq(string $content): string
    {
        return "\x30" . self::derLen(strlen($content)) . $content;
    }

    /** DER BIT STRING wrapper (0 unused bits). */
    private static function derBitString(string $content): string
    {
        $content = "\x00" . $content;
        return "\x03" . self::derLen(strlen($content)) . $content;
    }

    // ---------------------------------------------------------------- utilities

    /** base64url decode. */
    private static function b64uDecode(string $s): string
    {
        $s = strtr($s, '-_', '+/');
        $pad = strlen($s) % 4;
        if ($pad) {
            $s .= str_repeat('=', 4 - $pad);
        }
        return (string) base64_decode($s, true);
    }

    /** 32 bytes of CSPRNG entropy, URL-safe. */
    private static function randomToken(): string
    {
        return rtrim(strtr(base64_encode(random_bytes(32)), '+/', '-_'), '=');
    }

    /** Issue a 302 redirect and terminate. */
    private static function redirect(string $url): void
    {
        if (!headers_sent()) {
            header('Location: ' . $url, true, 302);
            header('Cache-Control: no-store');
        }
        exit;
    }

    /**
     * Fail an SSO attempt: redirect back to the app root with a short reason in
     * the query string so the login screen can surface it. Fails closed — a
     * failed SSO login never establishes a session. Never returns.
     */
    private static function fail(string $reason): void
    {
        error_log('[solari-oidc] ' . $reason);
        $c    = self::config();
        $base = (string) ($c['postLoginRedirect'] ?: '/');
        $sep  = strpos($base, '?') === false ? '?' : '&';
        self::redirect($base . $sep . 'sso_error=' . rawurlencode($reason));
    }
}
