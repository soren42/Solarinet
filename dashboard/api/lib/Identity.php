<?php
declare(strict_types=1);

/**
 * Identity — read-only Keycloak client for the dashboard's Identity screen.
 *
 * Surfaces the `akoria` realm's users (username/email/enabled/emailVerified),
 * their enrolled credential factors (password / otp / webauthn / passwordless
 * webauthn), and their group memberships — a friendly operator view over
 * Keycloak, never a place to create/update/delete anything (§CLAUDE.md: this
 * pass is additive + read-only, no write/CRUD actions).
 *
 * AUTH: client_credentials grant against KC_TOKEN_URL using the confidential
 * service account KC_SVC_CLIENT/KC_SVC_SECRET (a realm-management-scoped
 * client provisioned out of band). The access token is cached in-process for
 * its lifetime (minus a safety margin) so a burst of requests within one
 * php-fpm worker's lifetime doesn't re-authenticate every time.
 *
 * TLS: Keycloak's certificate is trusted in xenon's system store, so every
 * call here uses NORMAL certificate verification — CURLOPT_SSL_VERIFYPEER is
 * left at its secure default (true). Do not disable it (see Ca.php's
 * step-ca client for the one place that's legitimately different, and why).
 *
 * CONFIG (php-fpm pool env; no secrets are ever hardcoded here):
 *   KC_TOKEN_URL   e.g. https://sso.akoria.net/realms/akoria/protocol/openid-connect/token
 *   KC_ADMIN_URL   e.g. https://sso.akoria.net/admin/realms/akoria
 *   KC_SVC_CLIENT  confidential client id for the service account
 *   KC_SVC_SECRET  that client's secret
 *
 * FAIL-SOFT: any missing config or unreachable/erroring Keycloak degrades to
 * {available:false, reason:...} — never a 500, never a partial half-shaped
 * payload the screen would have to guess about.
 */
final class Identity
{
    /** Access-token cache: shared across requests within one php-fpm worker. */
    private static ?string $token = null;
    private static int $tokenExpiresAt = 0;

    /** Credential `type` values Keycloak reports, mapped to the screen's factor keys. */
    private const FACTOR_MAP = [
        'password'                => 'password',
        'otp'                     => 'otp',
        'webauthn'                => 'webauthn',
        'webauthn-passwordless'   => 'webauthnPasswordless',
    ];

    public static function available(): bool
    {
        return self::tokenUrl() !== null
            && self::adminUrl() !== null
            && self::clientId() !== null
            && self::clientSecret() !== null;
    }

    private static function tokenUrl(): ?string
    {
        $v = getenv('KC_TOKEN_URL');
        return ($v === false || $v === '') ? null : $v;
    }

    private static function adminUrl(): ?string
    {
        $v = getenv('KC_ADMIN_URL');
        return ($v === false || $v === '') ? null : rtrim($v, '/');
    }

    private static function clientId(): ?string
    {
        $v = getenv('KC_SVC_CLIENT');
        return ($v === false || $v === '') ? null : $v;
    }

    private static function clientSecret(): ?string
    {
        $v = getenv('KC_SVC_SECRET');
        return ($v === false || $v === '') ? null : $v;
    }

    /**
     * Full Identity-screen payload: the realm's users with their enrolled MFA
     * factors and group memberships.
     *
     * @return array<string,mixed>
     */
    public static function users(): array
    {
        if (!self::available()) {
            return [
                'available' => false,
                'reason'    => 'Keycloak is not configured (KC_TOKEN_URL/KC_ADMIN_URL/KC_SVC_CLIENT/KC_SVC_SECRET).',
                'users'     => [],
            ];
        }

        $token = self::getToken();
        if ($token === null) {
            return [
                'available' => false,
                'reason'    => 'Could not obtain a Keycloak service-account token.',
                'users'     => [],
            ];
        }

        $raw = self::get('/users?max=200', $token);
        if (!is_array($raw)) {
            return [
                'available' => false,
                'reason'    => 'Keycloak admin API unreachable at ' . self::adminUrl(),
                'users'     => [],
            ];
        }

        $users = [];
        foreach ($raw as $u) {
            if (!is_array($u) || !isset($u['id'])) {
                continue;
            }
            $id = (string) $u['id'];

            $creds  = self::get('/users/' . rawurlencode($id) . '/credentials', $token);
            $factors = self::factorsFromCredentials(is_array($creds) ? $creds : []);

            $groupsRaw = self::get('/users/' . rawurlencode($id) . '/groups', $token);
            $groups = [];
            if (is_array($groupsRaw)) {
                foreach ($groupsRaw as $g) {
                    if (is_array($g) && isset($g['name'])) {
                        $groups[] = (string) $g['name'];
                    }
                }
            }

            $createdMs = $u['createdTimestamp'] ?? null;
            $createdIso = is_numeric($createdMs)
                ? gmdate('Y-m-d\TH:i:s\Z', (int) ((int) $createdMs / 1000))
                : null;

            $users[] = [
                'id'            => $id,
                'username'      => (string) ($u['username'] ?? ''),
                'email'         => ($u['email'] ?? null) !== null ? (string) $u['email'] : null,
                'enabled'       => (bool) ($u['enabled'] ?? false),
                'emailVerified' => (bool) ($u['emailVerified'] ?? false),
                'createdAt'     => $createdIso,
                'factors'       => $factors,
                'groups'        => $groups,
            ];
        }

        usort($users, static function (array $a, array $b): int {
            return strcasecmp((string) $a['username'], (string) $b['username']);
        });

        return [
            'available' => true,
            'reason'    => null,
            'realmUrl'  => self::adminUrl(),
            'users'     => $users,
        ];
    }

