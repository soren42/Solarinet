<?php
declare(strict_types=1);

/**
 * Lifecycle control mutations (§6, §7.3, §11) — all via solariCtl:
 *   POST /api/control/provision      bring a node up (PROVISION)
 *   POST /api/control/decommission   retire + wipe (DECOMMISSION, double-confirmed)
 *   POST /api/control/survey         fleet survey now (SURVEY)
 *
 * PHP holds no CA material and writes no monitoring tables. provision/survey are
 * building/operational; decommission is irreversible and therefore demands an
 * operator role, an explicit confirm:true, and a non-empty wipeScope[] — and is
 * driven through the bridge's two-step confirm-token handshake.
 */

return static function (Router $router): void {

    // --- POST /api/control/provision -------------------------------------
    // body: { nodeId, buildId?, configEpoch?, configBlob? }
    $router->post('/api/control/provision', static function (): void {
        $body   = solari_json_body();
        $nodeId = ctrl_pos_int($body['nodeId'] ?? null, 'nodeId');

        $args = ['node' => $nodeId];
        if (isset($body['buildId']) && ctrl_is_int($body['buildId'])) {
            $args['build'] = (string) $body['buildId'];
        }
        if (isset($body['configEpoch']) && ctrl_is_int($body['configEpoch'])) {
            $args['epoch'] = (string) $body['configEpoch'];
        }
        if (isset($body['configBlob'])) {
            $cfg = is_string($body['configBlob'])
                 ? $body['configBlob']
                 : json_encode($body['configBlob']);
            if ($cfg !== '' && $cfg !== false) {
                $args['cfg'] = $cfg;   // SolariCtl URL-encodes blobs for the wire
            }
        }
        $op = Operator::name();
        if ($op !== '') {
            $args['op'] = $op;
        }

        SolariCtl::call('PROVISION', $args);
        Response::ok(['nodeId' => $nodeId, 'status' => 'provisioning']);
    });

    // --- POST /api/control/decommission ----------------------------------
    // body: { nodeId, confirm:true, wipeScope:["config","certs","spool","unit","data"] }
    //
    // Two distinct safety gates:
    //   (a) PHP: operator role + confirm:true + non-empty wipeScope[].
    //   (b) Bridge: a one-time confirm token (first DECOMMISSION call returns it;
    //       we immediately re-call with confirm=<token> to finalize the retire).
    $router->post('/api/control/decommission', static function (): void {
        $body   = solari_json_body();
        $op     = Operator::requireOperator();
        $nodeId = ctrl_pos_int($body['nodeId'] ?? null, 'nodeId');

        if (($body['confirm'] ?? null) !== true) {
            Response::error('confirm_required',
                'Decommission is irreversible; resend with {"confirm":true}.', 409);
        }
        $scope = ctrl_wipe_scope_to_hex($body['wipeScope'] ?? null);  // emits 400 if bad

        // Step 1: issue the bridge's one-time confirm token (node not yet retired).
        $issue = SolariCtl::call('DECOMMISSION', [
            'node'  => $nodeId,
            'scope' => $scope,
            'op'    => $op,
        ]);
        $token = $issue['confirm'] ?? '';
        if ($token === '') {
            Response::error('control_error',
                'Operator bridge did not return a confirm token.', 502);
        }

        // Step 2: finalize with the echoed token (double-confirmed). The bridge
        // retires the node and emits SCP_MSG_DECOMMISSION.
        SolariCtl::call('DECOMMISSION', [
            'node'    => $nodeId,
            'scope'   => $scope,
            'op'      => $op,
            'confirm' => $token,
        ]);

        Response::ok([
            'nodeId'    => $nodeId,
            'status'    => 'retired',
            'wipeScope' => array_values((array) $body['wipeScope']),
            'decidedBy' => $op,
        ]);
    });

    // --- POST /api/control/survey ----------------------------------------
    // body: { scope?: "all" }   (the bridge broadcasts SCP_MSG_SURVEY)
    $router->post('/api/control/survey', static function (): void {
        // (scope is informational for now; the bridge SURVEY is a broadcast.)
        solari_json_body();
        $op = Operator::name();
        $args = [];
        if ($op !== '') {
            $args['op'] = $op;
        }
        $fields = SolariCtl::call('SURVEY', $args);
        Response::ok(['survey' => $fields['survey'] ?? 'sent']);
    });

    // --- POST /api/control/deploy ----------------------------------------
    // body: { host:"[user@]host", server?, arch?, fqdn? }
    // Deploys the client agent to a remote host via the C bridge (which detaches
    // remote-deploy.sh and logs to run/deploy-<host>.log). Privileged: the deploy
    // installs software + signs a cert. Returns immediately; poll the GET below.
    $router->post('/api/control/deploy', static function (): void {
        $b   = solari_json_body();
        $op  = Operator::requireOperator();              // 403 unless operator/admin
        $host = (string) ($b['host'] ?? '');
        if (preg_match('/^[A-Za-z0-9._@-]{1,120}$/', $host) !== 1) {
            Response::error('bad_request', 'host must be "[user@]hostname".', 400);
        }
        $args = ['host' => $host, 'op' => $op];
        if (isset($b['server']) && preg_match('#^tls\+tcp://[A-Za-z0-9._:-]+$#', $b['server']) === 1) {
            $args['server'] = $b['server'];
        }
        if (isset($b['arch']) && in_array($b['arch'], ['arm64', 'arm32', 'amd64', 'x86_64'], true)) {
            $args['arch'] = $b['arch'];
        }
        if (isset($b['fqdn']) && preg_match('/^[A-Za-z0-9._-]{1,160}$/', (string) $b['fqdn']) === 1) {
            $args['fqdn'] = $b['fqdn'];
        }
        $reply = SolariCtl::call('DEPLOY', $args);
        Response::ok(['host' => $host, 'status' => 'deploying', 'log' => $reply['log'] ?? null]);
    });

    // --- GET /api/control/deploy?host= -----------------------------------
    // Tail the deploy log for a host (read-only; the bridge owns the deploy).
    $router->get('/api/control/deploy', static function (): void {
        $host = (string) ($_GET['host'] ?? '');
        if (preg_match('/^[A-Za-z0-9._@-]{1,120}$/', $host) !== 1) {
            Response::error('bad_request', 'host query param required.', 400);
        }
        $sanit  = preg_replace('/[^A-Za-z0-9._-]/', '_', $host);
        $sock   = getenv('SOLARI_CTL_SOCK') ?: '/run/solari/solariCtl.sock';
        $logf   = dirname($sock) . '/deploy-' . $sanit . '.log';
        $log    = is_readable($logf) ? (string) file_get_contents($logf) : '';
        $done   = strpos($log, '[deploy] done.') !== false || strpos($log, 'node should appear') !== false;
        $failed = strpos($log, '[deploy][error]') !== false;
        Response::ok([
            'host'    => $host,
            'log'     => $log,
            'running' => $log !== '' && !$done && !$failed,
            'done'    => $done,
            'failed'  => $failed,
        ]);
    });
};

