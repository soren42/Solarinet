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

        // One query for all current vantage rows, grouped in PHP. LEFT JOIN the
        // node roster so a vantage carries the reporting monitor's hostname when
        // that monitor is an enrolled node. Monitor outposts are not always in
        // the node table, so monitorFqdn may be NULL — monitorLabel() below then
        // synthesizes a stable short label. Without SOME name here the SPA's
        // matrix crashed (undefined.localeCompare) and blanked the screen.
        $current = Db::rows(
            'SELECT pc.targetId, pc.monitorNode, n.hostFqdn AS monitorFqdn,
                    pc.outcome, pc.rttMicros, pc.jitterMicros,
                    pc.lossPermille, pc.throughputKbps, pc.serviceMeta, pc.sampledAt
               FROM probeCurrent pc
          LEFT JOIN node n ON n.nodeId = pc.monitorNode
              ORDER BY pc.targetId, pc.monitorNode'
        );

        /** @var array<string,array<int,array<string,mixed>>> $byTarget */
        $byTarget = [];
        foreach ($current as $r) {
            $tid = (string) $r['targetId'];
            $byTarget[$tid][] = [
                'monitorNode'    => Coerce::id($r['monitorNode']),
                'monitorName'    => ProbeRollup::monitorLabel(
                    $r['monitorFqdn'] ?? null, (string) $r['monitorNode']),
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

    /**
     * Human label for a reporting monitor. Prefers the enrolled node's hostname
     * (short form, before the first dot); falls back to a stable synthesized
     * label derived from the monitor's node id when the monitor is not in the
     * roster (e.g. a portable monitor outpost). Never returns an empty string,
     * so the dashboard's matrix column sort can never see undefined/null.
     */
    public static function monitorLabel(?string $fqdn, string $monitorNode): string
    {
        $fqdn = is_string($fqdn) ? trim($fqdn) : '';
        if ($fqdn !== '') {
            $short = strstr($fqdn, '.', true);
            return $short !== false ? $short : $fqdn;
        }
        $id = trim($monitorNode);
        if ($id === '') {
            return 'monitor';
        }
        return 'mon-' . substr($id, -4);
    }
}