    /**
     * Turn a /users/{id}/credentials list into the screen's factor flags plus
     * the raw enrolled-type list (for anything not in FACTOR_MAP, kept around
     * for completeness rather than silently dropped).
     *
     * @param array<int,mixed> $credentials
     * @return array<string,mixed>
     */
    private static function factorsFromCredentials(array $credentials): array
    {
        $factors = [
            'password'             => false,
            'otp'                  => false,
            'webauthn'              => false,
            'webauthnPasswordless' => false,
            'types'                => [],
        ];
        foreach ($credentials as $c) {
            if (!is_array($c) || !isset($c['type'])) {
                continue;
            }
            $type = (string) $c['type'];
            $factors['types'][] = $type;
            if (isset(self::FACTOR_MAP[$type])) {
                $factors[self::FACTOR_MAP[$type]] = true;
            }
        }
        $factors['types'] = array_values(array_unique($factors['types']));
        return $factors;
    }

    /**
     * client_credentials token fetch, cached for its lifetime (minus a 30s
     * safety margin). Returns null on any failure (fail-soft).
     */
    private static function getToken(): ?string
    {
        if (self::$token !== null && time() < self::$tokenExpiresAt) {
            return self::$token;
        }

        $url = self::tokenUrl();
        if ($url === null) {
            return null;
        }

        $body = http_build_query([
            'grant_type'    => 'client_credentials',
            'client_id'     => self::clientId(),
            'client_secret' => self::clientSecret(),
        ]);

        $resp = self::http($url, [
            'Content-Type: application/x-www-form-urlencoded',
            'Accept: application/json',
        ], 'POST', $body);

        if ($resp === null) {
            return null;
        }
        $json = json_decode($resp, true);
        if (!is_array($json) || !isset($json['access_token'])) {
            return null;
        }

        self::$token = (string) $json['access_token'];
        $ttl = isset($json['expires_in']) && is_numeric($json['expires_in']) ? (int) $json['expires_in'] : 60;
        self::$tokenExpiresAt = time() + max(5, $ttl - 30);

        return self::$token;
    }

    /**
     * GET an admin-API path (relative to KC_ADMIN_URL) with the bearer token.
     * Returns the decoded JSON body, or null on any transport/HTTP/decode
     * failure.
     *
     * @return array<mixed>|null
     */
    private static function get(string $path, string $token)
    {
        $url = self::adminUrl() . $path;
        $body = self::http($url, [
            'Accept: application/json',
            'Authorization: Bearer ' . $token,
        ]);
        if ($body === null) {
            return null;
        }
        $decoded = json_decode($body, true);
        return is_array($decoded) ? $decoded : null;
    }

    /**
     * Minimal HTTP request with a short timeout and NORMAL TLS verification
     * (Keycloak's cert is trusted system-wide — never disable verify here).
     * Prefers cURL; falls back to a stream-context file_get_contents. Returns
     * the response body, or null on any failure (including non-2xx status).
     */
    private static function http(string $url, array $headers, string $method = 'GET', ?string $body = null): ?string
    {
        if (function_exists('curl_init')) {
            $ch = curl_init($url);
            $opts = [
                CURLOPT_RETURNTRANSFER => true,
                CURLOPT_CONNECTTIMEOUT => 3,
                CURLOPT_TIMEOUT        => 6,
                CURLOPT_HTTPHEADER     => $headers,
                // TLS verification left at cURL's secure defaults — deliberately
                // NOT disabled (Keycloak's cert is trusted in xenon's store).
            ];
            if ($method === 'POST') {
                $opts[CURLOPT_POST]       = true;
                $opts[CURLOPT_POSTFIELDS] = $body ?? '';
            }
            curl_setopt_array($ch, $opts);
            $resp = curl_exec($ch);
            $code = (int) curl_getinfo($ch, CURLINFO_HTTP_CODE);
            curl_close($ch);
            if ($resp === false || $code < 200 || $code >= 300) {
                return null;
            }
            return (string) $resp;
        }

        $ctx = stream_context_create([
            'http' => [
                'method'        => $method,
                'header'        => implode("\r\n", $headers),
                'content'       => $method === 'POST' ? ($body ?? '') : null,
                'timeout'       => 6,
                'ignore_errors' => true,
            ],
            // No 'ssl' block here at all: PHP's stream wrapper defaults to
            // verify_peer/verify_peer_name = true, which is exactly what we want.
        ]);
        $resp = @file_get_contents($url, false, $ctx);
        if ($resp === false) {
            return null;
        }
        $hdrs = function_exists('http_get_last_response_headers') ? http_get_last_response_headers() : null;
        $status = 0;
        if (is_array($hdrs) && isset($hdrs[0]) && preg_match('#\s(\d{3})\s#', (string) $hdrs[0], $m)) {
            $status = (int) $m[1];
        }
        if ($status !== 0 && ($status < 200 || $status >= 300)) {
            return null;
        }
        return (string) $resp;
    }
}
