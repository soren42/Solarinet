<?php
declare(strict_types=1);

/**
 * Ca — read-only view of the Akoria PKI for the dashboard's Certificates screen.
 *
 * Pulls three independent things and folds them into one payload:
 *   1. step-ca health + root/intermediate certificates from the internal CA on
 *      chlorine (https://10.7.0.10). The CA is internal-only with a private root,
 *      so TLS verification is deliberately DISABLED for these two calls (we are
 *      not authenticating a public endpoint, only reading its published roots).
 *   2. The SolariNet mTLS CA certificate shipped in the repo (run/pki/ca.pem),
 *      parsed with openssl_x509_parse() for its own expiry.
 *   3. The per-node mTLS cert inventory from the solarinet DB `node` table.
 *
 * FAIL-SOFT: an unreachable step-ca, a missing ca.pem, or an unparseable cert
 * degrades that section (available:false / an empty list), never a 500. PHP only
 * ever READS cert material here — it never signs, issues, or writes anything
 * (§11.1: cert/CA operations belong to step-ca + solariCtl, not the web tier).
 *
 * CONFIG (env, with sane internal defaults):
 *   STEPCA_URL      step-ca base URL (default https://10.7.0.10)
 *   SOLARINET_CA_PEM  path to the mTLS CA pem (default <repo>/run/pki/ca.pem)
 */
final class Ca
{
    /** Expiry warning threshold: a cert inside this window is flagged (amber). */
    private const WARN_DAYS = 60;

    /**
     * The Akoria step-ca provisioner set is a known, static configuration —
     * surfaced here rather than scraped so the screen documents intent even when
     * the CA is unreachable.
     *
     * @var array<int,array{name:string,type:string}>
     */
    private const PROVISIONERS = [
        ['name' => 'admin@akoria.net', 'type' => 'JWK'],
        ['name' => 'acme',             'type' => 'ACME'],
        ['name' => 'sshpop',           'type' => 'SSHPOP'],
        ['name' => 'smime',            'type' => 'S/MIME'],
    ];

    public static function stepCaUrl(): string
    {
        $u = getenv('STEPCA_URL');
        $u = ($u === false || $u === '') ? 'https://10.7.0.10' : $u;
        return rtrim($u, '/');
    }

    private static function caPemPath(): string
    {
        $p = getenv('SOLARINET_CA_PEM');
        if (is_string($p) && $p !== '') {
            return $p;
        }
        // dashboard/api/lib/Ca.php -> repo root is three levels up.
        return dirname(__DIR__, 3) . '/run/pki/ca.pem';
    }

    /**
     * Full Certificates-screen payload.
     *
     * @return array<string,mixed>
     */
    public static function summary(): array
    {
        [$health, $roots] = self::stepCa();
        $roots = array_merge($roots, self::localCa());

        return [
            'stepCa'       => $health,
            'roots'        => $roots,
            'provisioners' => self::PROVISIONERS,
            'nodeCerts'    => self::nodeCerts(),
            'warnDays'     => self::WARN_DAYS,
        ];
    }

    /**
     * Query step-ca /health and /roots. Returns [healthDescriptor, rootCertList].
     * Both sub-calls are guarded; an unreachable CA yields available:false and an
     * empty root list.
     *
     * @return array{0:array<string,mixed>,1:array<int,array<string,mixed>>}
     */
    private static function stepCa(): array
    {
        $url = self::stepCaUrl();

        $healthBody = self::http($url . '/health');
        if ($healthBody === null) {
            return [
                ['url' => $url, 'available' => false, 'status' => null, 'reason' => 'step-ca unreachable at ' . $url],
                [],
            ];
        }
        $healthJson = json_decode($healthBody, true);
        $status     = is_array($healthJson) ? (string) ($healthJson['status'] ?? '') : trim($healthBody);
        $health     = [
            'url'       => $url,
            'available' => true,
            'status'    => $status !== '' ? $status : 'unknown',
            'reason'    => null,
        ];

        $roots    = [];
        $rootsRaw = self::http($url . '/roots');
        if ($rootsRaw !== null) {
            foreach (self::extractPems($rootsRaw) as $pem) {
                $cert = self::describeCert($pem, 'step-ca', 'root');
                if ($cert !== null) {
                    $roots[] = $cert;
                }
            }
        }

        return [$health, $roots];
    }

