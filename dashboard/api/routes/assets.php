<?php
declare(strict_types=1);

/**
 * Monitored systems / assets (§10 ext).
 *   GET  /api/assets[?poolId=]   list systems + health + target count
 *   GET  /api/assets/{assetId}   one system: metadata + its probe targets + vantages
 *   POST /api/assets/{assetId}   edit metadata / pool / heartbeat (-> bridge ASSET_SET)
 *
 * Reads are SELECT-only. The edit reads the current row, merges the patch, and
 * marshals the full desired metadata to the C bridge (keyed by ip); PHP never
 * writes the DB.
 */

return static function (Router $router): void {

    $router->get('/api/assets', static function (): void {
        $states = Rollup::assetStates();
        $pools  = [];
        foreach (Db::rows('SELECT poolId, name FROM pool') as $p) $pools[(int) $p['poolId']] = $p['name'];
        $tc = [];
        foreach (Db::rows('SELECT assetId, COUNT(*) c FROM probeTarget WHERE assetId IS NOT NULL GROUP BY assetId') as $r) {
            $tc[(int) $r['assetId']] = (int) $r['c'];
        }
        $filter = isset($_GET['poolId']) && ctype_digit((string) $_GET['poolId']) ? (int) $_GET['poolId'] : null;

        $out = [];
        foreach (Db::rows('SELECT assetId, ip, host, displayName, class, poolId, tags, notes, monitorHost
                             FROM asset ORDER BY displayName, ip') as $a) {
            $pid = $a['poolId'] !== null ? (int) $a['poolId'] : null;
            if ($filter !== null && $pid !== $filter) continue;
            $aid = (int) $a['assetId'];
            $out[] = asset_row($a, $aid, $pid, $pools, $tc[$aid] ?? 0, $states[$aid] ?? 'unknown');
        }
        Response::ok($out);
    });

    $router->get('/api/assets/{assetId}', static function (array $p): void {
        $aid = asset_id($p['assetId']);
        $a = Db::row('SELECT assetId, ip, host, displayName, class, poolId, tags, notes, monitorHost
                        FROM asset WHERE assetId = :i', [':i' => $aid]);
        if ($a === null) Response::error('not_found', "No asset $aid", 404);

        $pools = [];
        foreach (Db::rows('SELECT poolId, name FROM pool') as $pp) $pools[(int) $pp['poolId']] = $pp['name'];
        $pid = $a['poolId'] !== null ? (int) $a['poolId'] : null;

        // targets + their current vantages
        $vByT = [];
        foreach (Db::rows('SELECT targetId, monitorNode, outcome, rttMicros, lossPermille, sampledAt
                             FROM probeCurrent') as $v) {
            $vByT[(string) $v['targetId']][] = $v;
        }
        $targets = [];
        foreach (Db::rows('SELECT targetId, host, port, proto, label FROM probeTarget
                            WHERE assetId = :i ORDER BY proto, port', [':i' => $aid]) as $t) {
            $tid = (string) $t['targetId'];
            $vs  = $vByT[$tid] ?? [];
            $targets[] = [
                'targetId' => $tid, 'host' => $t['host'], 'port' => Coerce::int($t['port']),
                'proto' => $t['proto'], 'label' => $t['label'],
                'state' => Rollup::fromVantages($vs), 'vantages' => count($vs),
            ];
        }
        $states = Rollup::assetStates();
        $row = asset_row($a, $aid, $pid, $pools, count($targets), $states[$aid] ?? 'unknown');
        $row['targets'] = $targets;
        Response::ok($row);
    });

    $router->post('/api/assets/{assetId}', static function (array $p): void {
        $aid = asset_id($p['assetId']);
        $cur = Db::row('SELECT ip, host, displayName, class, poolId, tags, notes, monitorHost
                          FROM asset WHERE assetId = :i', [':i' => $aid]);
        if ($cur === null) Response::error('not_found', "No asset $aid", 404);
        $b = solari_json_body();

        // Merge the patch over current state; ASSET_SET writes the full record.
        $name  = array_key_exists('displayName', $b) ? (string) $b['displayName'] : (string) ($cur['displayName'] ?? '');
        $class = array_key_exists('class', $b) ? (string) $b['class'] : (string) ($cur['class'] ?? 'host');
        if (!in_array($class, ['server','appliance','network','iot','host','other'], true)) {
            Response::error('bad_request', 'class invalid.', 400);
        }
        $pool  = array_key_exists('poolId', $b)
               ? ($b['poolId'] === null ? 0 : (int) $b['poolId'])
               : ($cur['poolId'] !== null ? (int) $cur['poolId'] : 0);
        $notes = array_key_exists('notes', $b) ? (string) $b['notes'] : (string) ($cur['notes'] ?? '');
        $hb    = array_key_exists('monitorHost', $b) ? (bool) $b['monitorHost'] : (bool) $cur['monitorHost'];
        $tags  = array_key_exists('tags', $b) && is_array($b['tags'])
               ? array_values($b['tags'])
               : ($cur['tags'] !== null ? (json_decode((string) $cur['tags'], true) ?: []) : []);

        if ($pool !== 0 && Db::row('SELECT poolId FROM pool WHERE poolId = :i', [':i' => $pool]) === null) {
            Response::error('bad_request', "No pool $pool", 400);
        }

        $args = [
            'ip'        => (string) $cur['ip'],
            'name'      => substr($name, 0, 120),
            'class'     => $class,
            'pool'      => (string) $pool,
            'notes'     => substr($notes, 0, 240),
            'tags'      => json_encode($tags),
            'heartbeat' => $hb ? '1' : '0',
        ];
        $op = Operator::name();
        if ($op !== '') $args['op'] = $op;
        SolariCtl::call('ASSET_SET', $args);
        Sor::upsertHost((string) $cur['ip'], $name);   // mirror edit/rename into the SoR (fail-soft)
        Response::ok(['assetId' => $aid, 'status' => 'updated']);
    });

    $router->post('/api/assets/{assetId}/remove', static function (array $p): void {
        $aid = asset_id($p['assetId']);
        $cur = Db::row('SELECT assetId, ip FROM asset WHERE assetId = :i', [':i' => $aid]);
        if ($cur === null) {
            Response::error('not_found', "No asset $aid", 404);
        }
        $b  = solari_json_body();
        $op = Operator::requireOperator();
        if (($b['confirm'] ?? null) !== true) {
            Response::error('confirm_required',
                'Asset removal deletes its probe targets and history; resend with {"confirm":true}.', 409);
        }
        $reply = SolariCtl::call('ASSET_REMOVE', [
            'asset' => (string) $aid,
            'op'    => $op,
        ]);
        Sor::removeHost((string) $cur['ip']);   // soft-delete the SoR host (fail-soft) -> drops from DNS
        Response::ok([
            'assetId' => $aid,
            'status'  => 'removed',
            'removed' => isset($reply['removed']) ? (int) $reply['removed'] : null,
        ]);
    });

    $router->post('/api/assets/{assetId}/targets/{targetId}/remove', static function (array $p): void {
        $aid = asset_id($p['assetId']);
        $targetId = (string) $p['targetId'];
        if ($targetId === '' || strlen($targetId) > 128) {
            Response::error('bad_request', 'targetId invalid.', 400);
        }
        if (Db::row('SELECT targetId FROM probeTarget WHERE assetId = :a AND targetId = :t',
                    [':a' => $aid, ':t' => $targetId]) === null) {
            Response::error('not_found', "No target $targetId for asset $aid", 404);
        }
        $b  = solari_json_body();
        $op = Operator::requireOperator();
        if (($b['confirm'] ?? null) !== true) {
            Response::error('confirm_required',
                'Target removal deletes current and historical probe state; resend with {"confirm":true}.', 409);
        }
        SolariCtl::call('TARGET_REMOVE', [
            'target' => $targetId,
            'op'     => $op,
        ]);
        Response::ok(['assetId' => $aid, 'targetId' => $targetId, 'status' => 'removed']);
    });
};

/** Shape one asset list/detail row. */
function asset_row(array $a, int $aid, ?int $pid, array $pools, int $targetCount, string $state): array
{
    return [
        'assetId'     => $aid,
        'ip'          => $a['ip'],
        'host'        => $a['host'],
        'displayName' => ($a['displayName'] !== null && $a['displayName'] !== '') ? $a['displayName'] : ($a['host'] ?: $a['ip']),
        'class'       => $a['class'],
        'poolId'      => $pid,
        'poolName'    => $pid !== null ? ($pools[$pid] ?? null) : null,
        'tags'        => $a['tags'] !== null ? (json_decode((string) $a['tags'], true) ?: []) : [],
        'notes'       => $a['notes'],
        'monitorHost' => (bool) $a['monitorHost'],
        'targetCount' => $targetCount,
        'state'       => $state,
    ];
}

/** Validate a positive asset id path segment. */
function asset_id($v): int
{
    if (preg_match('/^\d+$/', (string) $v) !== 1 || (string) $v === '0') {
        Response::error('bad_request', 'assetId must be a positive integer.', 400);
    }
    return (int) $v;
}
