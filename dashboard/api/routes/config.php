<?php
declare(strict_types=1);

/**
 * Config & rules (§6, handoff §6 table):
 *   GET  /api/config            read global tolerances/retention/lease/ports/flags
 *   POST /api/config            replace the global config (staged → solariCtl push)
 *   POST /api/rules/{ruleId}    enable/disable/edit one alert rule (→ alertRule)
 *
 * Boundary (§11.1): there is no global-config telemetry table; the global config
 * document is a control-plane object. Reads assemble a stable view from the
 * server's persisted state (serverLease + the nodeConfig epoch floor) layered on
 * the documented defaults. Writes to the GLOBAL document are staged and the
 * actual fleet push is delegated to solariCtl (per the handoff "server config +
 * solariCtl"); PHP never holds CA material. Alert RULES, per the handoff §6
 * mapping ("POST /api/rules/{ruleId} → alertRule"), are the dashboard's own
 * control-plane config and are written directly to alertRule — never the
 * monitoring telemetry tables.
 *
 * GET /api/rules is registered by routes/alerts.php (read layer); the POST is here.
 */

return static function (Router $router): void {

    // --- GET /api/config -------------------------------------------------
    $router->get('/api/config', static function (): void {
        Response::ok(config_current_view());
    });

    // --- POST /api/config ------------------------------------------------
    // Validate the (partial or full) global config, then hand the write to the C
    // bridge (CONFIG_SET) which persists globalConfig + bumps the epoch. PHP does
    // no DB write — it reads the current doc, merges, validates, and marshals.
    $router->post('/api/config', static function (): void {
        $body   = solari_json_body();
        $merged = config_merge(config_current_view(), $body);
        config_validate($merged);                       // emits 400 on bad ranges
        unset($merged['epoch']);                        // derived; not part of the doc

        $args = ['cfg' => json_encode($merged)];
        $op = Operator::name();
        if ($op !== '') $args['op'] = $op;
        $reply = SolariCtl::call('CONFIG_SET', $args);  // C writes; returns epoch=

        Response::ok($merged + ['epoch' => isset($reply['epoch']) ? (int) $reply['epoch'] : null]);
    });

    // --- GET /api/nodes/{id}/config -------------------------------------
    // Per-node desired/applied config from nodeConfig.
    $router->get('/api/nodes/{id}/config', static function (array $p): void {
        $nodeId = config_node_id($p['id']);
        $row = Db::row(
            'SELECT targetEpoch, appliedEpoch, configBlob, lastDirectiveAt, lastResult
               FROM nodeConfig WHERE nodeId = :id',
            [':id' => $nodeId]
        );
        if ($row === null) {
            Response::ok(['nodeId' => $nodeId, 'config' => null, 'targetEpoch' => 0,
                          'appliedEpoch' => 0, 'converged' => true, 'lastResult' => null]);
        }
        $cfg = $row['configBlob'] !== null ? json_decode((string) $row['configBlob'], true) : null;
        Response::ok([
            'nodeId'       => $nodeId,
            'config'       => $cfg,
            'targetEpoch'  => Coerce::int($row['targetEpoch']),
            'appliedEpoch' => Coerce::int($row['appliedEpoch'] ?? 0),
            'converged'    => Coerce::int($row['appliedEpoch'] ?? 0) >= Coerce::int($row['targetEpoch']),
            'lastResult'   => $row['lastResult'],
            'lastDirectiveAt' => Coerce::iso($row['lastDirectiveAt'] ?? null),
        ]);
    });

    // --- POST /api/nodes/{id}/config ------------------------------------
    // body: { config: {...} }  — set a node's desired config and push it.
    $router->post('/api/nodes/{id}/config', static function (array $p): void {
        $nodeId = config_node_id($p['id']);
        if (Db::row('SELECT nodeId FROM node WHERE nodeId = :id', [':id' => $nodeId]) === null) {
            Response::error('not_found', "No node $nodeId", 404);
        }
        $body = solari_json_body();
        $cfg  = $body['config'] ?? null;
        if (!is_array($cfg)) {
            Response::error('bad_request', 'body must be { "config": { ... } }.', 400);
        }

        // Next epoch = max(this node's target, global epoch) + 1, so a node push
        // never lands below the fleet floor.
        $cur    = (int) (Db::scalar('SELECT targetEpoch FROM nodeConfig WHERE nodeId = :id', [':id' => $nodeId]) ?? 0);
        $global = (int) (Db::scalar('SELECT epoch FROM globalConfig WHERE id = 1') ?? 0);
        $epoch  = max($cur, $global) + 1;

        $op   = Operator::name();
        $args = ['node' => (string) $nodeId, 'epoch' => (string) $epoch, 'cfg' => json_encode($cfg)];
        if ($op !== '') $args['op'] = $op;
        SolariCtl::call('PROVISION', $args);            // persists nodeConfig + emits

        Response::ok(['nodeId' => $nodeId, 'config' => $cfg, 'targetEpoch' => $epoch, 'status' => 'pushed']);
    });

    // --- POST /api/rules/{ruleId} ---------------------------------------
    // body: { enabled?:bool, threshold?:number, op?:string, forSeconds?:int,
    //         severity?:string, metric?:string, scope?:string }
    $router->post('/api/rules/{ruleId}', static function (array $p): void {
        if (preg_match('/^\d+$/', $p['ruleId']) !== 1 || $p['ruleId'] === '0') {
            Response::error('bad_request', 'ruleId must be a positive integer.', 400);
        }
        $ruleId = (int) $p['ruleId'];
        $body   = solari_json_body();

        $existing = Db::row('SELECT ruleId FROM alertRule WHERE ruleId = :id', [':id' => $ruleId]);
        if ($existing === null) {
            Response::error('not_found', "No alert rule $ruleId", 404);
        }

        // Validate + marshal whitelisted fields to the bridge (RULE_SET). The
        // write happens in C; PHP validates input and forwards only known fields.
        $args = ['rule' => (string) $ruleId];
        if (array_key_exists('enabled', $body)) {
            $args['enabled'] = $body['enabled'] ? '1' : '0';
        }
        if (array_key_exists('threshold', $body)) {
            if (!is_numeric($body['threshold'])) {
                Response::error('bad_request', 'threshold must be numeric.', 400);
            }
            $args['threshold'] = (string) (float) $body['threshold'];
        }
        if (array_key_exists('forSeconds', $body)) {
            if (!is_int($body['forSeconds']) && !ctype_digit((string) $body['forSeconds'])) {
                Response::error('bad_request', 'forSeconds must be an integer.', 400);
            }
            $args['forSeconds'] = (string) (int) $body['forSeconds'];
        }
        if (array_key_exists('op', $body)) {
            if (!in_array($body['op'], ['gt', 'lt', 'eq', 'transition'], true)) {
                Response::error('bad_request', "op must be one of: gt, lt, eq, transition", 400);
            }
            $args['op'] = (string) $body['op'];
        }
        if (array_key_exists('severity', $body)) {
            if (!in_array($body['severity'], ['info', 'warn', 'crit'], true)) {
                Response::error('bad_request', "severity must be one of: info, warn, crit", 400);
            }
            $args['severity'] = (string) $body['severity'];
        }
        if (array_key_exists('metric', $body)) {
            if (!is_string($body['metric']) || $body['metric'] === '' || strlen($body['metric']) > 64) {
                Response::error('bad_request', 'metric must be a 1–64 char string.', 400);
            }
            $args['metric'] = (string) $body['metric'];
        }
        if (array_key_exists('scope', $body)) {
            if (!in_array($body['scope'], ['host', 'probe'], true)) {
                Response::error('bad_request', "scope must be 'host' or 'probe'", 400);
            }
            $args['scope'] = (string) $body['scope'];
        }

        if (count($args) === 1) {   // only 'rule' present
            Response::error('bad_request', 'No editable rule fields supplied.', 400);
        }

        SolariCtl::call('RULE_SET', $args);   // C performs the UPDATE

        $row = Db::row(
            'SELECT ruleId, scope, metric, op, threshold, forSeconds, severity, enabled
               FROM alertRule WHERE ruleId = :id',
            [':id' => $ruleId]
        );
        Response::ok([
            'ruleId'     => Coerce::int($row['ruleId']),
            'scope'      => $row['scope'],
            'metric'     => $row['metric'],
            'op'         => $row['op'],
            'threshold'  => Coerce::float($row['threshold']),
            'forSeconds' => Coerce::int($row['forSeconds']),
            'severity'   => $row['severity'],
            'enabled'    => Coerce::bool($row['enabled']),
        ]);
    });
};

