<?php
declare(strict_types=1);

/**
 * Segments, network gear, and topology endpoints (§6):
 *   GET /api/segments                          segments + node rollup counts
 *   GET /api/netgear                           gear inventory + attached counts
 *   GET /api/topology?view=monitoring|network  dual-hierarchy graph
 *
 * Backed by: segment, networkGear, lldpEdge, node, probeTarget, probeCurrent.
 */

return static function (Router $router): void {

    // GET /api/segments — {segId,label,cidr,wireless, roll:{up,degraded,down,unknown}}
    $router->get('/api/segments', static function (): void {
        $segs = Db::rows(
            'SELECT segId, label, cidr, wireless, notes FROM segment ORDER BY segId'
        );

        // node state counts grouped by segId.
        $counts = Db::rows(
            "SELECT segId,
                    SUM(state='up')       AS up,
                    SUM(state='degraded') AS degraded,
                    SUM(state='down')     AS down,
                    SUM(state IN('unknown','retired')) AS unknown
               FROM node
              WHERE segId IS NOT NULL
              GROUP BY segId"
        );
        $roll = [];
        foreach ($counts as $c) {
            $roll[(string) $c['segId']] = [
                'up'       => Coerce::int($c['up']),
                'degraded' => Coerce::int($c['degraded']),
                'down'     => Coerce::int($c['down']),
                'unknown'  => Coerce::int($c['unknown']),
            ];
        }

        $out = array_map(static function (array $s) use ($roll): array {
            $sid = (string) $s['segId'];
            return [
                'segId'    => $sid,
                'label'    => $s['label'],
                'cidr'     => $s['cidr'],
                'wireless' => Coerce::bool($s['wireless']),
                'notes'    => $s['notes'],
                'roll'     => $roll[$sid] ?? ['up' => 0, 'degraded' => 0, 'down' => 0, 'unknown' => 0],
            ];
        }, $segs);

        Response::ok($out);
    });

    // GET /api/netgear — inventory + attached-node counts + uplink chain.
    $router->get('/api/netgear', static function (): void {
        $gear = Db::rows(
            'SELECT gearId, name, kind, model, segId, ports, uplinkGearId,
                    wireless, mgmtIp, lastSeenAt
               FROM networkGear
              ORDER BY gearId'
        );

        // attached node count per gear, via node.uplinkGearId and lldpEdge.gearId.
        $byNode = Db::rows(
            'SELECT uplinkGearId AS gearId, COUNT(*) AS n
               FROM node WHERE uplinkGearId IS NOT NULL GROUP BY uplinkGearId'
        );
        $byEdge = Db::rows(
            'SELECT gearId, COUNT(DISTINCT nodeId) AS n
               FROM lldpEdge WHERE gearId IS NOT NULL AND nodeId IS NOT NULL
              GROUP BY gearId'
        );
        $attached = [];
        foreach ($byNode as $r) {
            $attached[(string) $r['gearId']] = Coerce::int($r['n']);
        }
        foreach ($byEdge as $r) {
            $g = (string) $r['gearId'];
            $attached[$g] = max($attached[$g] ?? 0, Coerce::int($r['n']));
        }

        $out = array_map(static function (array $g) use ($attached): array {
            $gid = (string) $g['gearId'];
            return [
                'gearId'        => $gid,
                'name'          => $g['name'],
                'kind'          => $g['kind'],
                'model'         => $g['model'],
                'segId'         => $g['segId'],
                'ports'         => Coerce::int($g['ports']),
                'uplinkGearId'  => $g['uplinkGearId'],
                'wireless'      => Coerce::bool($g['wireless']),
                'mgmtIp'        => $g['mgmtIp'],
                'lastSeenAt'    => Coerce::iso($g['lastSeenAt']),
                'attachedNodes' => $attached[$gid] ?? 0,
            ];
        }, $gear);

        Response::ok($out);
    });

    // GET /api/topology?view=monitoring|network
    $router->get('/api/topology', static function (): void {
        $view = (string) ($_GET['view'] ?? 'monitoring');
        if ($view === 'monitoring') {
            Response::ok(TopologyViews::monitoring());
        } elseif ($view === 'network') {
            Response::ok(TopologyViews::network());
        } else {
            Response::error('bad_request', "view must be 'monitoring' or 'network'", 400);
        }
    });
};

