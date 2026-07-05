<?php
declare(strict_types=1);

/**
 * Discovery, enrollments, and builds read endpoints (§6):
 *   GET /api/discovery?status=new|all
 *   GET /api/enrollments?status=
 *   GET /api/builds
 *
 * Backed by: discovered, enrollment, buildArtifact, node, nodeConfig.
 * (The adopt/ignore/approve/reject/provision POSTs are added by the follow-up
 * mutation layer; they route through solariCtl. PHP never holds CA material.)
 */

return static function (Router $router): void {

    // GET /api/discovery?status=new|all  (default new == status='new')
    $router->get('/api/discovery', static function (): void {
        $status = (string) ($_GET['status'] ?? 'new');

        $params = [];
        $where  = '';
        if ($status === 'all') {
            $where = '';
        } elseif (in_array($status, ['new', 'ignored', 'adopting', 'adopted'], true)) {
            $where           = 'WHERE status = :status';
            $params[':status'] = $status;
        } else {
            Response::error('bad_request',
                "status must be one of: new, ignored, adopting, adopted, all", 400);
        }

        $rows = Db::rows(
            "SELECT discId, host, ip, kind, via, services, segId, arch,
                    mac, vendor, osName, deviceRole, sysDescr, enrichedAt,
                    seenCount, firstSeenAt, lastSeenAt, status
               FROM discovered
               $where
              ORDER BY lastSeenAt DESC",
            $params
        );

        $out = array_map(static function (array $r): array {
            return [
                'discId'      => Coerce::id($r['discId']),
                'host'        => $r['host'],
                'ip'          => $r['ip'],
                'kind'        => $r['kind'],
                'via'         => $r['via'],
                'services'    => Coerce::json($r['services']),
                'segId'       => $r['segId'],
                'arch'        => $r['arch'],
                'mac'         => $r['mac'],
                'vendor'      => $r['vendor'],
                'osName'      => $r['osName'],
                'deviceRole'  => $r['deviceRole'],
                'sysDescr'    => $r['sysDescr'],
                'enrichedAt'  => Coerce::iso($r['enrichedAt']),
                'seenCount'   => Coerce::int($r['seenCount']),
                'firstSeenAt' => Coerce::iso($r['firstSeenAt']),
                'lastSeenAt'  => Coerce::iso($r['lastSeenAt']),
                'status'      => $r['status'],
            ];
        }, $rows);

        Response::ok($out);
    });

    // GET /api/enrollments?status=
    $router->get('/api/enrollments', static function (): void {
        $params = [];
        $where  = '';
        $status = $_GET['status'] ?? null;
        if (is_string($status) && $status !== '') {
            if (!in_array($status, ['token', 'pending', 'approved', 'rejected', 'expired'], true)) {
                Response::error('bad_request',
                    "status must be one of: token, pending, approved, rejected, expired", 400);
            }
            $where           = 'WHERE status = :status';
            $params[':status'] = $status;
        }

        $rows = Db::rows(
            "SELECT enrId, host, ip, role, certFp, status, requestedAt, decidedAt, decidedBy
               FROM enrollment
               $where
              ORDER BY requestedAt DESC",
            $params
        );

        // csrPem is intentionally never returned to the web tier.
        $out = array_map(static function (array $r): array {
            return [
                'enrId'       => Coerce::id($r['enrId']),
                'host'        => $r['host'],
                'ip'          => $r['ip'],
                'role'        => $r['role'],
                'certFp'      => $r['certFp'],
                'status'      => $r['status'],
                'requestedAt' => Coerce::iso($r['requestedAt']),
                'decidedAt'   => Coerce::iso($r['decidedAt']),
                'decidedBy'   => $r['decidedBy'],
            ];
        }, $rows);

        Response::ok($out);
    });

    // GET /api/builds — registry + per-arch convergence (nodes on each version).
    $router->get('/api/builds', static function (): void {
        $builds = Db::rows(
            'SELECT buildId, arch, os, version, channel, sha256, sizeBytes,
                    artifactUri, publishedAt
               FROM buildArtifact
              ORDER BY arch, os, publishedAt DESC'
        );

        // Convergence: count nodes whose applied build (configBlob->version, or
        // appliedEpoch presence) lands on a given (arch, version). The schema
        // tracks appliedEpoch in nodeConfig and arch on node; we report the raw
        // node-version distribution per arch so the adapter can flag updates.
        $nodeVers = Db::rows(
            "SELECT n.arch AS arch,
                    JSON_UNQUOTE(JSON_EXTRACT(c.configBlob, '$.version')) AS version,
                    COUNT(*) AS n
               FROM node n
               LEFT JOIN nodeConfig c ON c.nodeId = n.nodeId
              WHERE n.state <> 'retired'
              GROUP BY n.arch, version"
        );
        // index: arch -> version -> count
        $dist = [];
        foreach ($nodeVers as $r) {
            $arch = (string) ($r['arch'] ?? '');
            $ver  = $r['version'] === null ? '' : (string) $r['version'];
            $dist[$arch][$ver] = Coerce::int($r['n']);
        }

        $out = array_map(static function (array $b) use ($dist): array {
            $arch = (string) $b['arch'];
            $ver  = (string) $b['version'];
            return [
                'buildId'     => Coerce::id($b['buildId']),
                'arch'        => $arch,
                'os'          => $b['os'],
                'version'     => $ver,
                'channel'     => $b['channel'],
                'sha256'      => $b['sha256'],
                'sizeBytes'   => Coerce::int($b['sizeBytes']),
                'artifactUri' => $b['artifactUri'],
                'publishedAt' => Coerce::iso($b['publishedAt']),
                'nodesOnThisVersion' => $dist[$arch][$ver] ?? 0,
            ];
        }, $builds);

        Response::ok([
            'builds'           => $out,
            'nodeVersionDist'  => $dist,
        ]);
    });
};