    /**
     * Parse the repo-local SolariNet mTLS CA cert. Returns a 0- or 1-element list
     * (empty if the pem is missing/unreadable/unparseable).
     *
     * @return array<int,array<string,mixed>>
     */
    private static function localCa(): array
    {
        $path = self::caPemPath();
        if (!is_file($path) || !is_readable($path)) {
            return [];
        }
        $pem = @file_get_contents($path);
        if ($pem === false || $pem === '') {
            return [];
        }
        $cert = self::describeCert($pem, 'solarinet-mtls', 'ca');
        return $cert !== null ? [$cert] : [];
    }

    /**
     * openssl_x509_parse() a single PEM into the screen's cert descriptor, or
     * null if it does not parse. daysLeft/severity drive the green/amber/red pill.
     *
     * @return array<string,mixed>|null
     */
    private static function describeCert(string $pem, string $source, string $kind): ?array
    {
        $info = @openssl_x509_parse($pem);
        if (!is_array($info)) {
            return null;
        }
        $subjectCn = self::dnField($info, 'subject', 'CN');
        $issuerCn  = self::dnField($info, 'issuer', 'CN');
        $notAfter  = isset($info['validTo_time_t']) ? (int) $info['validTo_time_t'] : null;
        $notBefore = isset($info['validFrom_time_t']) ? (int) $info['validFrom_time_t'] : null;

        $daysLeft = null;
        if ($notAfter !== null) {
            $daysLeft = (int) floor(($notAfter - time()) / 86400);
        }

        return [
            'name'       => $subjectCn ?? ($info['name'] ?? 'certificate'),
            'source'     => $source,          // 'step-ca' | 'solarinet-mtls'
            'kind'       => $kind,            // 'root' | 'intermediate' | 'ca'
            'subject'    => $subjectCn,
            'issuer'     => $issuerCn,
            'selfSigned' => $subjectCn !== null && $subjectCn === $issuerCn,
            'notBefore'  => $notBefore !== null ? gmdate('Y-m-d\TH:i:s\Z', $notBefore) : null,
            'notAfter'   => $notAfter !== null ? gmdate('Y-m-d\TH:i:s\Z', $notAfter) : null,
            'daysLeft'   => $daysLeft,
            'severity'   => self::severity($daysLeft),
            'serial'     => isset($info['serialNumberHex']) ? (string) $info['serialNumberHex'] : null,
        ];
    }

    /** green (ok) / amber (expiring < WARN_DAYS) / red (expired or unknown-none). */
    private static function severity(?int $daysLeft): string
    {
        if ($daysLeft === null) {
            return 'unknown';
        }
        if ($daysLeft < 0) {
            return 'expired';
        }
        if ($daysLeft < self::WARN_DAYS) {
            return 'expiring';
        }
        return 'ok';
    }

    /** Pull a component (e.g. CN) out of an openssl subject/issuer array. */
    private static function dnField(array $info, string $which, string $key): ?string
    {
        $dn = $info[$which] ?? null;
        if (!is_array($dn) || !isset($dn[$key])) {
            return null;
        }
        $v = $dn[$key];
        // A DN component can repeat (array); take the first for display.
        if (is_array($v)) {
            $v = $v[0] ?? null;
        }
        return ($v === null || $v === '') ? null : (string) $v;
    }