/** Builders for the two topology projections. */
final class TopologyViews
{
    /**
     * Monitoring hierarchy: fleet nodes + monitored-target pseudo-nodes; edges
     * are monitor->target derived from probeCurrent (which vantage reports each
     * target). Nodes and edges both carry health for coloring.
     *
     * Node health = node.state; staleSec is computed in SQL (TIMESTAMPDIFF vs
     * NOW()) to avoid TZ drift. Target health = worst probe outcome across its
     * vantages. Edge health = that vantage's own outcome mapping.
     *
     * @return array<string,mixed>
     */
    public static function monitoring(): array
    {
        $nodes = Db::rows(
            'SELECT nodeId, role, hostFqdn, state, segId, lastSeenAt,
                    TIMESTAMPDIFF(SECOND, lastSeenAt, NOW()) AS staleSec
               FROM node ORDER BY role, hostFqdn'
        );
        $nodeList = array_map(static function (array $n): array {
            return [
                'id'         => Coerce::id($n['nodeId']),
                'kind'       => 'node',
                'role'       => $n['role'],
                'hostFqdn'   => $n['hostFqdn'],
                'state'      => $n['state'],
                'segId'      => $n['segId'],
                'health'     => $n['state'] ?? 'unknown',
                'lastSeenAt' => Coerce::iso($n['lastSeenAt']),
                'staleSec'   => Coerce::int($n['staleSec']),
            ];
        }, $nodes);

        // target metadata (indexed by targetId for edge labels + node build).
        $targets   = Db::rows('SELECT targetId, label, proto, host, port, segId FROM probeTarget');
        $targetsById = [];
        foreach ($targets as $t) {
            $targetsById[(string) $t['targetId']] = $t;
        }

        // one row per (targetId, monitorNode) — PK guarantees current sample.
        $probes = Db::rows(
            'SELECT monitorNode, targetId, outcome, rttMicros, jitterMicros,
                    lossPermille, sampledAt
               FROM probeCurrent'
        );

        // worst per-target health across its vantages.
        $targetHealth = [];
        foreach ($probes as $p) {
            $tid = (string) $p['targetId'];
            $h   = self::probeHealth($p['outcome'], Coerce::int($p['lossPermille']));
            $targetHealth[$tid] = self::worse($targetHealth[$tid] ?? 'unknown', $h);
        }

        $targetList = array_map(static function (array $t) use ($targetHealth): array {
            $tid = (string) $t['targetId'];
            return [
                'id'     => 'target:' . $tid,
                'kind'   => 'target',
                'label'  => $t['label'],
                'proto'  => $t['proto'],
                'host'   => $t['host'],
                'port'   => Coerce::int($t['port']),
                'segId'  => $t['segId'],
                'health' => $targetHealth[$tid] ?? 'unknown',
            ];
        }, $targets);

        $edges = array_map(static function (array $e) use ($targetsById): array {
            $tid   = (string) $e['targetId'];
            $rtt   = Coerce::int($e['rttMicros']);
            $loss  = Coerce::int($e['lossPermille']);
            $proto = isset($targetsById[$tid]) ? (string) $targetsById[$tid]['proto'] : '';
            return [
                'from'         => Coerce::id($e['monitorNode']),
                'to'           => 'target:' . $tid,
                'kind'         => 'monitors',
                'outcome'      => $e['outcome'],
                'rttMicros'    => $rtt,
                'lossPermille' => $loss,
                'jitterMicros' => Coerce::int($e['jitterMicros']),
                'sampledAt'    => Coerce::iso($e['sampledAt']),
                'health'       => self::probeHealth($e['outcome'], $loss),
                'label'        => self::monitorLabel($proto, $rtt, $loss),
            ];
        }, $probes);

        return [
            'view'  => 'monitoring',
            'nodes' => array_merge($nodeList, $targetList),
            'edges' => $edges,
        ];
    }

