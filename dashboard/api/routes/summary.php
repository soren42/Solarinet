<?php
declare(strict_types=1);

/**
 * Summary endpoint (§6):
 *   GET /api/summary — one cheap call for the controller header tiles:
 *   counts of systems/hosts/servers/monitors/probes/applications, plus the
 *   server lease (failover) state. Raw counts only; no display formatting.
 *
 * Backed by: node (role/state), probeTarget, procCurrent, alertEvent, serverLease.
 */

return static function (Router $router): void {

    $router->get('/api/summary', static function (): void {
        // role counts + total + state rollup in one pass.
        $roleRows = Db::rows(
            "SELECT role,
                    COUNT(*)              AS total,
                    SUM(state='up')       AS up,
                    SUM(state='degraded') AS degraded,
                    SUM(state='down')     AS down,
                    SUM(state IN('unknown')) AS unknown,
                    SUM(state='retired')  AS retired
               FROM node
              GROUP BY role"
        );
        $byRole = ['client' => 0, 'monitor' => 0, 'server' => 0];
        $stateRoll = ['up' => 0, 'degraded' => 0, 'down' => 0, 'unknown' => 0, 'retired' => 0];
        $totalNodes = 0;
        foreach ($roleRows as $r) {
            $role = (string) $r['role'];
            $byRole[$role] = Coerce::int($r['total']);
            $totalNodes  += Coerce::int($r['total']);
            $stateRoll['up']       += Coerce::int($r['up']);
            $stateRoll['degraded'] += Coerce::int($r['degraded']);
            $stateRoll['down']     += Coerce::int($r['down']);
            $stateRoll['unknown']  += Coerce::int($r['unknown']);
            $stateRoll['retired']  += Coerce::int($r['retired']);
        }

        $probeCount = (int) (Db::scalar('SELECT COUNT(*) FROM probeTarget') ?? 0);
        // "applications" == distinct watched processes across the fleet.
        $appCount = (int) (Db::scalar('SELECT COUNT(DISTINCT procName) FROM procCurrent') ?? 0);
        $activeAlerts = (int) (Db::scalar('SELECT COUNT(*) FROM alertEvent WHERE clearedAt IS NULL') ?? 0);
        $segCount = (int) (Db::scalar('SELECT COUNT(*) FROM segment') ?? 0);

        $lease = Db::row(
            'SELECT leaseHolder, leaseEpoch, expiresAt FROM serverLease WHERE id = 1'
        );

        Response::ok([
            'counts' => [
                'nodes'        => $totalNodes,
                'clients'      => $byRole['client'],
                'monitors'     => $byRole['monitor'],
                'servers'      => $byRole['server'],
                'probes'       => $probeCount,
                'applications' => $appCount,
                'segments'     => $segCount,
                'activeAlerts' => $activeAlerts,
            ],
            'stateRoll' => $stateRoll,
            'lease' => $lease === null ? null : [
                'leaseHolder' => Coerce::id($lease['leaseHolder']),
                'leaseEpoch'  => Coerce::id($lease['leaseEpoch']),
                'expiresAt'   => Coerce::iso($lease['expiresAt']),
            ],
        ]);
    });
};
