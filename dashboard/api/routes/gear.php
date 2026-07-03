<?php
declare(strict_types=1);

/**
 * SNMP-managed network gear endpoints (§5 SNMP tier):
 *   GET /api/gear                        gear roster + interface rollups
 *   GET /api/gear/{id}/interfaces        current per-interface counters/rates
 *   GET /api/gear/{id}/history           time series for one interface metric
 *
 * Backed by: networkGear, gearInterfaceCurrent, gearInterfaceHistory.
 *
 * Read-only (SELECT only): PHP never writes monitoring tables; the SNMP poller
 * (serverSnmp.c) owns all writes. Mirrors the shape of routes/nodes.php and
 * reuses NodeHistoryHelpers (defined there) for the ISO/limit parsing.
 */

return static function (Router $router): void {

    // GET /api/gear — one row per managed device with interface rollups
    // (ifCount, upCount, and summed in/out rates) from gearInterfaceCurrent.
    // LEFT JOIN so gear with no SNMP samples yet still appears.
    $router->get('/api/gear', static function (): void {
        $rows = Db::rows(
            'SELECT g.gearId, g.name, g.kind, g.model, g.segId, g.ports,
                    g.uplinkGearId, g.wireless, g.mgmtIp, g.lastSeenAt,
                    COUNT(c.ifIndex)                          AS ifCount,
                    SUM(CASE WHEN c.operStatus = 1 THEN 1 ELSE 0 END) AS upCount,
                    SUM(c.inRateKbps)                         AS totalInRateKbps,
                    SUM(c.outRateKbps)                        AS totalOutRateKbps
               FROM networkGear g
               LEFT JOIN gearInterfaceCurrent c ON c.gearId = g.gearId
              GROUP BY g.gearId, g.name, g.kind, g.model, g.segId, g.ports,
                       g.uplinkGearId, g.wireless, g.mgmtIp, g.lastSeenAt
              ORDER BY g.gearId'
        );

        $out = array_map(static function (array $g): array {
            return [
                'gearId'           => (string) $g['gearId'],
                'name'             => $g['name'],
                'kind'             => $g['kind'],
                'model'            => $g['model'],
                'segId'            => $g['segId'],
                'ports'            => Coerce::int($g['ports']),
                'uplinkGearId'     => $g['uplinkGearId'],
                'wireless'         => Coerce::bool($g['wireless']),
                'mgmtIp'           => $g['mgmtIp'],
                'lastSeenAt'       => Coerce::iso($g['lastSeenAt']),
                'ifCount'          => Coerce::int($g['ifCount']),
                'upCount'          => Coerce::int($g['upCount']),
                'totalInRateKbps'  => Coerce::int($g['totalInRateKbps']),
                'totalOutRateKbps' => Coerce::int($g['totalOutRateKbps']),
            ];
        }, $rows);

        Response::ok($out);
    });

    // GET /api/gear/{id}/interfaces — current per-interface rows for one gear.
    $router->get('/api/gear/{id}/interfaces', static function (array $p): void {
        $gearId = $p['id'];

        $rows = Db::rows(
            'SELECT ifIndex, ifName, operStatus, inRateKbps, outRateKbps,
                    inOctets, outOctets, sampledAt
               FROM gearInterfaceCurrent
              WHERE gearId = :id
              ORDER BY ifIndex',
            [':id' => $gearId]
        );

        $out = array_map(static function (array $r): array {
            return [
                'ifIndex'     => Coerce::int($r['ifIndex']),
                'ifName'      => $r['ifName'],
                'operStatus'  => Coerce::int($r['operStatus']),
                'inRateKbps'  => Coerce::int($r['inRateKbps']),
                'outRateKbps' => Coerce::int($r['outRateKbps']),
                'inOctets'    => Coerce::int($r['inOctets']),
                'outOctets'   => Coerce::int($r['outOctets']),
                'sampledAt'   => Coerce::iso($r['sampledAt']),
            ];
        }, $rows);

        Response::ok([
            'gearId'     => (string) $gearId,
            'interfaces' => $out,
        ]);
    });

    // GET /api/gear/{id}/history?ifIndex=&metric=&from=&to=&limit=
    // gearInterfaceHistory columns: inRateKbps, outRateKbps.
    $router->get('/api/gear/{id}/history', static function (array $p): void {
        $gearId = $p['id'];

        // Whitelist metric -> column (prevents identifier injection).
        $allowed = [
            'inRateKbps'  => 'inRateKbps',
            'outRateKbps' => 'outRateKbps',
        ];
        $metric = (string) ($_GET['metric'] ?? 'inRateKbps');
        if (!isset($allowed[$metric])) {
            Response::error('bad_request',
                'Unknown metric; allowed: ' . implode(',', array_keys($allowed)), 400);
        }
        $col = $allowed[$metric];

        $where  = ['gearId = :id'];
        $params = [':id' => $gearId];

        // ifIndex is optional; when present, scope the series to one interface.
        $ifIndex = $_GET['ifIndex'] ?? null;
        if ($ifIndex !== null && $ifIndex !== '' && is_numeric($ifIndex)) {
            $where[]           = 'ifIndex = :ifx';
            $params[':ifx']    = (int) $ifIndex;
        }

        $from = $_GET['from'] ?? null;
        $to   = $_GET['to'] ?? null;
        if (is_string($from) && $from !== '') {
            $where[]         = 'sampledAt >= :from';
            $params[':from'] = NodeHistoryHelpers::isoToDb($from);
        }
        if (is_string($to) && $to !== '') {
            $where[]       = 'sampledAt <= :to';
            $params[':to'] = NodeHistoryHelpers::isoToDb($to);
        }

        $limit = NodeHistoryHelpers::clampLimit($_GET['limit'] ?? null, 1000, 5000);

        // $col is whitelist-derived; all values are bound parameters.
        $sql = "SELECT sampledAt, $col AS value
                  FROM gearInterfaceHistory
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
            'gearId'  => (string) $gearId,
            'ifIndex' => ($ifIndex !== null && $ifIndex !== '' && is_numeric($ifIndex)) ? (int) $ifIndex : null,
            'metric'  => $metric,
            'series'  => $series,
        ]);
    });
};