    /**
     * Network hierarchy: gateway -> switch/AP -> host, with uplink ports, link
     * type, speed, and LLDP flag from lldpEdge; gear self-references via
     * uplinkGearId; nodes attach via node.uplinkGearId and lldpEdge. Gear/host
     * nodes and every edge carry health, decorated from gearInterfaceCurrent.
     *
     * @return array<string,mixed>
     */
    public static function network(): array
    {
        // interface telemetry, fetched once. Indexed by gearId (rollup) and by
        // gearId+ifName (edge joins). Empty table -> everything degrades to
        // "unknown", never a fatal error.
        $ifaces = Db::rows(
            'SELECT gearId, ifIndex, ifName, inRateKbps, outRateKbps, operStatus, sampledAt
               FROM gearInterfaceCurrent'
        );
        $ifByGear     = [];   // gearId -> list of interface rows (for rollup)
        $ifByGearName = [];   // gearId -> ifName -> interface row (for joins)
        foreach ($ifaces as $r) {
            $gid = (string) $r['gearId'];
            $ifByGear[$gid][] = $r;
            $nm = $r['ifName'];
            if ($nm !== null && $nm !== '') {
                $ifByGearName[$gid][(string) $nm] = $r;
            }
        }

        $gear = Db::rows(
            'SELECT gearId, name, kind, model, segId, uplinkGearId, wireless, mgmtIp, lastSeenAt,
                    TIMESTAMPDIFF(SECOND, lastSeenAt, NOW()) AS staleSec
               FROM networkGear ORDER BY gearId'
        );
        $gearStale = [];   // gearId -> staleSec (for edge fallbacks)
        foreach ($gear as $g) {
            $gearStale[(string) $g['gearId']] = Coerce::int($g['staleSec']);
        }

        $gearNodes = array_map(static function (array $g) use ($ifByGear): array {
            $gid   = (string) $g['gearId'];
            $stale = Coerce::int($g['staleSec']);
            [$ifUp, $ifDown, $ifTotal, $rollup] = self::ifRollup($ifByGear[$gid] ?? []);
            return [
                'id'           => 'gear:' . $gid,
                'kind'         => 'gear',
                'gearKind'     => $g['kind'],
                'name'         => $g['name'],
                'model'        => $g['model'],
                'segId'        => $g['segId'],
                'uplinkGearId' => $g['uplinkGearId'],
                'wireless'     => Coerce::bool($g['wireless']),
                'mgmtIp'       => $g['mgmtIp'],
                'health'       => self::gearHealth($rollup, $stale),
                'lastSeenAt'   => Coerce::iso($g['lastSeenAt']),
                'staleSec'     => $stale,
                'ifUp'         => $ifUp,
                'ifDown'       => $ifDown,
                'ifTotal'      => $ifTotal,
            ];
        }, $gear);

        $hosts = Db::rows(
            'SELECT nodeId, role, hostFqdn, state, segId, uplinkGearId, lastSeenAt,
                    TIMESTAMPDIFF(SECOND, lastSeenAt, NOW()) AS staleSec
               FROM node'
        );
        $hostNodes = array_map(static function (array $n): array {
            return [
                'id'           => Coerce::id($n['nodeId']),
                'kind'         => 'node',
                'role'         => $n['role'],
                'hostFqdn'     => $n['hostFqdn'],
                'state'        => $n['state'],
                'segId'        => $n['segId'],
                'uplinkGearId' => $n['uplinkGearId'],
                'health'       => $n['state'] ?? 'unknown',
                'lastSeenAt'   => Coerce::iso($n['lastSeenAt']),
                'staleSec'     => Coerce::int($n['staleSec']),
            ];
        }, $hosts);

        // gear -> gear uplink edges. No column identifies the child's uplink
        // port, so interface fields stay null and health falls back to the
        // child gear's staleness.
        $edges = [];
        foreach ($gear as $g) {
            if ($g['uplinkGearId'] === null || $g['uplinkGearId'] === '') {
                continue;
            }
            $childStale = Coerce::int($g['staleSec']);
            $wireless   = Coerce::bool($g['wireless']);
            $edges[] = [
                'from'        => 'gear:' . $g['uplinkGearId'],
                'to'          => 'gear:' . $g['gearId'],
                'kind'        => 'uplink',
                'linkType'    => $wireless ? 'wireless' : 'wired',
                'health'      => self::staleHealth($childStale),
                'operStatus'  => null,
                'inRateKbps'  => null,
                'outRateKbps' => null,
                'utilPct'     => null,
                'speedMbps'   => null,
                'ifName'      => null,
                'label'       => $wireless ? 'wireless' : 'wired',
            ];
        }

        // node -> gear edges from lldpEdge (richer: port/speed/lldp/rssi),
        // decorated by joining gearInterfaceCurrent on gearId AND (ifName =
        // peerPort OR ifName = localIf). lldpEdge is append-only (one row per
        // observation), so collapse to the MOST RECENT row per distinct
        // (nodeId, gearId, localIf) link — otherwise the projection fans out to
        // one edge per historical sample.
        $lldp = Db::rows(
            'SELECT nodeId, gearId, localIf, peerPort, linkType, speedMbps, rssi, viaLldp
               FROM (
                 SELECT nodeId, gearId, localIf, peerPort, linkType, speedMbps,
                        rssi, viaLldp,
                        ROW_NUMBER() OVER (
                          PARTITION BY nodeId, gearId, localIf
                          ORDER BY observedAt DESC, id DESC
                        ) AS rn
                   FROM lldpEdge
                  WHERE nodeId IS NOT NULL AND gearId IS NOT NULL
               ) t
              WHERE rn = 1'
        );
        $seen = [];
        foreach ($lldp as $e) {
            $from = Coerce::id($e['nodeId']);
            $gid  = (string) $e['gearId'];
            $to   = 'gear:' . $gid;
            $seen[$from . '|' . $to] = true;

            $speed    = Coerce::int($e['speedMbps']);
            $rssi     = Coerce::int($e['rssi']);
            $linkType = $e['linkType'];
            $iface    = $ifByGearName[$gid][(string) $e['peerPort']]
                     ?? $ifByGearName[$gid][(string) $e['localIf']]
                     ?? null;

            $edges[] = self::attachEdge(
                $from, $to, $gid, $linkType, $speed, $rssi,
                Coerce::bool($e['viaLldp']), $e['localIf'], $e['peerPort'],
                $iface, $gearStale
            );
        }

        // fall back to node.uplinkGearId for nodes without an lldpEdge row.
        foreach ($hosts as $n) {
            if ($n['uplinkGearId'] === null || $n['uplinkGearId'] === '') {
                continue;
            }
            $from = Coerce::id($n['nodeId']);
            $gid  = (string) $n['uplinkGearId'];
            $to   = 'gear:' . $gid;
            if (isset($seen[$from . '|' . $to])) {
                continue;
            }
            $edges[] = self::attachEdge(
                $from, $to, $gid, null, null, null,
                false, null, null, null, $gearStale
            );
        }

        return [
            'view'  => 'network',
            'nodes' => array_merge($gearNodes, $hostNodes),
            'edges' => $edges,
        ];
    }

