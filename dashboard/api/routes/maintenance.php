<?php
declare(strict_types=1);

/**
 * Maintenance windows (deploy/maintenance/maint.py's DB, mirrored here):
 *   GET  /api/maintenance?status=active|upcoming|past|all   list windows
 *   POST /api/maintenance                                    schedule a window
 *   POST /api/maintenance/{id}/cancel                        cancel a window
 *
 * Backed by: maintenanceWindow (migration 014, solarinet DB — NOT the SoR).
 * A window suppresses alert-bridge/dead-man's-switch/Opie for its target while
 * in effect; deploy/maintenance/maint.py is the CLI twin of this endpoint and
 * host resolution here matches its resolve_target() exactly (nodeId AND ipAddr
 * are both resolved — a host may be a client node, a probe-only target like
 * benzene, or both).
 *
 * status is DERIVED per row:
 *   'live' (bool) = (NOW() BETWEEN startsAt AND endsAt AND status IN ('scheduled','active')).
 * ?status= filters which rows are returned:
 *   active   = live windows right now
 *   upcoming = scheduled/active windows whose startsAt is still in the future
 *   past     = completed/cancelled windows, or ended windows
 *   all      = no filter (default query below still excludes nothing)
 */

return static function (Router $router): void {

    // GET /api/maintenance?status=active|upcoming|past|all
    $router->get('/api/maintenance', static function (): void {
        $status = (string) ($_GET['status'] ?? 'all');
        if (!in_array($status, ['active', 'upcoming', 'past', 'all'], true)) {
            Response::error('bad_request', "status must be 'active', 'upcoming', 'past', or 'all'", 400);
        }

        $where = [];
        if ($status === 'active') {
            $where[] = "(NOW() BETWEEN startsAt AND endsAt AND status IN ('scheduled','active'))";
        } elseif ($status === 'upcoming') {
            $where[] = "(status IN ('scheduled','active') AND startsAt > NOW())";
        } elseif ($status === 'past') {
            $where[] = "(status IN ('completed','cancelled') OR endsAt < NOW())";
        }
        $whereSql = $where === [] ? '' : ('WHERE ' . implode(' AND ', $where));

        $rows = Db::rows(
            "SELECT windowId, scope, nodeId, hostFqdn, ipAddr, reason, startsAt, endsAt,
                    status, createdBy, createdAt, updatedAt,
                    (NOW() BETWEEN startsAt AND endsAt AND status IN ('scheduled','active')) AS live
               FROM maintenanceWindow
               $whereSql
              ORDER BY startsAt DESC
              LIMIT 500"
        );

        $out = array_map(static function (array $r): array {
            return [
                'windowId'  => Coerce::int($r['windowId']),
                'scope'     => $r['scope'],
                'nodeId'    => Coerce::id($r['nodeId']),
                'hostFqdn'  => $r['hostFqdn'],
                'ipAddr'    => $r['ipAddr'],
                'reason'    => $r['reason'],
                'startsAt'  => Coerce::iso($r['startsAt']),
                'endsAt'    => Coerce::iso($r['endsAt']),
                'status'    => $r['status'],
                'live'      => Coerce::bool($r['live']),
                'createdBy' => $r['createdBy'],
                'createdAt' => Coerce::iso($r['createdAt']),
                'updatedAt' => Coerce::iso($r['updatedAt']),
            ];
        }, $rows);

        Response::ok($out);
    });

    // POST /api/maintenance   body {host?|all:true, reason?, hours?:number | from?+to?:datetime}
    $router->post('/api/maintenance', static function (): void {
        $b = solari_json_body();

        $op = Operator::requireOperator();

        $host = isset($b['host']) ? trim((string) $b['host']) : '';
        $all  = ($b['all'] ?? false) === true;

        if ($all && $host !== '') {
            Response::error('bad_request', 'Give either host or all:true, not both.', 400);
        }
        if (!$all && $host === '') {
            Response::error('bad_request', 'host is required unless all:true is given.', 400);
        }

        // Window bounds: either {hours} from now, or an explicit {from,to} pair.
        $now = new DateTimeImmutable('now');
        if (isset($b['hours']) && $b['hours'] !== '' && $b['hours'] !== null) {
            if (!is_numeric($b['hours']) || (float) $b['hours'] <= 0) {
                Response::error('bad_request', 'hours must be a positive number.', 400);
            }
            $start = $now;
            $end   = $now->add(MaintenanceHelpers::intervalFromHours((float) $b['hours']));
        } elseif (isset($b['from']) && isset($b['to'])) {
            $start = MaintenanceHelpers::parseDt((string) $b['from'], 'from');
            $end   = MaintenanceHelpers::parseDt((string) $b['to'], 'to');
        } else {
            Response::error('bad_request', 'Give hours, or both from and to.', 400);
        }

        if ($end <= $start) {
            Response::error('bad_request', 'end must be after start.', 400);
        }

        $scope   = $all ? 'all' : 'node';
        $nodeId  = null;
        $hostFqdn = null;
        $ipAddr  = null;

        if (!$all) {
            [$nodeId, $hostFqdn, $ipAddr] = MaintenanceHelpers::resolveTarget($host);
            if ($nodeId === null && $ipAddr === null) {
                Response::error('bad_request',
                    "Can't resolve '$host' to a node or an IP — give a host that has reported, " .
                    'or one that resolves in DNS.', 400);
            }
        }

        $reason = isset($b['reason']) && $b['reason'] !== '' ? substr((string) $b['reason'], 0, 255) : null;

        Db::exec(
            'INSERT INTO maintenanceWindow (scope, nodeId, hostFqdn, ipAddr, reason, startsAt, endsAt, status, createdBy)
             VALUES (:scope, :nodeId, :hostFqdn, :ipAddr, :reason, :startsAt, :endsAt, \'scheduled\', :createdBy)',
            [
                ':scope'     => $scope,
                ':nodeId'    => $nodeId,
                ':hostFqdn'  => $hostFqdn,
                ':ipAddr'    => $ipAddr,
                ':reason'    => $reason,
                ':startsAt'  => $start->format('Y-m-d H:i:s'),
                ':endsAt'    => $end->format('Y-m-d H:i:s'),
                ':createdBy' => $op,
            ]
        );
        $windowId = (int) Db::pdo()->lastInsertId();

        Response::ok([
            'windowId'  => $windowId,
            'scope'     => $scope,
            'nodeId'    => Coerce::id($nodeId),
            'hostFqdn'  => $hostFqdn,
            'ipAddr'    => $ipAddr,
            'reason'    => $reason,
            'startsAt'  => Coerce::iso($start->format('Y-m-d H:i:s')),
            'endsAt'    => Coerce::iso($end->format('Y-m-d H:i:s')),
            'status'    => 'scheduled',
            'createdBy' => $op,
        ], 201);
    });

    // POST /api/maintenance/{id}/cancel
    $router->post('/api/maintenance/{id}/cancel', static function (array $p): void {
        if (preg_match('/^\d+$/', $p['id']) !== 1 || $p['id'] === '0') {
            Response::error('bad_request', 'id must be a positive integer.', 400);
        }
        $windowId = (int) $p['id'];

        Operator::requireOperator();

        $count = Db::exec(
            "UPDATE maintenanceWindow SET status = 'cancelled'
              WHERE windowId = :id AND status IN ('scheduled','active')",
            [':id' => $windowId]
        );

        if ($count === 0) {
            $existing = Db::row('SELECT windowId FROM maintenanceWindow WHERE windowId = :id', [':id' => $windowId]);
            if ($existing === null) {
                Response::error('not_found', "No maintenance window $windowId", 404);
            }
            Response::error('bad_request', "Window $windowId is not scheduled/active.", 409);
        }

        Response::ok(['windowId' => $windowId, 'status' => 'cancelled']);
    });
};

/**
 * Local helpers for host resolution and time parsing, mirroring
 * deploy/maintenance/maint.py's resolve_target()/parse_dt() exactly so the CLI
 * and the dashboard API agree on what a window targets.
 */
final class MaintenanceHelpers
{
    /**
     * Resolve a host to (nodeId|null, fqdn, ip|null) — same shape/precedence as
     * maint.py resolve_target(): a host may be a client node (matched by
     * hostFqdn or a hostFqdn prefix), a probe-only target (resolved via DNS,
     * optionally suffixed with .akoria.net), or both.
     *
     * @return array{0:?string,1:string,2:?string}
     */
    public static function resolveTarget(string $host): array
    {
        $node = Db::row(
            'SELECT nodeId, hostFqdn FROM node
              WHERE hostFqdn = :h OR hostFqdn LIKE :hp
              ORDER BY (hostFqdn = :h2) DESC
              LIMIT 1',
            [':h' => $host, ':hp' => $host . '.%', ':h2' => $host]
        );

        $ip = null;
        $candidates = [$host];
        if (!str_ends_with($host, '.akoria.net')) {
            $candidates[] = $host . '.akoria.net';
        }
        foreach ($candidates as $name) {
            $resolved = gethostbyname($name);
            if ($resolved !== $name) {
                $ip = $resolved;
                break;
            }
        }

        $fqdn = ($node['hostFqdn'] ?? null) ?: $host;
        $nodeId = $node['nodeId'] ?? null;

        return [$nodeId === null ? null : (string) $nodeId, $fqdn, $ip];
    }

    /** Parse a 'YYYY-MM-DD HH:MM[:SS]' or ISO 'YYYY-MM-DDTHH:MM[:SS]' datetime. */
    public static function parseDt(string $s, string $field): DateTimeImmutable
    {
        foreach (['Y-m-d H:i', 'Y-m-d H:i:s', 'Y-m-d\TH:i', 'Y-m-d\TH:i:s'] as $fmt) {
            $dt = DateTimeImmutable::createFromFormat($fmt, $s);
            if ($dt !== false) {
                return $dt;
            }
        }
        Response::error('bad_request', "Invalid $field datetime '$s' (use 'YYYY-MM-DD HH:MM').", 400);
    }

    /** hours (fractional allowed) -> DateInterval, via whole seconds. */
    public static function intervalFromHours(float $hours): DateInterval
    {
        $seconds = (int) round($hours * 3600);
        return new DateInterval('PT' . max(1, $seconds) . 'S');
    }
}
