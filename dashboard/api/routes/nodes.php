<?php
declare(strict_types=1);

/**
 * Node endpoints (§11.2):
 *   GET /api/nodes                  fleet roster + rollup state
 *   GET /api/nodes/{id}             full current host metrics + procs + alerts
 *   GET /api/nodes/{id}/history     time series for one metric (host history)
 *
 * Backed by: node, hostCurrent, procCurrent, hostHistory, alertEvent, alertRule.
 */

return static function (Router $router): void {

    // GET /api/nodes — fleet summaries (+ §5 additions: segId, uplinkGearId).
    $router->get('/api/nodes', static function (): void {
        $rows = Db::rows(
            'SELECT nodeId, role, hostFqdn, state, lastSeenAt, arch, osName,
                    segId, uplinkGearId, enrolledAt, configEpoch
               FROM node
              ORDER BY role, hostFqdn'
        );
        $out = array_map(static function (array $r): array {
            return [
                'nodeId'       => Coerce::id($r['nodeId']),
                'role'         => $r['role'],
                'hostFqdn'     => $r['hostFqdn'],
                'state'        => $r['state'],
                'lastSeenAt'   => Coerce::iso($r['lastSeenAt']),
                'arch'         => $r['arch'],
                'osName'       => $r['osName'],
                'segId'        => $r['segId'],
                'uplinkGearId' => $r['uplinkGearId'],
                'enrolledAt'   => Coerce::iso($r['enrolledAt']),
                'configEpoch'  => Coerce::id($r['configEpoch']),
            ];
        }, $rows);
        Response::ok($out);
    });

    // GET /api/nodes/{id} — full detail for one node.
    $router->get('/api/nodes/{id}', static function (array $p): void {
        $nodeId = $p['id'];

        $node = Db::row(
            'SELECT nodeId, role, hostFqdn, certCn, osName, arch, segId, uplinkGearId,
                    enrolledAt, lastSeenAt, configEpoch, state
               FROM node WHERE nodeId = :id',
            [':id' => $nodeId]
        );
        if ($node === null) {
            Response::error('not_found', "No node $nodeId", 404);
        }

        $host = Db::row(
            'SELECT sampledAt, cpuLoadMilli, ramUsedKb, ramTotalKb, swapUsedKb,
                    swapTotalKb, disks, ifaces, usbBuses
               FROM hostCurrent WHERE nodeId = :id',
            [':id' => $nodeId]
        );

        $procs = Db::rows(
            'SELECT procName, pid, runState, nFiles, nSockets, rssKb, sampledAt
               FROM procCurrent WHERE nodeId = :id ORDER BY procName',
            [':id' => $nodeId]
        );

        $alerts = Db::rows(
            'SELECT e.eventId, e.ruleId, e.targetId, e.firedAt, e.clearedAt,
                    e.severity, e.detail, r.scope, r.metric, r.op, r.threshold
               FROM alertEvent e
               LEFT JOIN alertRule r ON r.ruleId = e.ruleId
              WHERE e.nodeId = :id
              ORDER BY e.firedAt DESC
              LIMIT 50',
            [':id' => $nodeId]
        );

        Response::ok([
            'node' => [
                'nodeId'       => Coerce::id($node['nodeId']),
                'role'         => $node['role'],
                'hostFqdn'     => $node['hostFqdn'],
                'certCn'       => $node['certCn'],
                'osName'       => $node['osName'],
                'arch'         => $node['arch'],
                'segId'        => $node['segId'],
                'uplinkGearId' => $node['uplinkGearId'],
                'enrolledAt'   => Coerce::iso($node['enrolledAt']),
                'lastSeenAt'   => Coerce::iso($node['lastSeenAt']),
                'configEpoch'  => Coerce::id($node['configEpoch']),
                'state'        => $node['state'],
            ],
            'host' => $host === null ? null : [
                'sampledAt'    => Coerce::iso($host['sampledAt']),
                'cpuLoadMilli' => Coerce::json($host['cpuLoadMilli']),
                'ramUsedKb'    => Coerce::int($host['ramUsedKb']),
                'ramTotalKb'   => Coerce::int($host['ramTotalKb']),
                'swapUsedKb'   => Coerce::int($host['swapUsedKb']),
                'swapTotalKb'  => Coerce::int($host['swapTotalKb']),
                'disks'        => Coerce::json($host['disks']),
                'ifaces'       => Coerce::json($host['ifaces']),
                'usbBuses'     => Coerce::json($host['usbBuses']),
            ],
            'procs' => array_map(static function (array $r): array {
                return [
                    'procName'  => $r['procName'],
                    'pid'       => Coerce::int($r['pid']),
                    'runState'  => $r['runState'],
                    'nFiles'    => Coerce::int($r['nFiles']),
                    'nSockets'  => Coerce::int($r['nSockets']),
                    'rssKb'     => Coerce::int($r['rssKb']),
                    'sampledAt' => Coerce::iso($r['sampledAt']),
                ];
            }, $procs),
            'alerts' => array_map(static function (array $r): array {
                return [
                    'eventId'   => Coerce::id($r['eventId']),
                    'ruleId'    => Coerce::int($r['ruleId']),
                    'targetId'  => $r['targetId'],
                    'firedAt'   => Coerce::iso($r['firedAt']),
                    'clearedAt' => Coerce::iso($r['clearedAt']),
                    'severity'  => $r['severity'],
                    'detail'    => $r['detail'],
                    'scope'     => $r['scope'],
                    'metric'    => $r['metric'],
                    'op'        => $r['op'],
                    'threshold' => Coerce::float($r['threshold']),
                ];
            }, $alerts),
        ]);
    });

    // GET /api/nodes/{id}/history?metric=&from=&to=&limit=
    // hostHistory columns: cpuAvgMilli, ramUsedKb, swapUsedKb, diskMinFreePct.
    $router->get('/api/nodes/{id}/history', static function (array $p): void {
        $nodeId = $p['id'];

        // Whitelist metric -> column (prevents identifier injection).
        $allowed = [
            'cpuAvgMilli'    => 'cpuAvgMilli',
            'ramUsedKb'      => 'ramUsedKb',
            'swapUsedKb'     => 'swapUsedKb',
            'diskMinFreePct' => 'diskMinFreePct',
        ];
        $metric = (string) ($_GET['metric'] ?? 'cpuAvgMilli');
        if (!isset($allowed[$metric])) {
            Response::error('bad_request',
                'Unknown metric; allowed: ' . implode(',', array_keys($allowed)), 400);
        }
        $col = $allowed[$metric];

        $where  = ['nodeId = :id'];
        $params = [':id' => $nodeId];

        $from = $_GET['from'] ?? null;
        $to   = $_GET['to'] ?? null;
        if (is_string($from) && $from !== '') {
            $where[]        = 'sampledAt >= :from';
            $params[':from'] = NodeHistoryHelpers::isoToDb($from);
        }
        if (is_string($to) && $to !== '') {
            $where[]      = 'sampledAt <= :to';
            $params[':to'] = NodeHistoryHelpers::isoToDb($to);
        }

        $limit = NodeHistoryHelpers::clampLimit($_GET['limit'] ?? null, 1000, 5000);

        // $col is whitelist-derived; all values are bound parameters.
        $sql = "SELECT sampledAt, $col AS value
                  FROM hostHistory
                 WHERE " . implode(' AND ', $where) . "
                 ORDER BY sampledAt ASC
                 LIMIT $limit";
        $rows = Db::rows($sql, $params);

        $series = array_map(static function (array $r): array {
            return [
                'ts'    => Coerce::iso($r['sampledAt']),
                'value' => Coerce::int($r['value']),
            ];
        }, $rows);

        Response::ok([
            'nodeId' => Coerce::id($nodeId),
            'metric' => $metric,
            'series' => $series,
        ]);
    });
};

/**
 * Local helpers shared by the node routes. Declared as a final class so they are
 * callable via self:: from the closures above without polluting the global ns.
 */
final class NodeHistoryHelpers
{
    public static function isoToDb(string $iso): string
    {
        $dt = DateTimeImmutable::createFromFormat(DateTimeInterface::ATOM, $iso)
            ?: DateTimeImmutable::createFromFormat('Y-m-d\TH:i:s\Z', $iso, new DateTimeZone('UTC'));
        if ($dt === false) {
            Response::error('bad_request', "Invalid ISO-8601 timestamp: $iso", 400);
        }
        return $dt->setTimezone(new DateTimeZone('UTC'))->format('Y-m-d H:i:s');
    }

    public static function clampLimit($raw, int $default, int $max): int
    {
        if ($raw === null || $raw === '' || !is_numeric($raw)) {
            return $default;
        }
        $n = (int) $raw;
        if ($n < 1) {
            return $default;
        }
        return min($n, $max);
    }
}
