<?php
declare(strict_types=1);

/**
 * Functional pools (§10 ext) — groups of monitored systems that drive the
 * top-level dashboard cards.
 *   GET  /api/pools            list pools + asset count + health rollup
 *   POST /api/pools            create a pool        (-> bridge POOL_NEW)
 *   POST /api/pools/{poolId}   rename/recolor       (-> bridge POOL_SET)
 *
 * Reads are SELECT-only; every write is marshalled to the C bridge (PHP never
 * writes the DB).
 */

return static function (Router $router): void {

    $router->get('/api/pools', static function (): void {
        $states = Rollup::assetStates();                 // assetId => state
        $byPool = [];
        foreach (Db::rows('SELECT assetId, poolId FROM asset') as $a) {
            $pid = $a['poolId'] !== null ? (int) $a['poolId'] : 0;
            if (!isset($byPool[$pid])) $byPool[$pid] = ['count' => 0, 'health' => Rollup::emptyHealth()];
            $byPool[$pid]['count']++;
            $st = $states[(int) $a['assetId']] ?? 'unknown';
            $byPool[$pid]['health'][$st]++;
        }
        $out = [];
        foreach (Db::rows('SELECT poolId, name, description, color FROM pool ORDER BY name') as $p) {
            $pid = (int) $p['poolId'];
            $agg = $byPool[$pid] ?? ['count' => 0, 'health' => Rollup::emptyHealth()];
            $out[] = [
                'poolId'      => $pid,
                'name'        => $p['name'],
                'description' => $p['description'],
                'color'       => $p['color'],
                'assetCount'  => $agg['count'],
                'health'      => $agg['health'],
            ];
        }
        Response::ok($out);
    });

    $router->post('/api/pools', static function (): void {
        $b    = solari_json_body();
        $name = isset($b['name']) ? trim((string) $b['name']) : '';
        if ($name === '' || strlen($name) > 64) {
            Response::error('bad_request', 'name is required (1–64 chars).', 400);
        }
        $args = ['name' => $name];
        if (isset($b['description']) && $b['description'] !== '') $args['desc']  = substr((string) $b['description'], 0, 240);
        if (isset($b['color']) && $b['color'] !== '')             $args['color'] = substr((string) $b['color'], 0, 15);
        $reply = SolariCtl::call('POOL_NEW', $args);
        Response::ok(['poolId' => isset($reply['poolId']) ? (int) $reply['poolId'] : null, 'name' => $name], 201);
    });

    $router->post('/api/pools/{poolId}', static function (array $p): void {
        if (preg_match('/^\d+$/', $p['poolId']) !== 1 || $p['poolId'] === '0') {
            Response::error('bad_request', 'poolId must be a positive integer.', 400);
        }
        if (Db::row('SELECT poolId FROM pool WHERE poolId = :i', [':i' => (int) $p['poolId']]) === null) {
            Response::error('not_found', "No pool {$p['poolId']}", 404);
        }
        $b    = solari_json_body();
        $args = ['pool' => $p['poolId']];
        if (array_key_exists('name', $b)) {
            $n = trim((string) $b['name']);
            if ($n === '' || strlen($n) > 64) Response::error('bad_request', 'name must be 1–64 chars.', 400);
            $args['name'] = $n;
        }
        if (array_key_exists('description', $b)) $args['desc']  = substr((string) $b['description'], 0, 240);
        if (array_key_exists('color', $b))       $args['color'] = substr((string) $b['color'], 0, 15);
        if (count($args) === 1) Response::error('bad_request', 'No editable fields supplied.', 400);
        SolariCtl::call('POOL_SET', $args);
        Response::ok(['poolId' => (int) $p['poolId']]);
    });
};
