<?php
declare(strict_types=1);

/**
 * Probe endpoints:
 *   GET /api/probes  — reachability matrix (§6 amended: label, replFactor, and a
 *                      rolled-up state across all reporting vantages per target).
 *
 * Backed by: probeTarget (label, replFactor, segId, proto, host, port) joined to
 * probeCurrent (per-vantage outcome/rtt/jitter/loss/throughput/sampledAt).
 *
 * Roll-up rule (raw, no display conversion): a target's state is the worst
 * outcome seen across its vantages — ok < degraded < down — where any non-"ok"
 * outcome that is not a hard failure (e.g. elevated loss with outcome ok) maps
 * via the per-vantage outcome only. We surface the raw per-vantage rows too so
 * the adapter can compute its own matrix colouring.
 */

return static function (Router $router): void {

    $router->get('/api/probes', static function (): void {
        $targets = Db::rows(
            'SELECT targetId, host, port, proto, replFactor, label, segId
               FROM probeTarget
              ORDER BY targetId'
        );

        // One query for all current vantage rows, grouped in PHP.
        $current = Db::rows(
            'SELECT targetId, monitorNode, outcome, rttMicros, jitterMicros,
                    lossPermille, throughputKbps, serviceMeta, sampledAt
               FROM probeCurrent
              ORDER BY targetId, monitorNode'
        );

        /** @var array<string,array<int,array<string,mixed>>> $byTarget */
        $byTarget = [];
        foreach ($current as $r) {
            $tid = (string) $r['targetId'];
            $byTarget[$tid][] = [
                'monitorNode'    => Coerce::id($r['monitorNode']),
                'outcome'        => $r['outcome'],
                'rttMicros'      => Coerce::int($r['rttMicros']),
                'jitterMicros'   => Coerce::int($r['jitterMicros']),
                'lossPermille'   => Coerce::int($r['lossPermille']),
                'throughputKbps' => Coerce::int($r['throughputKbps']),
                'serviceMeta'    => Coerce::json($r['serviceMeta']),
                'sampledAt'      => Coerce::iso($r['sampledAt']),
            ];
        }

        $out = array_map(static function (array $t) use ($byTarget): array {
            $tid      = (string) $t['targetId'];
            $vantages = $byTarget[$tid] ?? [];
            return [
                'targetId'   => $tid,
                'host'       => $t['host'],
                'port'       => Coerce::int($t['port']),
                'proto'      => $t['proto'],
                'replFactor' => Coerce::int($t['replFactor']),
                'label'      => $t['label'],
                'segId'      => $t['segId'],
                'state'      => ProbeRollup::state($vantages),
                'vantages'   => $vantages,
            ];
        }, $targets);

        Response::ok($out);
    });
};

/** Roll-up of per-vantage outcomes into a single coarse target state. */
final class ProbeRollup
{
    /**
     * @param array<int,array<string,mixed>> $vantages
     * @return string one of: up | degraded | down | unknown
     */
    public static function state(array $vantages): string
    {
        if ($vantages === []) {
            return 'unknown';
        }
        $okCount   = 0;
        $downCount = 0;
        foreach ($vantages as $v) {
            if (($v['outcome'] ?? null) === 'ok') {
                $okCount++;
            } else {
                $downCount++;
            }
        }
        if ($downCount === 0) {
            return 'up';
        }
        if ($okCount === 0) {
            return 'down';
        }
        return 'degraded'; // reachable from some vantages, not all
    }
}