    /**
     * Extract every PEM certificate block from a response body. step-ca's /roots
     * may return raw PEM, or a JSON envelope ({"crts":[...]} or {"crt":"..."});
     * this handles all three by scanning for BEGIN CERTIFICATE blocks in whatever
     * text we end up with.
     *
     * @return array<int,string>
     */
    private static function extractPems(string $raw): array
    {
        $text = $raw;
        $json = json_decode($raw, true);
        if (is_array($json)) {
            $parts = [];
            foreach (['crts', 'certificates', 'roots'] as $listKey) {
                if (isset($json[$listKey]) && is_array($json[$listKey])) {
                    foreach ($json[$listKey] as $c) {
                        if (is_string($c)) {
                            $parts[] = $c;
                        }
                    }
                }
            }
            foreach (['crt', 'ca', 'certificate'] as $oneKey) {
                if (isset($json[$oneKey]) && is_string($json[$oneKey])) {
                    $parts[] = $json[$oneKey];
                }
            }
            if ($parts !== []) {
                $text = implode("\n", $parts);
            }
        }

        if (preg_match_all(
            '/-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----/s',
            $text,
            $m
        )) {
            return $m[0];
        }
        return [];
    }

    /**
     * The fleet's per-node mTLS cert inventory from the solarinet DB. Read-only
     * SELECT; an empty/absent table degrades to [].
     *
     * @return array<int,array<string,mixed>>
     */
    private static function nodeCerts(): array
    {
        try {
            $rows = Db::rows(
                'SELECT hostFqdn, certCn, role, arch, enrolledAt, lastSeenAt
                   FROM node ORDER BY hostFqdn'
            );
        } catch (\Throwable $e) {
            error_log('Ca::nodeCerts read failed: ' . $e->getMessage());
            return [];
        }
        return array_map(static function (array $r): array {
            return [
                'hostFqdn'   => $r['hostFqdn'],
                'certCn'     => $r['certCn'],
                'role'       => $r['role'],
                'arch'       => $r['arch'],
                'enrolledAt' => Coerce::iso($r['enrolledAt'] ?? null),
                'lastSeenAt' => Coerce::iso($r['lastSeenAt'] ?? null),
            ];
        }, $rows);
    }

    /**
     * HTTP GET with TLS verification DISABLED (internal CA, private root) and a
     * short timeout. cURL when available, else a stream-context fallback. Returns
     * the body or null on any failure.
     */
    private static function http(string $url): ?string
    {
        if (function_exists('curl_init')) {
            $ch = curl_init($url);
            curl_setopt_array($ch, [
                CURLOPT_RETURNTRANSFER => true,
                CURLOPT_CONNECTTIMEOUT => 3,
                CURLOPT_TIMEOUT        => 6,
                CURLOPT_SSL_VERIFYPEER => false,
                CURLOPT_SSL_VERIFYHOST => 0,
                CURLOPT_HTTPHEADER     => ['Accept: application/json', 'User-Agent: SolariNet-Dashboard'],
            ]);
            $body = curl_exec($ch);
            $code = (int) curl_getinfo($ch, CURLINFO_HTTP_CODE);
            curl_close($ch);
            if ($body === false || $code < 200 || $code >= 300) {
                return null;
            }
            return (string) $body;
        }

        $ctx  = stream_context_create([
            'http' => [
                'method'        => 'GET',
                'header'        => "Accept: application/json\r\nUser-Agent: SolariNet-Dashboard",
                'timeout'       => 6,
                'ignore_errors' => true,
            ],
            'ssl'  => [
                'verify_peer'      => false,
                'verify_peer_name' => false,
            ],
        ]);
        $body = @file_get_contents($url, false, $ctx);
        if ($body === false) {
            return null;
        }
        // Status line from the response headers (PHP 8.5+ API). On older builds
        // the function is absent and we skip the status gate — a non-2xx body then
        // fails to decode/parse and the caller fails soft. We deliberately never
        // reference the predefined $http_response_header (deprecated on 8.5, and
        // merely naming it here would trip that deprecation).
        $hdrs = function_exists('http_get_last_response_headers') ? http_get_last_response_headers() : null;
        $status = 0;
        if (is_array($hdrs) && isset($hdrs[0]) && preg_match('#\s(\d{3})\s#', (string) $hdrs[0], $m)) {
            $status = (int) $m[1];
        }
        if ($status !== 0 && ($status < 200 || $status >= 300)) {
            return null;
        }
        return (string) $body;
    }
}