/* ---- global config view + validation -------------------------------- */

/**
 * Assemble the global config document. There is no global-config table; we layer
 * the documented defaults with the live lease TTL from serverLease so the
 * dashboard reflects the running coordination state. The shape matches the
 * fixture data.jsx exposes so the adapter + ConfigScreen bind unchanged.
 *
 * @return array<string,mixed>
 */
function config_current_view(): array
{
    $cfg = [
        'schedule'  => ['sampleIntervalSec' => 15, 'watchdogIntervalSec' => 5],
        'probe'     => ['roundIntervalSec' => 30, 'probesPerRound' => 5, 'replFactor' => 2, 'gossipIntervalSec' => 20],
        'retention' => ['historyDays' => 90, 'partitionByMonth' => true],
        'lease'     => ['renewSec' => 5, 'ttlSec' => 15],
        'ingest'    => ['ports' => ['ingest' => 7701, 'survey' => 7702, 'pub' => 7703], 'tls' => 'TLS 1.3 · mbedTLS'],
        'ca'        => ['issuer' => 'internal CA', 'enroll' => 'token + CSR', 'certTtlDays' => 365],
        'autoDiscover' => true,
        'autoEnroll'   => false,
    ];

    // Overlay the persisted global config document, if one has been saved. This
    // is what makes edits stick across reads (the document is the control-plane
    // source of truth; defaults are only the initial seed).
    try {
        $row = Db::row('SELECT configBlob, epoch FROM globalConfig WHERE id = 1');
        if ($row !== null && $row['configBlob'] !== null) {
            $saved = json_decode((string) $row['configBlob'], true);
            if (is_array($saved)) {
                $cfg = config_merge($cfg, $saved);
            }
            $cfg['epoch'] = Coerce::int($row['epoch']);
        }
    } catch (Throwable $e) {
        // No globalConfig table/row yet: the documented defaults are a valid view.
    }

    // Reflect the live lease TTL window if a row exists (best-effort; defaults stand).
    try {
        $row = Db::row('SELECT leaseEpoch, expiresAt FROM serverLease WHERE id = 1');
        if ($row !== null) {
            $cfg['lease']['epoch'] = Coerce::int($row['leaseEpoch']);
        }
    } catch (Throwable $e) {
        // No DB / no row: the documented defaults are a valid view.
    }

    return $cfg;
}

