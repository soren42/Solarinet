<?php
declare(strict_types=1);

/**
 * Discovery mutations (§6) — adopt / ignore a discovered candidate.
 *   POST /api/discovery/{discId}/adopt   [body: {spec?:<probeSpec>}]
 *   POST /api/discovery/{discId}/ignore
 *
 * Both route through solariCtl (ADOPT / IGNORE). PHP writes no monitoring tables
 * and holds no CA material; the C bridge performs the actual adopt/ignore.
 * Adopt is a building, non-destructive op so it does not require the operator
 * role; it still records who acted (op=) for audit when available.
 */

return static function (Router $router): void {

    // POST /api/discovery/scan  body: { cidr: "10.0.0.0/24", ports?: "22,80,443" }
    // Triggers an active discovery scan (TCP connect-scan) via solariCtl; results
    // land in `discovered` and are read back via GET /api/discovery.
    $router->post('/api/discovery/scan', static function (): void {
        $body = solari_json_body();
        $cidr = isset($body['cidr']) ? (string) $body['cidr'] : '';
        if (preg_match('#^\d{1,3}(\.\d{1,3}){3}/\d{1,2}$#', $cidr) !== 1) {
            Response::error('bad_request', 'cidr must be IPv4 CIDR, e.g. 192.168.1.0/24.', 400);
        }
        $args = ['cidr' => $cidr];
        if (isset($body['ports'])) {
            $ports = (string) $body['ports'];
            if (preg_match('/^[0-9,]+$/', $ports) === 1) {
                $args['ports'] = $ports;
            }
        }
        $op = Operator::name();
        if ($op !== '') {
            $args['op'] = $op;
        }
        $reply = SolariCtl::call('DISCOVER', $args);
        Response::ok(['cidr' => $cidr, 'found' => isset($reply['found']) ? (int) $reply['found'] : null, 'status' => 'scanned']);
    });

    // POST /api/discovery/{discId}/adopt
    // body (all optional): { displayName, class, poolId, tags:[], notes,
    //   heartbeat:bool (default true), services:[port,...] (default: all discovered) }
    // The C bridge (ADOPT) upserts the asset, provisions a TCP target per selected
    // service + the ICMP heartbeat, and marks the row adopted. PHP only marshals.
    $router->post('/api/discovery/{discId}/adopt', static function (array $p): void {
        $discId = self_disc_id($p['discId']);
        $body   = solari_json_body();

        $args = ['disc' => $discId];
        if (isset($body['displayName']) && is_string($body['displayName']) && $body['displayName'] !== '') {
            $args['name'] = substr($body['displayName'], 0, 120);
        }
        if (isset($body['class']) && in_array($body['class'], ['server','appliance','network','iot','host','other'], true)) {
            $args['class'] = $body['class'];
        }
        if (isset($body['poolId']) && (is_int($body['poolId']) || ctype_digit((string) $body['poolId']))) {
            $args['pool'] = (string) (int) $body['poolId'];
        }
        if (isset($body['notes']) && is_string($body['notes']) && $body['notes'] !== '') {
            $args['notes'] = substr($body['notes'], 0, 240);
        }
        if (isset($body['tags']) && is_array($body['tags'])) {
            $args['tags'] = json_encode(array_values($body['tags']));
        }
        // heartbeat defaults to on
        $args['heartbeat'] = (array_key_exists('heartbeat', $body) && !$body['heartbeat']) ? '0' : '1';
        // explicit service port selection (else the bridge uses all discovered)
        if (isset($body['services']) && is_array($body['services']) && $body['services'] !== []) {
            $ports = [];
            foreach ($body['services'] as $s) {
                $port = is_array($s) ? ($s['port'] ?? null) : $s;
                if (is_int($port) || (is_string($port) && ctype_digit($port))) {
                    $pn = (int) $port;
                    if ($pn > 0 && $pn <= 65535) $ports[] = $pn;
                }
            }
            if ($ports !== []) $args['services'] = implode(',', $ports);
        }
        $op = Operator::name();
        if ($op !== '') $args['op'] = $op;

        $reply = SolariCtl::call('ADOPT', $args);
        Response::ok([
            'discId'  => $discId,
            'assetId' => isset($reply['assetId']) ? (int) $reply['assetId'] : null,
            'status'  => 'adopted',
        ]);
    });

    // POST /api/discovery/{discId}/ignore
    $router->post('/api/discovery/{discId}/ignore', static function (array $p): void {
        $discId = self_disc_id($p['discId']);
        $op = Operator::name();
        $args = ['disc' => $discId];
        if ($op !== '') {
            $args['op'] = $op;
        }
        SolariCtl::call('IGNORE', $args);
        Response::ok(['discId' => $discId, 'status' => 'ignored']);
    });
};

/** Validate a {discId} path segment is a positive integer id. */
function self_disc_id(string $raw): string
{
    if (preg_match('/^\d+$/', $raw) !== 1 || $raw === '0') {
        Response::error('bad_request', 'discId must be a positive integer.', 400);
    }
    return $raw;
}