/* ---- shared validators / mappers ------------------------------------- */

/** True if a value is an integer-ish (int or all-digit string). */
function ctrl_is_int($v): bool
{
    return is_int($v) || (is_string($v) && preg_match('/^\d+$/', $v) === 1);
}

/** Require a positive integer id; emit 400 on failure. */
function ctrl_pos_int($v, string $name): string
{
    if (!ctrl_is_int($v) || (string) $v === '0') {
        Response::error('bad_request', "$name must be a positive integer.", 400);
    }
    return (string) $v;
}

/**
 * Map the wipeScope[] string set onto the 5-bit hex scope the C DECOMMISSION
 * verb expects (it requires (scope & 0x1F) != 0). The bit layout mirrors the §4
 * TLV_LIFE_WIPE_SCOPE bitfield (config|certs|spool|logs|unit) so this tier and
 * solariCtl/serverProvisionDecommission agree byte-for-byte:
 *
 *   0x01 config   0x02 certs   0x04 spool   0x08 logs   0x10 unit (service files)
 *
 * Emits a 400 envelope on an empty/unknown scope so a stray "wipe everything"
 * cannot slip through under-specified.
 */
function ctrl_wipe_scope_to_hex($scope): string
{
    static $bits = [
        'config' => 0x01,
        'certs'  => 0x02,
        'spool'  => 0x04,
        'logs'   => 0x08,
        'unit'   => 0x10,
    ];
    if (!is_array($scope) || $scope === []) {
        Response::error('bad_request',
            'wipeScope[] must list at least one of: ' . implode(', ', array_keys($bits)), 400);
    }
    $mask = 0;
    foreach ($scope as $s) {
        $key = is_string($s) ? strtolower($s) : '';
        if (!isset($bits[$key])) {
            Response::error('bad_request',
                "Unknown wipeScope '$s'; allowed: " . implode(', ', array_keys($bits)), 400);
        }
        $mask |= $bits[$key];
    }
    return '0x' . dechex($mask);
}