/** Validate + return a positive node id, or emit 400. */
function config_node_id($v): string
{
    if (preg_match('/^\d+$/', (string) $v) !== 1 || (string) $v === '0') {
        Response::error('bad_request', 'node id must be a positive integer.', 400);
    }
    return (string) $v;
}

/**
 * Deep-merge an incoming (partial) config over the current view.
 *
 * @param array<string,mixed> $base
 * @param array<string,mixed> $patch
 * @return array<string,mixed>
 */
function config_merge(array $base, array $patch): array
{
    foreach ($patch as $k => $v) {
        if (is_array($v) && isset($base[$k]) && is_array($base[$k])) {
            $base[$k] = config_merge($base[$k], $v);
        } else {
            $base[$k] = $v;
        }
    }
    return $base;
}

/**
 * Validate the numeric tolerances are within the same ranges the ConfigScreen
 * sliders enforce, so a hand-crafted POST cannot push absurd values to the fleet.
 * Emits a 400 envelope on the first violation.
 *
 * @param array<string,mixed> $c
 */
function config_validate(array $c): void
{
    $rng = static function ($v, int $lo, int $hi, string $name): void {
        if (!is_numeric($v) || $v < $lo || $v > $hi) {
            Response::error('bad_request', "$name must be between $lo and $hi.", 400);
        }
    };
    $rng($c['schedule']['sampleIntervalSec']   ?? 15, 5,  120, 'schedule.sampleIntervalSec');
    $rng($c['schedule']['watchdogIntervalSec'] ?? 5,  1,  30,  'schedule.watchdogIntervalSec');
    $rng($c['probe']['roundIntervalSec']       ?? 30, 10, 120, 'probe.roundIntervalSec');
    $rng($c['probe']['probesPerRound']         ?? 5,  1,  20,  'probe.probesPerRound');
    $rng($c['probe']['replFactor']             ?? 2,  1,  5,   'probe.replFactor');
    $rng($c['probe']['gossipIntervalSec']      ?? 20, 5,  60,  'probe.gossipIntervalSec');
    $rng($c['retention']['historyDays']        ?? 90, 7,  365, 'retention.historyDays');
    $rng($c['lease']['renewSec']               ?? 5,  1,  30,  'lease.renewSec');
    $rng($c['lease']['ttlSec']                 ?? 15, 5,  60,  'lease.ttlSec');
}
