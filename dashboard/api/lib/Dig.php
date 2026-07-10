<?php
declare(strict_types=1);

/**
 * Dig — runs the system `dig` binary against a fixed, server-side resolver IP
 * and parses its output into a stable wire shape. Never throws; every failure
 * mode (bad exec, non-zero exit, wall-clock timeout, unparseable output)
 * degrades to a TIMEOUT-shaped result so callers can always emit a clean
 * envelope.
 *
 * SECURITY: callers must NEVER pass user-controlled input as $serverIp — the
 * IP must always come from the route module's server-side whitelist
 * (DnsRoutes::servers()), never from $_GET. $name/$type are still shell-quoted
 * via escapeshellarg() as defence in depth even though route-level validation
 * has already restricted their charset before Dig ever sees them.
 */
final class Dig
{
    /** Wall-clock floor for a Dig::many() batch, regardless of $timeoutSec. */
    private const MIN_BATCH_WALL_CLOCK = 3.0;

    /** Poll tick for stream_select() while a batch is in flight. */
    private const SELECT_TICK_USEC = 200000; // 0.2s

    /**
     * Run a single dig query. Convenience wrapper over many().
     *
     * @return array{status:string,flags:array{aa:bool,ra:bool},answers:array,authority:array,queryTimeMs:?int,error:?string}
     */
    public static function query(string $serverIp, string $name, string $type, float $timeoutSec = 2.0): array
    {
        $r = self::many(['q' => ['ip' => $serverIp, 'name' => $name, 'type' => $type]], $timeoutSec);
        return $r['q'];
    }

    /**
     * Concurrent fan-out. $jobs: [jobKey => ['ip'=>..., 'name'=>..., 'type'=>...]].
     * Returns [jobKey => <same shape as query()>], one entry per input job, in
     * the same key set (never fewer keys than requested).
     *
     * @param array<string,array{ip:string,name:string,type:string}> $jobs
     * @return array<string,array{status:string,flags:array{aa:bool,ra:bool},answers:array,authority:array,queryTimeMs:?int,error:?string}>
     */
    public static function many(array $jobs, float $timeoutSec = 2.0): array
    {
        if ($jobs === []) {
            return [];
        }

        $procs = [];
        foreach ($jobs as $key => $job) {
            $descriptors = [
                1 => ['pipe', 'w'],
                2 => ['pipe', 'w'],
            ];
            $cmd  = self::buildCmd($job['ip'], $job['name'], $job['type']);
            $proc = @proc_open($cmd, $descriptors, $pipes);
            if (!is_resource($proc)) {
                $procs[$key] = ['proc' => null, 'pipes' => null, 'out' => '', 'done' => true, 'failed' => true];
                continue;
            }
            stream_set_blocking($pipes[1], false);
            stream_set_blocking($pipes[2], false);
            $procs[$key] = ['proc' => $proc, 'pipes' => $pipes, 'out' => '', 'done' => false, 'failed' => false];
        }

        $deadline = microtime(true) + max($timeoutSec + 1.0, self::MIN_BATCH_WALL_CLOCK);

        while (true) {
            $pendingKeys = [];
            foreach ($procs as $key => $p) {
                if (!$p['done']) {
                    $pendingKeys[] = $key;
                }
            }
            if ($pendingKeys === []) {
                break;
            }
            if (microtime(true) >= $deadline) {
                break;
            }

            $read = [];
            $map  = [];
            foreach ($pendingKeys as $key) {
                $out = $procs[$key]['pipes'][1];
                $err = $procs[$key]['pipes'][2];
                $read[] = $out;
                $map[(int) $out] = [$key, 1];
                $read[] = $err;
                $map[(int) $err] = [$key, 2];
            }

            $write  = null;
            $except = null;
            $n      = @stream_select($read, $write, $except, 0, self::SELECT_TICK_USEC);
            if ($n === false) {
                break;
            }
            if ($n > 0) {
                foreach ($read as $stream) {
                    $id = (int) $stream;
                    if (!isset($map[$id])) {
                        continue;
                    }
                    [$key, $fd] = $map[$id];
                    $chunk = @fread($stream, 65536);
                    if (is_string($chunk) && $chunk !== '' && $fd === 1) {
                        $procs[$key]['out'] .= $chunk;
                    }
                }
            }

            foreach ($pendingKeys as $key) {
                $status = proc_get_status($procs[$key]['proc']);
                if ($status !== false && !$status['running']) {
                    $procs[$key]['out'] .= (string) @stream_get_contents($procs[$key]['pipes'][1]);
                    @fclose($procs[$key]['pipes'][1]);
                    @fclose($procs[$key]['pipes'][2]);
                    @proc_close($procs[$key]['proc']);
                    $procs[$key]['done'] = true;
                }
            }
        }

        // Anything still running at the deadline is a timeout — kill it.
        foreach ($procs as $key => $p) {
            if (!$p['done']) {
                if (is_resource($p['proc'])) {
                    @proc_terminate($p['proc'], 9);
                    @fclose($p['pipes'][1]);
                    @fclose($p['pipes'][2]);
                    @proc_close($p['proc']);
                }
                $procs[$key]['done'] = true;
            }
        }

        $results = [];
        foreach ($jobs as $key => $job) {
            $p = $procs[$key] ?? null;
            if ($p === null || !empty($p['failed']) || trim((string) ($p['out'] ?? '')) === '') {
                $results[$key] = self::timeoutResult('dig timed out, failed to start, or produced no output');
                continue;
            }
            $results[$key] = self::parseOutput($p['out']);
        }
        return $results;
    }