    // ---- health + label helpers (pure) ---------------------------------------

    /** Severity rank: worse = higher. unknown is lowest so real data wins. */
    private static function rank(string $h): int
    {
        switch ($h) {
            case 'down':     return 3;
            case 'degraded': return 2;
            case 'up':       return 1;
            default:         return 0; // unknown
        }
    }

    /** Return whichever of the two healths is worse (higher severity). */
    private static function worse(string $a, string $b): string
    {
        return self::rank($b) > self::rank($a) ? $b : $a;
    }

    /** Monitoring interconnect / target health from a probe outcome. */
    private static function probeHealth(?string $outcome, ?int $lossPermille): string
    {
        switch ($outcome) {
            case 'ok':
                $h = 'up';
                break;
            case 'timeout':
            case 'unreachable':
            case 'dns_fail':
                $h = 'down';
                break;
            case 'refused':
            case 'tls_fail':
            case 'proto_err':
                $h = 'degraded';
                break;
            default:
                $h = 'unknown';
        }
        if ($lossPermille !== null && $lossPermille >= 100 && $h === 'up') {
            $h = 'degraded';
        }
        return $h;
    }

    /** Interface rollup -> [ifUp, ifDown, ifTotal, rollupHealth|null]. */
    private static function ifRollup(array $rows): array
    {
        $up = 0;
        $down = 0;
        foreach ($rows as $r) {
            if ((int) $r['operStatus'] === 1) {
                $up++;
            } else {
                $down++;   // operStatus 2 or anything non-1 counts as down
            }
        }
        $total = $up + $down;
        if ($total === 0) {
            $rollup = null;
        } elseif ($down === 0) {
            $rollup = 'up';
        } elseif ($up === 0) {
            $rollup = 'down';
        } else {
            $rollup = 'degraded';
        }
        return [$up, $down, $total, $rollup];
    }

    /**
     * Gear health = interface rollup capped by staleness. lastSeen > 900s -> down;
     * > 180s -> at most degraded. No interfaces and no staleness signal -> unknown.
     */
    private static function gearHealth(?string $rollup, ?int $staleSec): string
    {
        $h = $rollup; // may be null
        if ($staleSec !== null) {
            if ($staleSec > 900) {
                $h = 'down';
            } elseif ($staleSec > 180 && ($h === null || $h === 'up')) {
                $h = 'degraded';
            }
        }
        return $h ?? 'unknown';
    }

    /** Staleness-only health (edge fallback when no interface is identifiable). */
    private static function staleHealth(?int $staleSec): string
    {
        if ($staleSec === null) {
            return 'unknown';
        }
        if ($staleSec > 900) {
            return 'down';
        }
        if ($staleSec > 180) {
            return 'degraded';
        }
        return 'up';
    }

    /** util % of link capacity, or null when speed is unknown. */
    private static function utilPct(?int $inKbps, ?int $outKbps, ?int $speedMbps): ?int
    {
        if ($speedMbps === null || $speedMbps <= 0) {
            return null;
        }
        $max = max($inKbps ?? 0, $outKbps ?? 0);
        return (int) round($max / ($speedMbps * 1000) * 100);
    }

