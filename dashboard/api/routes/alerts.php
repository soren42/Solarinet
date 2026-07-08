<?php
declare(strict_types=1);

/**
 * Alerts & rules endpoints:
 *   GET  /api/alerts?status=active|history|all   alert events + rule/node context
 *   GET  /api/rules                               alert rule list (§6: enable/disable POST
 *                                                  is added by the follow-up mutation layer)
 *   POST /api/alerts/ack                          ack a single alert  {eventId}
 *   POST /api/alerts/ack-all                      ack every currently-ACTIVE alert
 *
 * Backed by: alertEvent, alertRule, node, opieReport.
 *
 * Lifecycle (migration 012): ack is a dashboard-layer concept — the C engine
 * only INSERTs fire/clear rows on alertEvent, so ackedAt/ackedBy are written
 * directly here via Db::exec, same as the existing alertRule control-plane
 * writes (never through solariCtl, never on monitoring telemetry tables).
 *
 * status is DERIVED per row:
 *   'acked'   if ackedAt is not null
 *   'cleared' if clearedAt is not null (and not acked)
 *   'active'  otherwise
 * ACTIVE set (status=active, the default) = ackedAt IS NULL AND
 *   (clearedAt IS NULL OR clearedAt > NOW() - INTERVAL 60 MINUTE).
 * HISTORY set (status=history) = everything else (acked, or cleared >60min ago).
 * 'all' = no filter.
 */

return static function (Router $router): void {

    // GET /api/alerts?status=active|history|all
    $router->get('/api/alerts', static function (): void {
        $status = (string) ($_GET['status'] ?? 'active');
        if (!in_array($status, ['active', 'history', 'all'], true)) {
            Response::error('bad_request', "status must be 'active', 'history', or 'all'", 400);
        }

        $where = [];
        if ($status === 'active') {
            $where[] = '(e.ackedAt IS NULL AND (e.clearedAt IS NULL OR e.clearedAt > NOW() - INTERVAL 60 MINUTE))';
        } elseif ($status === 'history') {
            $where[] = '(e.ackedAt IS NOT NULL OR (e.clearedAt IS NOT NULL AND e.clearedAt <= NOW() - INTERVAL 60 MINUTE))';
        }
        $whereSql = $where === [] ? '' : ('WHERE ' . implode(' AND ', $where));

        $rows = Db::rows(
            "SELECT e.eventId, e.ruleId, e.nodeId, e.targetId, e.firedAt, e.clearedAt,
                    e.ackedAt, e.ackedBy, e.severity, e.detail,
                    r.scope, r.metric, r.op, r.threshold,
                    n.hostFqdn, n.role,
                    o.reportId AS opieReportId
               FROM alertEvent e
               LEFT JOIN alertRule r  ON r.ruleId = e.ruleId
               LEFT JOIN node n       ON n.nodeId = e.nodeId
               LEFT JOIN opieReport o ON o.firstEventId = e.eventId
               $whereSql
              ORDER BY e.firedAt DESC
              LIMIT 500"
        );

        $out = array_map(static function (array $r): array {
            $ackedAt   = $r['ackedAt'] ?? null;
            $clearedAt = $r['clearedAt'] ?? null;
            if ($ackedAt !== null) {
                $lifecycleStatus = 'acked';
            } elseif ($clearedAt !== null) {
                $lifecycleStatus = 'cleared';
            } else {
                $lifecycleStatus = 'active';
            }

            return [
                'eventId'      => Coerce::id($r['eventId']),
                'ruleId'       => Coerce::int($r['ruleId']),
                'nodeId'       => Coerce::id($r['nodeId']),
                'hostFqdn'     => $r['hostFqdn'],
                'role'         => $r['role'],
                'targetId'     => $r['targetId'],
                'firedAt'      => Coerce::iso($r['firedAt']),
                'clearedAt'    => Coerce::iso($clearedAt),
                'ackedAt'      => Coerce::iso($ackedAt),
                'ackedBy'      => $r['ackedBy'],
                'status'       => $lifecycleStatus,
                'severity'     => $r['severity'],
                'detail'       => $r['detail'],
                'scope'        => $r['scope'],
                'metric'       => $r['metric'],
                'op'           => $r['op'],
                'threshold'    => Coerce::float($r['threshold']),
                'opieReportId' => Coerce::int($r['opieReportId']),
            ];
        }, $rows);

        Response::ok($out);
    });

    // GET /api/rules
    $router->get('/api/rules', static function (): void {
        $rows = Db::rows(
            'SELECT ruleId, scope, metric, op, threshold, forSeconds, severity, enabled
               FROM alertRule
              ORDER BY ruleId'
        );
        $out = array_map(static function (array $r): array {
            return [
                'ruleId'     => Coerce::int($r['ruleId']),
                'scope'      => $r['scope'],
                'metric'     => $r['metric'],
                'op'         => $r['op'],
                'threshold'  => Coerce::float($r['threshold']),
                'forSeconds' => Coerce::int($r['forSeconds']),
                'severity'   => $r['severity'],
                'enabled'    => Coerce::bool($r['enabled']),
            ];
        }, $rows);
        Response::ok($out);
    });

    // POST /api/alerts/ack   body {eventId}
    $router->post('/api/alerts/ack', static function (): void {
        $body = solari_json_body();
        if (!isset($body['eventId'])
            || preg_match('/^\d+$/', (string) $body['eventId']) !== 1
            || (string) $body['eventId'] === '0'
        ) {
            Response::error('bad_request', 'eventId is required and must be a positive integer.', 400);
        }
        $eventId = (int) $body['eventId'];

        if (Db::row('SELECT eventId FROM alertEvent WHERE eventId = :id', [':id' => $eventId]) === null) {
            Response::error('not_found', "No alert event $eventId", 404);
        }

        $op = Operator::requireOperator();
        if ($op === '') {
            $op = 'operator';
        }

        Db::exec(
            'UPDATE alertEvent SET ackedAt = NOW(), ackedBy = :by WHERE eventId = :id',
            [':by' => $op, ':id' => $eventId]
        );

        Response::ok(['eventId' => $eventId, 'ackedBy' => $op]);
    });

    // POST /api/alerts/ack-all   ack every currently-ACTIVE alert
    $router->post('/api/alerts/ack-all', static function (): void {
        $op = Operator::requireOperator();
        if ($op === '') {
            $op = 'operator';
        }

        $count = Db::exec(
            'UPDATE alertEvent
                SET ackedAt = NOW(), ackedBy = :by
              WHERE ackedAt IS NULL
                AND (clearedAt IS NULL OR clearedAt > NOW() - INTERVAL 60 MINUTE)',
            [':by' => $op]
        );

        Response::ok(['ackedCount' => $count, 'ackedBy' => $op]);
    });
};
