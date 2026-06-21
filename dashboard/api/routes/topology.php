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
     * Monitoring hierarchy: server -> monitor -> target/client edges.
     * Nodes are the fleet; edges are monitor->target derived from probeCurrent
     * (which vantage/monitor reports each target).
     *
     * @return array<string,mixed>
     */
    public static function monitoring(): array
    {
        $nodes = Db::rows(
            'SELECT nodeId, role, hostFqdn, state, segId FROM node ORDER BY role, hostFqdn'
        );
        $nodeList = array_map(static function (array $n): array {
            return [
                'id'       => Coerce::id($n['nodeId']),
                'kind'     => 'node',
                'role'     => $n['role'],
                'hostFqdn' => $n['hostFqdn'],
                'state'    => $n['state'],
                'segId'    => $n['segId'],
            ];
        }, $nodes);

        // target pseudo-nodes + monitor->target edges from probeCurrent.
        $targets = Db::rows('SELECT targetId, label, proto, host, port, segId FROM probeTarget');
        $targetList = array_map(static function (array $t): array {
            return [
                'id'    => 'target:' . $t['targetId'],
                'kind'  => 'target',
                'label' => $t['label'],
                'proto' => $t['proto'],
                'host'  => $t['host'],
                'port'  => Coerce::int($t['port']),
                'segId' => $t['segId'],
            ];
        }, $targets);

        $edgeRows = Db::rows(
            'SELECT DISTINCT monitorNode, targetId, outcome FROM probeCurrent'
        );
        $edges = array_map(static function (array $e): array {
            return [
                'from'    => Coerce::id($e['monitorNode']),
                'to'      => 'target:' . $e['targetId'],
                'kind'    => 'monitors',
                'outcome' => $e['outcome'],
            ];
        }, $edgeRows);

        return [
            'view'  => 'monitoring',
            'nodes' => array_merge($nodeList, $targetList),
            'edges' => $edges,
        ];
    }

    /**
     * Network hierarchy: gateway -> switch/AP -> host, with uplink ports, link
     * type, speed, and LLDP flag from lldpEdge; gear self-references via
     * uplinkGearId; nodes attach via node.uplinkGearId and lldpEdge.
     *
     * @return array<string,mixed>
     */
    public static function network(): array
    {
        $gear = Db::rows(
            'SELECT gearId, name, kind, model, segId, uplinkGearId, wireless, mgmtIp
               FROM networkGear ORDER BY gearId'
        );
        $gearNodes = array_map(static function (array $g): array {
            return [
                'id'           => 'gear:' . $g['gearId'],
                'kind'         => 'gear',
                'gearKind'     => $g['kind'],
                'name'         => $g['name'],
                'model'        => $g['model'],
                'segId'        => $g['segId'],
                'uplinkGearId' => $g['uplinkGearId'],
                'wireless'     => Coerce::bool($g['wireless']),
                'mgmtIp'       => $g['mgmtIp'],
            ];
        }, $gear);

        $hosts = Db::rows(
            'SELECT nodeId, role, hostFqdn, state, segId, uplinkGearId FROM node'
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
            ];
        }, $hosts);

        // gear -> gear uplink edges.
        $edges = [];
        foreach ($gear as $g) {
            if ($g['uplinkGearId'] !== null && $g['uplinkGearId'] !== '') {
                $edges[] = [
                    'from'     => 'gear:' . $g['uplinkGearId'],
                    'to'       => 'gear:' . $g['gearId'],
                    'kind'     => 'uplink',
                    'linkType' => Coerce::bool($g['wireless']) ? 'wireless' : 'wired',
                ];
            }
        }

        // node -> gear edges from lldpEdge (richer: port/speed/lldp/rssi).
        $lldp = Db::rows(
            'SELECT nodeId, gearId, localIf, peerPort, linkType, speedMbps, rssi, viaLldp
               FROM lldpEdge
              WHERE nodeId IS NOT NULL AND gearId IS NOT NULL'
        );
        $seen = [];
        foreach ($lldp as $e) {
            $from = Coerce::id($e['nodeId']);
            $to   = 'gear:' . $e['gearId'];
            $seen[$from . '|' . $to] = true;
            $edges[] = [
                'from'      => $from,
                'to'        => $to,
                'kind'      => 'attaches',
                'localIf'   => $e['localIf'],
                'peerPort'  => $e['peerPort'],
                'linkType'  => $e['linkType'],
                'speedMbps' => Coerce::int($e['speedMbps']),
                'rssi'      => Coerce::int($e['rssi']),
                'viaLldp'   => Coerce::bool($e['viaLldp']),
            ];
        }

        // fall back to node.uplinkGearId for nodes without an lldpEdge row.
        foreach ($hosts as $n) {
            if ($n['uplinkGearId'] === null || $n['uplinkGearId'] === '') {
                continue;
            }
            $from = Coerce::id($n['nodeId']);
            $to   = 'gear:' . $n['uplinkGearId'];
            if (isset($seen[$from . '|' . $to])) {
                continue;
            }
            $edges[] = [
                'from'    => $from,
                'to'      => $to,
                'kind'    => 'attaches',
                'viaLldp' => false,
            ];
        }

        return [
            'view'  => 'network',
            'nodes' => array_merge($gearNodes, $hostNodes),
            'edges' => $edges,
        ];
    }
}