    /** Network interface health from operStatus + utilisation, or null (no data). */
    private static function netIfHealth(?int $operStatus, ?int $utilPct): ?string
    {
        if ($operStatus === 2) {
            return 'down';
        }
        if ($operStatus === 1) {
            return ($utilPct !== null && $utilPct >= 85) ? 'degraded' : 'up';
        }
        return null;
    }

    /** "1G", "100M", "2.5G", or null. */
    private static function speedLabel(?int $mbps): ?string
    {
        if ($mbps === null || $mbps <= 0) {
            return null;
        }
        if ($mbps % 1000 === 0) {
            return (string) intdiv($mbps, 1000) . 'G';
        }
        if ($mbps >= 1000) {
            return self::trimNum($mbps / 1000) . 'G';
        }
        return $mbps . 'M';
    }

    /** Trim a float to <=1dp without trailing zeros ("0", "3.2", "2.5"). */
    private static function trimNum(float $v): string
    {
        $s = number_format($v, 1, '.', '');
        return rtrim(rtrim($s, '0'), '.');
    }

    /** "TCP · 3.2ms · 0% loss" (rtt in ms 1dp; loss = permille/10 %). */
    private static function monitorLabel(string $proto, ?int $rttMicros, ?int $lossPermille): string
    {
        $parts = [];
        if ($proto !== '') {
            $parts[] = strtoupper($proto);
        }
        $parts[] = $rttMicros !== null
            ? number_format($rttMicros / 1000, 1, '.', '') . 'ms'
            : '—ms';
        $parts[] = ($lossPermille !== null ? self::trimNum($lossPermille / 10) : '—') . '% loss';
        return implode(' · ', $parts);
    }

    /**
     * Build an "attaches" (node -> gear) edge, decorated from a matched
     * gearInterfaceCurrent row when available, else falling back to the gear's
     * staleness for health.
     *
     * @param array<string,mixed>|null $iface     matched interface row or null
     * @param array<string,int|null>   $gearStale gearId -> staleSec
     * @return array<string,mixed>
     */
    private static function attachEdge(
        ?string $from,
        string $to,
        string $gid,
        ?string $linkType,
        ?int $speed,
        ?int $rssi,
        ?bool $viaLldp,
        ?string $localIf,
        ?string $peerPort,
        ?array $iface,
        array $gearStale
    ): array {
        $operStatus  = $iface !== null ? Coerce::int($iface['operStatus']) : null;
        $inKbps      = $iface !== null ? Coerce::int($iface['inRateKbps']) : null;
        $outKbps     = $iface !== null ? Coerce::int($iface['outRateKbps']) : null;
        $ifName      = $iface !== null ? $iface['ifName'] : null;
        $utilPct     = self::utilPct($inKbps, $outKbps, $speed);

        $health = self::netIfHealth($operStatus, $utilPct);
        if ($health === null) {
            $health = self::staleHealth($gearStale[$gid] ?? null);
        }

        return [
            'from'        => $from,
            'to'          => $to,
            'kind'        => 'attaches',
            'localIf'     => $localIf,
            'peerPort'    => $peerPort,
            'linkType'    => $linkType,
            'speedMbps'   => $speed,
            'rssi'        => $rssi,
            'viaLldp'     => $viaLldp,
            'health'      => $health,
            'operStatus'  => $operStatus,
            'inRateKbps'  => $inKbps,
            'outRateKbps' => $outKbps,
            'utilPct'     => $utilPct,
            'label'       => self::attachLabel($linkType, $speed, $rssi, $ifName, $utilPct),
        ];
    }

    /** "1G · eth0 · 12%" (wired) or "wireless · -58dBm" (wireless). */
    private static function attachLabel(
        ?string $linkType,
        ?int $speed,
        ?int $rssi,
        ?string $ifName,
        ?int $utilPct
    ): string {
        $parts = [];
        if ($linkType === 'wireless') {
            $parts[] = 'wireless';
            if ($rssi !== null) {
                $parts[] = $rssi . 'dBm';
            }
        } else {
            $sp = self::speedLabel($speed);
            if ($sp !== null) {
                $parts[] = $sp;
            }
            if ($ifName !== null && $ifName !== '') {
                $parts[] = $ifName;
            }
            if ($utilPct !== null) {
                $parts[] = $utilPct . '%';
            }
        }
        if ($parts === []) {
            return $linkType === 'wireless' ? 'wireless' : 'wired';
        }
        return implode(' · ', $parts);
    }
}
