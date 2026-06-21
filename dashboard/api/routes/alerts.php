<?php
declare(strict_types=1);

/**
 * Alerts & rules endpoints:
 *   GET /api/alerts?status=active|all   alert events + rule + node context
 *   GET /api/rules                      alert rule list (§6: enable/disable POST
 *                                       is added by the follow-up mutation layer)
 *
 * Backed by: alertEvent, alertRule, node.
 */

return static function (Router $router): void {

    // GET /api/alerts?status=active|all  (active == not yet cleared)
    $router->get('/api/alerts', static function (): void {
        $status = (string) ($_GET['status'] ?? 'active');

        $where  = [];
        $params = [];
        if ($status === 'active') {
            $where[] = 'e.clearedAt IS NULL';
        } elseif ($status !== 'all') {
            Response::error('bad_request', "status must be 'active' or 'all'", 400);
        }
        $whereSql = $where === [] ? '' : ('WHERE ' . implode(' AND ', $where));

        $rows = Db::rows(
            "SELECT e.eventId, e.ruleId, e.nodeId, e.targetId, e.firedAt, e.clearedAt,
                    e.severity, e.detail,
                    r.scope, r.metric, r.op, r.threshold,
                    n.hostFqdn, n.role
               FROM alertEvent e
               LEFT JOIN alertRule r ON r.ruleId = e.ruleId
               LEFT JOIN node n      ON n.nodeId = e.nodeId
               $whereSql
              ORDER BY e.firedAt DESC
              LIMIT 500",
            $params
        );

        $out = array_map(static function (array $r): array {
            return [
                'eventId'   => Coerce::id($r['eventId']),
                'ruleId'    => Coerce::int($r['ruleId']),
                'nodeId'    => Coerce::id($r['nodeId']),
                'hostFqdn'  => $r['hostFqdn'],
                'role'      => $r['role'],
                'targetId'  => $r['targetId'],
                'firedAt'   => Coerce::iso($r['firedAt']),
                'clearedAt' => Coerce::iso($r['clearedAt']),
                'severity'  => $r['severity'],
                'detail'    => $r['detail'],
                'scope'     => $r['scope'],
                'metric'    => $r['metric'],
                'op'        => $r['op'],
                'threshold' => Coerce::float($r['threshold']),
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
};
