<?php
declare(strict_types=1);

/**
 * Discovery read endpoint (§6) — the discovered/not-yet-monitored candidate
 * list, enriched with every asset-metadata column plus a best-effort
 * LLDP/topology neighbor.
 *
 *   GET /api/discovery?status=new|ignored|adopting|adopted|all   (default new)
 *
 * Backed by: discovered (identity + nmap/SNMP/mDNS/OUI enrichment) and, for the
 * neighbor decoration, lldpEdge + networkGear + node (cross-referenced by host,
 * since the discovered table carries no nodeId/gearId of its own).
 *
 * Pure read — it never mutates. Adopt/ignore/scan live in
 * routes/discovery_mut.php and route through solariCtl.
 */

return static function (Router $router): void {

    // GET /api/discovery?status=new|all
    $router->get('/api/discovery', static function (): void {
        $status = (string) ($_GET['status'] ?? 'new');

        $params = [];
        $where  = '';
        if ($status === 'all') {
            $where = '';
        } elseif (in_array($status, ['new', 'ignored', 'adopting', 'adopted'], true)) {
            $where             = 'WHERE status = :status';
            $params[':status'] = $status;
        } else {
            Response::error('bad_request',
                'status must be one of: new, ignored, adopting, adopted, all', 400);
        }

        $rows = Db::rows(
            "SELECT discId, host, ip, kind, via, services, segId, arch,
                    mac, vendor, osName, deviceRole, sysDescr, enrichedAt,
                    mdnsName, mdnsServices,
                    seenCount, firstSeenAt, lastSeenAt, status
               FROM discovered
               $where
              ORDER BY lastSeenAt DESC",
            $params
        );

        // Best-effort LLDP/topology neighbor, keyed by host (the discovered table
        // has no nodeId/gearId of its own). Built once; O(1) lookup per row.
        $neighborByHost = DiscoveryReads::neighborsByHost();

        $out = array_map(static function (array $r) use ($neighborByHost): array {
            return [
                'discId'       => Coerce::id($r['discId']),
                'host'         => $r['host'],
                'ip'           => $r['ip'],
                'kind'         => $r['kind'],
                'via'          => $r['via'],
                'services'     => Coerce::json($r['services']),
                'segId'        => $r['segId'],
                'arch'         => $r['arch'],
                'mac'          => $r['mac'],
                'vendor'       => $r['vendor'],
                'osName'       => $r['osName'],
                'deviceRole'   => $r['deviceRole'],
                'sysDescr'     => $r['sysDescr'],
                'enrichedAt'   => Coerce::iso($r['enrichedAt']),
                'mdnsName'     => $r['mdnsName'],
                // comma-joined service types → array of trimmed, unique tokens.
                'mdnsServices' => DiscoveryReads::splitServices($r['mdnsServices']),
                'seenCount'    => Coerce::int($r['seenCount']),
                'firstSeenAt'  => Coerce::iso($r['firstSeenAt']),
                'lastSeenAt'   => Coerce::iso($r['lastSeenAt']),
                'status'       => $r['status'],
                'neighbor'     => DiscoveryReads::lookupNeighbor(
                    $neighborByHost, (string) ($r['host'] ?? '')
                ),
            ];
        }, $rows);

        Response::ok($out);
    });
};

/**
 * File-local read helpers for the discovery route. Kept in a final class so the
 * route closure can reach them by name without leaking free functions.
 */
final class DiscoveryReads
{
    /**
     * Split the comma-joined mDNS service-type list into trimmed, de-duplicated
     * tokens (e.g. "_spotify-connect._tcp, _airplay._tcp"). Null/empty → [].
     *
     * @return list<string>
     */
    public static function splitServices(?string $raw): array
    {
        if ($raw === null || trim($raw) === '') {
            return [];
        }
        $seen = [];
        $out  = [];
        foreach (explode(',', $raw) as $tok) {
            $t = trim($tok);
            if ($t === '' || isset($seen[$t])) {
                continue;
            }
            $seen[$t] = true;
            $out[]    = $t;
        }
        return $out;
    }

    /**
     * Build host → neighbor descriptor for every monitored node that has an LLDP
     * edge, indexed by both the full FQDN and its leading label. Empty topology
     * → empty map (callers degrade to a null neighbor, never a fatal error).
     *
     * @return array<string,array<string,mixed>>
     */
    public static function neighborsByHost(): array
    {
        // gearId → display name, so the neighbor reads as a switch/AP name.
        $gearName = [];
        foreach (Db::rows('SELECT gearId, name FROM networkGear') as $g) {
            $gearName[(string) $g['gearId']] = $g['name'];
        }

        // Most-recent lldpEdge per (nodeId, gearId, localIf); collapse history.
        $edges = Db::rows(
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
        // nodeId → first (best) neighbor edge.
        $byNode = [];
        foreach ($edges as $e) {
            $nid = (string) $e['nodeId'];
            if (isset($byNode[$nid])) {
                continue;
            }
            $gid = (string) $e['gearId'];
            $byNode[$nid] = [
                'gearId'    => $gid,
                'gearName'  => $gearName[$gid] ?? $gid,
                'peerPort'  => $e['peerPort'],
                'localIf'   => $e['localIf'],
                'linkType'  => $e['linkType'],
                'speedMbps' => Coerce::int($e['speedMbps']),
                'rssi'      => Coerce::int($e['rssi']),
                'viaLldp'   => Coerce::bool($e['viaLldp']),
            ];
        }
        if ($byNode === []) {
            return [];
        }

        // host → neighbor, indexed by full FQDN and leading label so "host"
        // (discovery) matches "host.akoria.net" (node) and vice-versa.
        $out = [];
        foreach (Db::rows('SELECT nodeId, hostFqdn FROM node') as $n) {
            $nid = (string) $n['nodeId'];
            if (!isset($byNode[$nid])) {
                continue;
            }
            $fqdn = strtolower(trim((string) ($n['hostFqdn'] ?? '')));
            if ($fqdn === '') {
                continue;
            }
            $out[$fqdn] = $byNode[$nid];
            $label = strtok($fqdn, '.');
            if ($label !== false && $label !== '' && !isset($out[$label])) {
                $out[$label] = $byNode[$nid];
            }
        }
        return $out;
    }

    /**
     * Look up a discovered host in the neighbor map by full name then leading
     * label. Returns null when nothing cross-references.
     *
     * @param array<string,array<string,mixed>> $map
     * @return array<string,mixed>|null
     */
    public static function lookupNeighbor(array $map, string $host): ?array
    {
        if ($map === [] || $host === '') {
            return null;
        }
        $h = strtolower(trim($host));
        if (isset($map[$h])) {
            return $map[$h];
        }
        $label = strtok($h, '.');
        if ($label !== false && $label !== '' && isset($map[$label])) {
            return $map[$label];
        }
        return null;
    }
}
