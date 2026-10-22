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

    require_once __DIR__ . '/../lib/DiscoveryScanInput.php';

    // POST /api/discovery/scan  body: { cidr: "10.0.0.0/24", ports?: "22,80,443" }
    // Validates a bounded IPv4 range and strict TCP port list before the control call.
    // Enqueues an operator-authenticated durable scan. The server queue request
    // is only used to cross the PHP/host privilege boundary; the returned scanId
    // is the durable discoveryScanJob identity.
    $router->post('/api/discovery/scan', static function (): void {
        $op = Operator::requireOperator();
        $body = solari_json_body();
        $cidr = isset($body['cidr']) ? (string) $body['cidr'] : '';
        if (!DiscoveryScanInput::validCidr($cidr)) {
            Response::error('bad_request', 'cidr must be a valid IPv4 range of at most 4096 hosts.', 400);
        }
        $args = ['cidr' => $cidr];
        if (array_key_exists('ports', $body)) {
            if (!is_string($body['ports'])) {
                Response::error('bad_request', 'ports must be a comma-separated list of TCP ports.', 400);
            }
            $ports = DiscoveryScanInput::ports($body['ports']);
            if ($ports === null) {
                Response::error('bad_request', 'ports must contain 1–64 unique TCP ports from 1 to 65535.', 400);
            }
            if ($ports !== []) {
                $args['ports'] = implode(',', $ports);
            }
        }
        $idem = $_SERVER['HTTP_IDEMPOTENCY_KEY'] ?? '';
        if ($idem !== '' && preg_match('/^[A-Za-z0-9._-]{8,128}$/', $idem) !== 1) {
            Response::error('bad_request', 'Idempotency-Key must be 8-128 chars of [A-Za-z0-9._-].', 400);
        }
        $reply = SolariCtl::callQueued('DISCOVER', $args, $op,
            $idem !== '' ? $idem : null, 4.0);
        if (($reply['status'] ?? '') === 'queued' && isset($reply['request'])) {
            Response::ok(['cidr' => $cidr, 'status' => 'admission_pending',
                          'requestId' => (int) $reply['request']], 202);
        }
        if (!isset($reply['scan']) || !ctype_digit((string) $reply['scan'])) {
            Response::error('control_error', 'Operator bridge returned no discovery scan id.', 502);
        }
        Response::ok(['cidr' => $cidr, 'scanId' => (int) $reply['scan'],
                      'hostsTotal' => isset($reply['hosts']) ? (int) $reply['hosts'] : null,
                      'status' => 'queued'], 202);
    });

    $router->get('/api/discovery/scan-requests/{requestId}', static function (array $p): void {
        $requestId = self_scan_id($p['requestId'], 'requestId');
        $op = Operator::requireOperator();
        $reply = SolariCtl::poll((int) $requestId, $op);
        $state = (string) ($reply['state'] ?? '');
        $out = ['requestId' => (int) $requestId, 'state' => $state];
        if ($state === 'done') {
            $raw = (string) ($reply['result'] ?? '');
            if (preg_match('/^OK scan=([1-9][0-9]*) status=queued hosts=([0-9]+)$/', $raw, $m) !== 1) {
                Response::error('control_error', 'Discovery scan admission returned an invalid result.', 502);
            }
            $out['scanId'] = (int) $m[1];
            $out['hostsTotal'] = (int) $m[2];
        } elseif ($state === 'failed') {
            $out['error'] = (string) ($reply['error'] ?? 'Scan admission failed.');
        }
        Response::ok($out);
    }, ['auth' => 'operator']);

    // Recover recent durable scans when browser session storage is lost or the
    // operator returns from another tab/device. Keep the result scoped to the
    // authenticated requester and bound it to the ten newest jobs.
    $router->get('/api/discovery/scan-jobs', static function (): void {
        $op = Operator::requireOperator();
        $rows = Db::rows(
            'SELECT scanId, cidr, ports, status, hostsTotal, hostsDone, hostsFound, errorText, createdAt '
            . 'FROM discoveryScanJob WHERE requestedBy = :requestedBy ORDER BY scanId DESC LIMIT 10',
            [':requestedBy' => $op]
        );
        $jobs = array_map(static function (array $row): array {
            return [
                'scanId' => (int) $row['scanId'],
                'cidr' => (string) $row['cidr'],
                'ports' => (string) $row['ports'],
                'status' => (string) $row['status'],
                'hostsTotal' => (int) $row['hostsTotal'],
                'hostsDone' => (int) $row['hostsDone'],
                'hostsFound' => (int) $row['hostsFound'],
                'error' => (string) ($row['errorText'] ?? ''),
                'createdAt' => (string) $row['createdAt'],
            ];
        }, $rows);
        Response::ok($jobs);
    }, ['auth' => 'operator']);

    $router->get('/api/discovery/scans/{scanId}', static function (array $p): void {
        $scanId = self_scan_id($p['scanId'], 'scanId');
        $op = Operator::requireOperator();
        $reply = SolariCtl::call('DISCOVER_STATUS', ['scan' => $scanId, 'op' => $op]);
        Response::ok([
            'scanId' => (int) ($reply['scan'] ?? $scanId),
            'status' => (string) ($reply['status'] ?? 'unknown'),
            'hostsDone' => (int) ($reply['done'] ?? 0),
            'hostsTotal' => (int) ($reply['total'] ?? 0),
            'hostsFound' => (int) ($reply['found'] ?? 0),
            'errorCode' => (int) ($reply['errorCode'] ?? 0),
            'error' => (string) ($reply['error'] ?? ''),
        ]);
    }, ['auth' => 'operator']);

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
            $pid = (int) $body['poolId'];
            if ($pid !== 0 && Db::row('SELECT poolId FROM pool WHERE poolId = :i', [':i' => $pid]) === null) {
                Response::error('bad_request', "No pool $pid", 400);
            }
            if ($pid !== 0) $args['pool'] = (string) $pid;
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
        // mirror the adopt into the SoR immediately (fail-soft; sor_reconcile is the net)
        $disc = Db::row('SELECT ip, host, mdnsName FROM discovered WHERE discId = :i', [':i' => $discId]);
        if ($disc !== null) {
            Sor::upsertHost((string) $disc['ip'], $args['name'] ?? ($disc['host'] ?: ($disc['mdnsName'] ?: null)));
        }
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

function self_scan_id(string $raw, string $name): string
{
    if (preg_match('/^[1-9][0-9]{0,18}$/', $raw) !== 1) {
        Response::error('bad_request', "$name must be a positive integer.", 400);
    }
    return $raw;
}
