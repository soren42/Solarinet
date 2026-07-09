<?php
declare(strict_types=1);

/**
 * Forgejo — read-only client for the Akoria Forgejo (Gitea-compatible) instance.
 *
 * READ-ONLY by contract: this helper only ever issues GET requests against the
 * Forgejo HTTP API; it never mutates a repository. It is also FAIL-SOFT — a
 * missing token, an unset/unreachable base URL, or a garbage response degrades
 * to { available:false, reason:... } rather than throwing, so the dashboard's
 * Git screen renders an "unavailable" state instead of 500-ing.
 *
 * CONFIG (env only; NEVER hardcode the token):
 *   FORGEJO_URL    base URL, e.g. http://10.1.0.200:3000  (defaults to that host)
 *   FORGEJO_TOKEN  personal access token; sent as "Authorization: token <TOKEN>"
 *
 * The token is read from getenv() and used solely for the Authorization header;
 * it is never echoed back on the wire (the route surfaces only the base URL).
 */
final class Forgejo
{
    /** How many repos to fan out commit/PR fetches over (bounded to stay cheap). */
    private const REPO_FANOUT = 10;

    public static function baseUrl(): string
    {
        $u = getenv('FORGEJO_URL');
        $u = ($u === false || $u === '') ? 'http://10.1.0.200:3000' : $u;
        return rtrim($u, '/');
    }

    private static function token(): ?string
    {
        $t = getenv('FORGEJO_TOKEN');
        return ($t === false || $t === '') ? null : $t;
    }

    public static function available(): bool
    {
        return self::token() !== null;
    }

    /**
     * Assemble the Git screen payload: version, repo list, recent commits across
     * repos, and open PR/issue counts. Every sub-fetch is individually guarded so
     * one dead repo can't sink the whole response.
     *
     * @return array<string,mixed>
     */
    public static function summary(): array
    {
        $base = self::baseUrl();
        if (!self::available()) {
            return ['available' => false, 'reason' => 'FORGEJO_TOKEN is not configured', 'baseUrl' => $base];
        }

        $version = self::get('/api/v1/version');
        if ($version === null) {
            return ['available' => false, 'reason' => 'Forgejo API unreachable at ' . $base, 'baseUrl' => $base];
        }

        $reposRaw = self::get('/api/v1/repos/search?limit=50&order=desc&sort=updated');
        $list     = ($reposRaw !== null && isset($reposRaw['data']) && is_array($reposRaw['data']))
            ? $reposRaw['data']
            : (is_array($reposRaw) ? $reposRaw : []);

        $repos      = [];
        $commits    = [];
        $prs        = [];
        $openIssues = 0;
        $openPrs    = 0;

        $i = 0;
        foreach ($list as $r) {
            if (!is_array($r)) {
                continue;
            }
            $owner = (string) (($r['owner']['login'] ?? $r['owner']['username'] ?? '') ?: '');
            $name  = (string) ($r['name'] ?? '');
            $full  = (string) ($r['full_name'] ?? ($owner !== '' ? "$owner/$name" : $name));

            $issueCount = (int) ($r['open_issues_count'] ?? 0);
            $openIssues += $issueCount;

            $repos[] = [
                'name'        => $name,
                'fullName'    => $full,
                'owner'       => $owner,
                'description' => ($r['description'] ?? '') !== '' ? (string) $r['description'] : null,
                'private'     => (bool) ($r['private'] ?? false),
                'fork'        => (bool) ($r['fork'] ?? false),
                'stars'       => (int) ($r['stars_count'] ?? 0),
                'forks'       => (int) ($r['forks_count'] ?? 0),
                'openIssues'  => $issueCount,
                'defaultBranch' => ($r['default_branch'] ?? null) ?: null,
                'updatedAt'   => self::iso($r['updated_at'] ?? null),
                'htmlUrl'     => ($r['html_url'] ?? null) ?: null,
            ];

            // Fan out commit + PR reads over the most-recently-updated repos only.
            if ($i < self::REPO_FANOUT && $owner !== '' && $name !== '') {
                $enc = rawurlencode($owner) . '/' . rawurlencode($name);

                $cRaw = self::get("/api/v1/repos/$enc/commits?limit=5&stat=false&verification=false");
                if (is_array($cRaw)) {
                    foreach ($cRaw as $c) {
                        if (!is_array($c)) {
                            continue;
                        }
                        $cm  = $c['commit'] ?? [];
                        $msg = (string) ($cm['message'] ?? '');
                        $commits[] = [
                            'repo'       => $full,
                            'sha'        => substr((string) ($c['sha'] ?? ''), 0, 10),
                            'message'    => trim(strtok($msg, "\n") ?: $msg),
                            'author'     => (string) (($cm['author']['name'] ?? $c['author']['login'] ?? '') ?: 'unknown'),
                            'date'       => self::iso($cm['author']['date'] ?? $cm['committer']['date'] ?? null),
                            'htmlUrl'    => ($c['html_url'] ?? null) ?: null,
                        ];
                    }
                }

                $pRaw = self::get("/api/v1/repos/$enc/issues?state=open&type=pulls&limit=50");
                if (is_array($pRaw)) {
                    foreach ($pRaw as $pr) {
                        if (!is_array($pr)) {
                            continue;
                        }
                        $openPrs++;
                        $prs[] = [
                            'repo'      => $full,
                            'number'    => (int) ($pr['number'] ?? 0),
                            'title'     => (string) ($pr['title'] ?? ''),
                            'author'    => (string) (($pr['user']['login'] ?? '') ?: 'unknown'),
                            'createdAt' => self::iso($pr['created_at'] ?? null),
                            'htmlUrl'   => ($pr['html_url'] ?? null) ?: null,
                        ];
                    }
                }
            }
            $i++;
        }

        // Newest commits first, then bound the merged cross-repo activity list.
        usort($commits, static function (array $a, array $b): int {
            return strcmp((string) ($b['date'] ?? ''), (string) ($a['date'] ?? ''));
        });
        $commits = array_slice($commits, 0, 20);

        usort($prs, static function (array $a, array $b): int {
            return strcmp((string) ($b['createdAt'] ?? ''), (string) ($a['createdAt'] ?? ''));
        });

        return [
            'available' => true,
            'baseUrl'   => $base,
            'version'   => (string) ($version['version'] ?? '—'),
            'counts'    => [
                'repos'      => count($repos),
                'openIssues' => $openIssues,
                'openPrs'    => $openPrs,
            ],
            'repos'     => $repos,
            'commits'   => $commits,
            'prs'       => $prs,
        ];
    }

