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
 *      claims to a dashboard operator + role, and pin the principal into the
 *      same server-side session the local login uses (Auth::establishSession).
 *   4. We 302 back to the dashboard; the SPA re-boots authenticated.
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

    // ------------------------------------------------------------------ config

    /** The `oidc` config block, merged over safe defaults. */
    public static function config(): array
    {
        $store = Auth::store();
        $c = (isset($store['oidc']) && is_array($store['oidc'])) ? $store['oidc'] : [];
        return $c + [
            'enabled'            => false,
            'issuer'             => '',
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
        if ($authEp === '') {
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
            || !hash_equals((string) ($tx['state'] ?? ''), $state)) {
            self::fail('Invalid or expired SSO login attempt. Please try again.');
        }

        $claims    = self::exchangeAndVerify($code, (string) $tx['nonce']);
        $principal = self::principalFromClaims($claims);
        Auth::establishSession($principal);

        $c = self::config();
        self::redirect((string) ($c['postLoginRedirect'] ?: '/'));
    }

    /** Exchange the auth code for tokens and return the validated ID-token claims. */
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
        if (!is_array($tok) || empty($tok['id_token'])) {
            error_log('[solari-oidc] token endpoint returned no id_token: ' . substr($resp, 0, 300));
            self::fail('SSO token exchange failed.');
        }

        return self::verifyIdToken((string) $tok['id_token'], $nonce);
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

        // Standard claim checks.
        $c   = self::config();
        $iss = (string) ($claims['iss'] ?? '');
        if (!hash_equals(rtrim((string) $c['issuer'], '/'), rtrim($iss, '/'))) {
            self::fail('ID token issuer mismatch.');
        }
        $aud = $claims['aud'] ?? '';
        $audOk = is_array($aud) ? in_array($c['clientId'], $aud, true) : ($aud === $c['clientId']);
        if (!$audOk) {
            self::fail('ID token audience mismatch.');
        }
        $now = time();
        if (isset($claims['exp']) && $now >= ((int) $claims['exp'] + 60)) {
            self::fail('ID token has expired.');
        }
        if (isset($claims['nbf']) && $now + 60 < (int) $claims['nbf']) {
            self::fail('ID token not yet valid.');
        }
        if ($expectedNonce !== '' && !hash_equals($expectedNonce, (string) ($claims['nonce'] ?? ''))) {
            self::fail('ID token nonce mismatch.');
        }

        return $claims;
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

        $memberships = self::gatherGroups($claims);
        $role = (string) ($c['defaultRole'] ?: 'viewer');
        if (self::intersectsCI($memberships, (array) ($c['operatorGroups'] ?? []))) {
            $role = 'operator';
        }
        if (self::intersectsCI($memberships, (array) ($c['adminGroups'] ?? []))) {
            $role = 'admin';   // admin wins over operator
        }

        return [
            'username'    => $username,
            'role'        => $role,
            'displayName' => $display,
            'source'      => 'oidc',
        ];
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
    private static function discovery(): array
    {
        static $cache = null;
        if ($cache !== null) {
            return $cache;
        }
        $c   = self::config();
        $url = rtrim((string) $c['issuer'], '/') . '/.well-known/openid-configuration';
        $doc = json_decode(self::httpGet($url), true);
        if (!is_array($doc) || empty($doc['authorization_endpoint'])) {
            error_log("[solari-oidc] discovery failed at $url");
            self::fail('Could not reach the SSO provider (discovery).');
        }
        return $cache = $doc;
    }

    /** HTTP GET with TLS verification against the configured CA bundle. */
    private static function httpGet(string $url): string
    {
        return self::http($url, 'GET', null, []);
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
    private static function http(string $url, string $method, ?string $body, array $headers): string
    {
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
                'timeout'       => 10,
                'ignore_errors' => true,   // read body on 4xx/5xx too
            ],
            'ssl' => $ssl,
        ]);

        $resp = @file_get_contents($url, false, $ctx);
        if ($resp === false) {
            $err = error_get_last();
            error_log('[solari-oidc] HTTP ' . $method . ' failed for ' . $url . ': '
                . ($err['message'] ?? 'unknown'));
            self::fail('Could not reach the SSO provider.');
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
