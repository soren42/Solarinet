<?php
declare(strict_types=1);

/**
 * Opie — the AI on-call SA — endpoints:
 *   GET /api/opie?status=all|done|investigating   report list, newest first
 *   GET /api/opie/{reportId}                       full report incl. analysis markdown
 *
 * Backed by: opieReport (migration 012). Read-only; opied.py is the sole writer.
 */

return static function (Router $router): void {

    // GET /api/opie?status=all|done|investigating
    $router->get('/api/opie', static function (): void {
        $status = (string) ($_GET['status'] ?? 'all');
        $allowed = ['all', 'done', 'investigating', 'failed'];
        if (!in_array($status, $allowed, true)) {
            Response::error('bad_request',
                "status must be one of: " . implode(',', $allowed), 400);
        }

        $where  = [];
        $params = [];
        if ($status !== 'all') {
            $where[]           = 'status = :status';
            $params[':status'] = $status;
        }
        $whereSql = $where === [] ? '' : ('WHERE ' . implode(' AND ', $where));

        $rows = Db::rows(
            "SELECT reportId, incidentKey, triggerKind, firstEventId, nodeId, hostFqdn,
                    severity, status, summary, model, durationSec, startedAt, finishedAt
               FROM opieReport
               $whereSql
              ORDER BY startedAt DESC
              LIMIT 500",
            $params
        );

        $out = array_map(static function (array $r): array {
            return [
                'reportId'     => Coerce::int($r['reportId']),
                'incidentKey'  => $r['incidentKey'],
                'triggerKind'  => $r['triggerKind'],
                'firstEventId' => Coerce::id($r['firstEventId']),
                'nodeId'       => Coerce::id($r['nodeId']),
                'hostFqdn'     => $r['hostFqdn'],
                'severity'     => $r['severity'],
                'status'       => $r['status'],
                'summary'      => $r['summary'],
                'model'        => $r['model'],
                'durationSec'  => Coerce::int($r['durationSec']),
                'startedAt'    => Coerce::iso($r['startedAt']),
                'finishedAt'   => Coerce::iso($r['finishedAt']),
            ];
        }, $rows);

        Response::ok($out);
    });

    // GET /api/opie/{reportId} — full report including the analysis markdown.
    $router->get('/api/opie/{reportId}', static function (array $p): void {
        if (preg_match('/^\d+$/', $p['reportId']) !== 1 || $p['reportId'] === '0') {
            Response::error('bad_request', 'reportId must be a positive integer.', 400);
        }
        $reportId = (int) $p['reportId'];

        $r = Db::row(
            'SELECT reportId, incidentKey, triggerKind, firstEventId, nodeId, hostFqdn,
                    severity, status, summary, analysis, model, durationSec, startedAt, finishedAt
               FROM opieReport
              WHERE reportId = :id',
            [':id' => $reportId]
        );
        if ($r === null) {
            Response::error('not_found', "No opie report $reportId", 404);
        }

        Response::ok([
            'reportId'     => Coerce::int($r['reportId']),
            'incidentKey'  => $r['incidentKey'],
            'triggerKind'  => $r['triggerKind'],
            'firstEventId' => Coerce::id($r['firstEventId']),
            'nodeId'       => Coerce::id($r['nodeId']),
            'hostFqdn'     => $r['hostFqdn'],
            'severity'     => $r['severity'],
            'status'       => $r['status'],
            'summary'      => $r['summary'],
            'analysis'     => $r['analysis'],
            'model'        => $r['model'],
            'durationSec'  => Coerce::int($r['durationSec']),
            'startedAt'    => Coerce::iso($r['startedAt']),
            'finishedAt'   => Coerce::iso($r['finishedAt']),
        ]);
    });
};