    /**
     * GET a Forgejo API path and JSON-decode it. Returns the decoded structure,
     * or null on any transport/HTTP/decode failure (fail-soft — callers guard).
     *
     * @return array<mixed>|null
     */
    private static function get(string $path)
    {
        $url   = self::baseUrl() . $path;
        $token = self::token();
        $hdrs  = [
            'Accept: application/json',
            'User-Agent: SolariNet-Dashboard',
        ];
        if ($token !== null) {
            $hdrs[] = 'Authorization: token ' . $token;
        }

        $body = self::http($url, $hdrs);
        if ($body === null) {
            return null;
        }
        $decoded = json_decode($body, true);
        return is_array($decoded) ? $decoded : null;
    }

    /**
     * Minimal HTTP GET with a short timeout. Prefers cURL when the extension is
     * loaded, else falls back to a stream-context file_get_contents so the helper
     * works regardless of the PHP build. Returns the body string or null.
     */
    private static function http(string $url, array $headers): ?string
    {
        if (function_exists('curl_init')) {
            $ch = curl_init($url);
            curl_setopt_array($ch, [
                CURLOPT_RETURNTRANSFER => true,
                CURLOPT_HTTPHEADER     => $headers,
                CURLOPT_CONNECTTIMEOUT => 3,
                CURLOPT_TIMEOUT        => 6,
                CURLOPT_FOLLOWLOCATION => true,
                CURLOPT_MAXREDIRS      => 2,
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
                'header'        => implode("\r\n", $headers),
                'timeout'       => 6,
                'ignore_errors' => true,
            ],
        ]);
        $body = @file_get_contents($url, false, $ctx);
        if ($body === false) {
            return null;
        }
        // Status line from the response headers (PHP 8.5+ API). On older builds
        // the function is absent and we simply skip the status gate — a non-2xx
        // body then fails json_decode and the caller fails soft. We deliberately
        // never reference the predefined $http_response_header (deprecated on 8.5,
        // and merely naming it here would trip that deprecation).
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

    /** ISO-8601-ish passthrough: Forgejo already emits RFC3339; normalise null/empty. */
    private static function iso($v): ?string
    {
        if (!is_string($v) || $v === '' || strpos($v, '0001-01-01') === 0) {
            return null;
        }
        return $v;
    }
}