    /**
     * RPZ detection: true when any answer/authority row's owner name is the
     * rpz.akoria SOA (leaks into AUTHORITY on rewrite/NXDOMAIN policy) OR the
     * answer is a CNAME to the root (StevenBlack-style walled-garden rule).
     */
    public static function isRpzBlocked(array $result): bool
    {
        // BIND RPZ "CNAME ." rewrites to NXDOMAIN *during recursion*: the reply
        // has no 'aa' flag and (unlike a genuine authoritative NXDOMAIN) carries
        // no SOA in the authority section. That pair is the reliable signal for
        // our ad-block setup — the queried name is returned verbatim, never
        // "rpz.akoria", so the old name-match heuristic never fired.
        $status    = strtoupper((string) ($result['status'] ?? ''));
        $aa        = (bool) ($result['flags']['aa'] ?? false);
        $authority = $result['authority'] ?? [];
        if ($status === 'NXDOMAIN' && !$aa && count($authority) === 0) {
            return true;
        }
        // Fallback: RPZ deployed as an explicit CNAME-to-"." rewrite instead.
        foreach (array_merge($result['answers'] ?? [], $authority) as $r) {
            if (strtoupper((string) ($r['type'] ?? '')) === 'CNAME' && trim((string) ($r['data'] ?? '')) === '.') {
                return true;
            }
        }
        return false;
    }

    /** Build the fully shell-quoted dig command string. */
    private static function buildCmd(string $ip, string $name, string $type): string
    {
        $argv = [
            'dig',
            '@' . $ip,
            $name,
            $type,
            '+time=1',
            '+tries=1',
            '+noall',
            '+comments',
            '+answer',
            '+authority',
            '+stats',
        ];
        return implode(' ', array_map('escapeshellarg', $argv));
    }

    /**
     * @return array{status:string,flags:array{aa:bool,ra:bool},answers:array,authority:array,queryTimeMs:?int,error:?string}
     */
    private static function parseOutput(string $output): array
    {
        $lines       = preg_split('/\r?\n/', $output) ?: [];
        $status      = null;
        $aa          = false;
        $ra          = false;
        $queryTimeMs = null;
        $answers     = [];
        $authority   = [];
        $section     = null;

        foreach ($lines as $line) {
            if (preg_match('/;;\s*->>HEADER<<-.*status:\s*([A-Za-z]+)/', $line, $m) === 1) {
                $status = strtoupper($m[1]);
                continue;
            }
            if (preg_match('/^;;\s*flags:\s*([^;]*);/', $line, $m) === 1) {
                $flags = preg_split('/\s+/', trim($m[1])) ?: [];
                $aa    = in_array('aa', $flags, true);
                $ra    = in_array('ra', $flags, true);
                continue;
            }
            if (preg_match('/;;\s*Query time:\s*(\d+)\s*msec/', $line, $m) === 1) {
                $queryTimeMs = (int) $m[1];
                continue;
            }
            if (stripos($line, ';; ANSWER SECTION:') !== false) {
                $section = 'answer';
                continue;
            }
            if (stripos($line, ';; AUTHORITY SECTION:') !== false) {
                $section = 'authority';
                continue;
            }
            if (trim($line) === '') {
                $section = null;
                continue;
            }
            if (strpos(ltrim($line), ';') === 0) {
                continue;
            }
            if ($section === 'answer' || $section === 'authority') {
                $parts = preg_split('/\s+/', trim($line), 5);
                if ($parts !== false && count($parts) >= 5) {
                    $rec = [
                        'name' => $parts[0],
                        'ttl'  => (int) $parts[1],
                        'type' => $parts[3],
                        'data' => $parts[4],
                    ];
                    if ($section === 'answer') {
                        $answers[] = $rec;
                    } else {
                        $authority[] = $rec;
                    }
                }
            }
        }

        if ($status === null) {
            return self::timeoutResult('no dig header parsed (process failed or timed out)');
        }

        return [
            'status'      => $status,
            'flags'       => ['aa' => $aa, 'ra' => $ra],
            'answers'     => $answers,
            'authority'   => $authority,
            'queryTimeMs' => $queryTimeMs,
            'error'       => null,
        ];
    }

    /**
     * @return array{status:string,flags:array{aa:bool,ra:bool},answers:array,authority:array,queryTimeMs:?int,error:?string}
     */
    private static function timeoutResult(string $reason): array
    {
        return [
            'status'      => 'TIMEOUT',
            'flags'       => ['aa' => false, 'ra' => false],
            'answers'     => [],
            'authority'   => [],
            'queryTimeMs' => null,
            'error'       => $reason,
        ];
    }
}
