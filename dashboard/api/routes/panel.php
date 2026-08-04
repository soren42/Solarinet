<?php
declare(strict_types=1);

/**
 * Panel endpoint — GET /api/panel.
 *
 * Purpose: provide one small, panel-specific fleet snapshot suitable for a
 * five-second poll.  Every SELECT runs inside one read-only consistent
 * snapshot, so pools, systems, alerts, and telemetry describe the same view.
 *
 * Input: authenticated viewer session (the front controller is the shared
 * authentication gate); current-state monitoring and dashboard pool tables.
 * Output: Status Panel CONTRACT §3 + §9 JSON payload via Response::ok().
 */

/**
 * Convert a display string to the panel's deliberately small ASCII alphabet.
 *
 * Input: arbitrary database/display text and the destination wire width.
 * Output: uppercase [A-Z0-9 .:-] text, truncated with a trailing dot.
 */
function panelText($value, int $width): string
{
    $text = strtoupper((string) $value);
    $text = preg_replace('/[^A-Z0-9 .:-]/', ' ', $text) ?? '';
    $text = trim(preg_replace('/\\s+/', ' ', $text) ?? '');
    if (strlen($text) <= $width) {
        return $text;
    }
    return substr($text, 0, max(0, $width - 1)) . '.';
}

/**
 * Calculate a system load percentage from the client-report JSON array.
 *
 * Input: hostCurrent.cpuLoadMilli (per-core thousandths of a percent scale).
 * Output: integer 0..100; malformed/missing telemetry yields zero.
 */
function panelLoadPct($cpuLoadMilli): int
{
    $loads = is_string($cpuLoadMilli) ? json_decode($cpuLoadMilli, true) : $cpuLoadMilli;
    if (!is_array($loads) || $loads === []) {
        return 0;
    }
    $sum = 0.0;
    $count = 0;
    foreach ($loads as $load) {
        if (is_numeric($load)) {
            $sum += (float) $load;
            $count++;
        }
    }
    return $count === 0 ? 0 : max(0, min(100, (int) round(($sum / $count) / 10.0)));
}

/**
 * Sum host interface directions without depending on a database JSON version.
 *
 * Input: hostCurrent.ifaces, encoded by serverDb.c as rxKbps/txKbps objects.
 * Output: [rxKbps, txKbps], clamped to the unsigned 32-bit wire range.
 *
 * @return array{0:int,1:int}
 */
function panelInterfaceKbps($ifaces): array
{
    $entries = is_string($ifaces) ? json_decode($ifaces, true) : $ifaces;
    $rx = 0;
    $tx = 0;
    if (is_array($entries)) {
        foreach ($entries as $entry) {
            if (!is_array($entry)) {
                continue;
            }
            $rx += max(0, (int) ($entry['rxKbps'] ?? 0));
            $tx += max(0, (int) ($entry['txKbps'] ?? 0));
        }
    }
    return [min(4294967295, $rx), min(4294967295, $tx)];
}

/**
 * Derive a panel tier where the current pool schema has no tier metadata.
 *
 * Input: linked node role and asset class.
 * Output: monitor=0, server/server-class=1, standby=2, clients/unknown=3.
 *
 * This intentionally implements the contract fallback: monitor/server roles
 * are critical tiers while clients are tier 3.  A pool inherits its lowest
 * member tier, so a critical member cannot be hidden by a mixed pool.
 */
function panelTier(?string $role, ?string $class): int
{
    if ($role === 'monitor') {
        return 0;
    }
    if ($role === 'server' || $class === 'server' || $class === 'network' || $class === 'appliance') {
        return 1;
    }
    if ($class === 'standby' || $role === 'standby') {
        return 2;
    }
    return 3;
}

/**
 * Identify the local, read-only account reserved for the panel daemon.
 *
 * Input: current authenticated session principal.
 * Output: true only for local username panel with viewer role.
 */
function panelIsServicePrincipal(array $principal): bool
{
    return ($principal['source'] ?? null) === 'local'
        && ($principal['username'] ?? null) === 'panel'
        && ($principal['role'] ?? null) === 'viewer';
}

/**
 * Record rejected panel write attempts without including request content.
 *
 * Input: route name and current session principal, if present.
 * Output: one PHP error-log record carrying principal and remote address.
 */
function panelLogRejectedWrite(string $route, ?array $principal): void
{
    $username = is_array($principal) ? (string) ($principal['username'] ?? 'unknown') : 'anonymous';
    $ip = (string) ($_SERVER['REMOTE_ADDR'] ?? 'unknown');
    error_log("[panel] rejected write route=$route principal=$username ip=$ip");
}

/**
 * Validate one CONTROL command using the protocol's wire argument ranges.
 *
 * Input: decoded JSON body containing integer kind and arg fields.
 * Output: [kind, arg] or a 400 JSON response for malformed/unsupported input.
 *
 * @return array{0:int,1:int}
 */
function panelCommandInput(array $body): array
{
    $kind = $body['kind'] ?? null;
    $arg = $body['arg'] ?? null;
    if (!is_int($kind) || !is_int($arg)) {
        Response::error('bad_request', 'kind and arg must be integers.', 400);
    }
    if ($kind < 1 || $kind > 7 || $arg < 0 || $arg > 255) {
        Response::error('bad_request', 'Unsupported command kind or argument.', 400);
    }
    $valid = match ($kind) {
        1 => $arg <= 3,
        2 => $arg <= 2,
        3 => $arg <= 100,
        4, 5 => true, // Protocol reserves but deliberately ignores this byte.
        6 => in_array($arg, [3, 6, 30], true),
        7 => $arg <= 1,
    };
    if (!$valid) {
        Response::error('bad_request', 'Argument is invalid for this command kind.', 400);
    }
    return [$kind, $arg];
}

/**
 * Validate a decoded STATE frame before its latest-state upsert.
 *
 * Input: decoded JSON body carrying all protocol STATE values.
 * Output: normalised associative state array or a 400 JSON response.
 *
 * @return array<string,int>
 */
function panelStateInput(array $body): array
{
    $ranges = [
        'theme' => [0, 3], 'screen' => [0, 2], 'brightnessPct' => [0, 100],
        'autoBright' => [0, 1], 'sleeping' => [0, 1], 'alarmArmed' => [0, 1],
        'alarmAcked' => [0, 1], 'dwellSec' => [0, 255],
        'ackedEpisodeId' => [0, 4294967295], 'lastCmdId' => [0, 4294967295],
    ];
    $state = [];
    foreach ($ranges as $field => [$minimum, $maximum]) {
        $value = $body[$field] ?? null;
        if (!is_int($value) || $value < $minimum || $value > $maximum) {
            Response::error('bad_request', "$field is outside its protocol range.", 400);
        }
        $state[$field] = $value;
    }
    return $state;
}

return static function (Router $router): void {
    $router->get('/api/panel', static function (): void {
        $principal = Auth::current() ?? [];
        $isServicePrincipal = panelIsServicePrincipal($principal);
        $isOperator = in_array($principal['role'] ?? null, ['operator', 'admin'], true);

        // Expiry is durable queue maintenance, not a dashboard-user side effect.
        // It runs before the read-only snapshot so the returned status is current.
        if ($isServicePrincipal) {
            Db::exec(
                "UPDATE panelCommand
                    SET status = 'expired', expiredAt = NOW(6)
                  WHERE status = 'pending'
                    AND createdAt < DATE_SUB(NOW(6), INTERVAL 120 SECOND)"
            );
        }
        /*
         * A transaction makes this a consistent read even while reporters are
         * upserting.  The route uses aggregates/grouping only: no per-system
         * history or maintenance-window query is issued.
         */
        $pdo = Db::pdo();
        $pdo->exec('START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY');
        try {
            /* Roster is NODE-driven (Lead fix, review round 2 regression):
             * asset.nodeId is unpopulated today, so linkage to pool metadata
             * goes by short-hostname match. stateRoll equivalence with
             * /api/summary requires node.state verbatim over node rows. */
            $systemRows = Db::rows(
                "SELECT n.nodeId, n.hostFqdn, n.role, n.state,
                        hc.cpuLoadMilli, hc.ifaces,
                        a.assetId, a.poolId, a.displayName, a.host, a.class
                   FROM node n
              LEFT JOIN asset a
                     ON a.nodeId = n.nodeId
                     OR (a.nodeId IS NULL
                         AND a.displayName = SUBSTRING_INDEX(n.hostFqdn, '.', 1))
              LEFT JOIN hostCurrent hc ON hc.nodeId = n.nodeId
                  /* Deterministic duplicate survivor (review N1): a
                   * nodeId-linked asset beats a name-matched one, then
                   * lowest assetId; PHP keeps the first row per node. */
                  ORDER BY n.nodeId, a.nodeId IS NULL, a.assetId"
            );
            /* Adopted, probe-only systems (no agent node): state comes from
             * probe outcomes — ok on any ok vantage, down when probed and
             * never ok, unknown when unprobed. Mirrors the SPA's
             * reachability-row treatment. */
            $adoptedRows = Db::rows(
                "SELECT a.assetId, a.poolId, a.displayName, a.host, a.class,
                        COUNT(pc.targetId) AS probeCount,
                        MAX(pc.outcome = 'ok') AS anyOk
                   FROM asset a
              LEFT JOIN probeTarget pt ON pt.assetId = a.assetId
              LEFT JOIN probeCurrent pc ON pc.targetId = pt.targetId
                  WHERE a.nodeId IS NULL
                    AND NOT EXISTS (SELECT 1 FROM node n2
                                     WHERE SUBSTRING_INDEX(n2.hostFqdn, '.', 1)
                                           = a.displayName)
               GROUP BY a.assetId, a.poolId, a.displayName, a.host, a.class
               ORDER BY a.assetId"
            );
            $nodeRollRows = Db::rows(
                'SELECT state, COUNT(*) AS count FROM node GROUP BY state'
            );
            $poolRows = Db::rows('SELECT poolId, name FROM pool ORDER BY name');
            $alertCountRows = Db::rows(
                "SELECT e.severity, COUNT(*) AS count
                   FROM alertEvent e
                  WHERE e.clearedAt IS NULL
                  GROUP BY e.severity"
            );
            $critRows = Db::rows(
                "SELECT e.eventId FROM alertEvent e
                  WHERE e.clearedAt IS NULL AND e.severity = 'crit'
                  ORDER BY e.eventId ASC"
            );
            $topAlertRow = Db::row(
                "SELECT e.eventId, e.nodeId, e.targetId, e.severity, e.firedAt,
                        r.metric, n.hostFqdn, pt.label AS targetLabel,
                        a.displayName AS targetAsset
                   FROM alertEvent e
              LEFT JOIN alertRule r ON r.ruleId = e.ruleId
              LEFT JOIN node n ON n.nodeId = e.nodeId
              /* Probe-scoped alerts carry a targetId, not a nodeId: resolve a
               * human label via the probe target and its owning asset. The two
               * tables prefix targetId differently (numeric proto ordinal vs
               * proto name), so match on the host:port suffix. */
              LEFT JOIN probeTarget pt
                     ON SUBSTR(pt.targetId, INSTR(pt.targetId, ':') + 1)
                      = SUBSTR(e.targetId,  INSTR(e.targetId,  ':') + 1)
              LEFT JOIN asset a ON a.assetId = pt.assetId
                  /* Copied from summary.php: dashboard-active alert predicate. */
                  WHERE e.clearedAt IS NULL AND e.severity IN ('crit', 'warn')
                  ORDER BY CASE e.severity WHEN 'crit' THEN 0 WHEN 'warn' THEN 1 ELSE 2 END,
                           e.firedAt DESC, e.eventId DESC, pt.targetId ASC
                  LIMIT 1"
            );
            $probeRow = Db::row(
                'SELECT AVG(rttMicros) / 100 AS rttTenthMs, AVG(lossPermille) AS lossPermille
                   FROM probeCurrent'
            );
            // Contract timestamp is sampled at payload composition, not request entry.
            $nowRow = Db::row('SELECT UNIX_TIMESTAMP(NOW()) AS ts, UNIX_TIMESTAMP(MAX(sampledAt)) AS newestSampleTs FROM hostCurrent');
            $panelStateRow = Db::row(
                'SELECT theme, screen, brightnessPct, autoBright, sleeping,
                        alarmArmed, alarmAcked, dwellSec, ackedEpisodeId, lastCmdId, reportedAt
                   FROM panelState WHERE stateId = 1'
            );
            $recentCommandRows = $isOperator ? Db::rows(
                'SELECT cmdId, kind, arg, status, createdAt, appliedAt, expiredAt
                   FROM panelCommand ORDER BY cmdId DESC LIMIT 16'
            ) : [];
            $commandRows = $isServicePrincipal ? Db::rows(
                "SELECT cmdId, kind, arg FROM panelCommand
                  WHERE status = 'pending' ORDER BY cmdId ASC"
            ) : [];
            $pdo->commit();
        } catch (Throwable $e) {
            if ($pdo->inTransaction()) {
                $pdo->rollBack();
            }
            throw $e;
        }

        $poolNames = [];
        foreach ($poolRows as $poolRow) {
            $poolNames[(int) $poolRow['poolId']] = (string) $poolRow['name'];
        }
        $poolNames[0] = $poolNames[0] ?? 'UNASSIGNED';

        /* Node state is canonical; probe health must not re-derive it. */
        $systemsAll = [];
        $rxKbps = 0;
        $txKbps = 0;
        $seenNodeIds = [];
        foreach ($systemRows as $row) {
            if ($row['nodeId'] !== null) {
                $nodeId = (int) $row['nodeId'];
                if (isset($seenNodeIds[$nodeId])) {
                    continue;
                }
                $seenNodeIds[$nodeId] = true;
            }
            // Verbatim node.state.  An asset not linked to a node has no canonical state.
            $state = $row['state'] === null ? 'unknown' : (string) $row['state'];
            // summary.php reports retired separately; it is not a panel wire state.
            if ($state === 'retired') {
                continue;
            }
            $host = (string) ($row['hostFqdn'] ?? $row['host'] ?? $row['displayName'] ?? 'SYSTEM');
            [$systemRx, $systemTx] = panelInterfaceKbps($row['ifaces'] ?? null);
            $rxKbps = min(4294967295, $rxKbps + $systemRx);
            $txKbps = min(4294967295, $txKbps + $systemTx);
            $systemsAll[] = [
                'name' => panelText($row['displayName'] ?? $host, 12),
                'poolId' => $row['poolId'] === null ? 0 : (int) $row['poolId'],
                'tier' => panelTier($row['role'] ?? null, $row['class'] ?? null),
                'state' => $state,
                'loadPct' => panelLoadPct($row['cpuLoadMilli'] ?? null),
            ];
        }
        /* Adopted probe-only systems join the roster after agent nodes. */
        foreach ($adoptedRows as $row) {
            $probed = (int) ($row['probeCount'] ?? 0) > 0;
            $systemsAll[] = [
                'name' => panelText($row['displayName'] ?? $row['host'] ?? 'SYSTEM', 12),
                'poolId' => $row['poolId'] === null ? 0 : (int) $row['poolId'],
                'tier' => panelTier(null, $row['class'] ?? null),
                'state' => $probed ? (((int) ($row['anyOk'] ?? 0)) === 1 ? 'up' : 'down')
                                   : 'unknown',
                'loadPct' => 0,
            ];
        }

        $poolAgg = [];
        foreach ($systemsAll as $system) {
            $poolId = $system['poolId'];
            if (!isset($poolAgg[$poolId])) {
                $poolAgg[$poolId] = ['name' => $poolNames[$poolId] ?? 'UNASSIGNED', 'tier' => 3,
                    'up' => 0, 'degraded' => 0, 'down' => 0, 'unknown' => 0, 'retired' => 0, 'maint' => 0,
                    'loadSum' => 0, 'total' => 0];
            }
            $poolAgg[$poolId]['tier'] = min($poolAgg[$poolId]['tier'], $system['tier']);
            $state = array_key_exists($system['state'], $poolAgg[$poolId]) ? $system['state'] : 'unknown';
            $poolAgg[$poolId][$state]++;
            $poolAgg[$poolId]['loadSum'] += $system['loadPct'];
            $poolAgg[$poolId]['total']++;
        }
        uasort($poolAgg, static fn (array $a, array $b): int => [$a['tier'], $a['name']] <=> [$b['tier'], $b['name']]);

        $score = 0;
        $breachingPools = [];
        foreach ($poolAgg as $poolId => &$pool) {
            $total = $pool['total'];
            $downFraction = $total === 0 ? 0.0 : $pool['down'] / $total;
            $degradedFraction = $total === 0 ? 0.0 : $pool['degraded'] / $total;
            if ($pool['tier'] <= 1) {
                $poolScore = $pool['down'] > 0
                    ? 100 + (int) round(40 * $downFraction)
                    : min(80, (int) round(60 * $degradedFraction / 0.1));
            } else {
                $poolScore = ($downFraction >= 0.2 && $pool['down'] >= 5)
                    ? 100 + (int) round(40 * $downFraction)
                    : min(85, (int) round(85 * $downFraction / 0.2 + 30 * $degradedFraction / 0.4));
            }
            $score = max($score, $poolScore);
            if ($poolScore >= 100) {
                $breachingPools[] = (string) $poolId;
            }
            $pool['loadPct'] = $total === 0 ? 0 : (int) round($pool['loadSum'] / $total);
        }
        unset($pool);

        $alerts = ['info' => 0, 'warn' => 0, 'crit' => 0];
        foreach ($alertCountRows as $row) {
            $severity = (string) $row['severity'];
            if (isset($alerts[$severity])) $alerts[$severity] = (int) $row['count'];
        }
        $critEpisodeId = 0;
        foreach ($critRows as $row) {
            $eventId = (int) $row['eventId'];
            if ($critEpisodeId === 0 || $eventId < $critEpisodeId) $critEpisodeId = $eventId;
        }
        $topAlert = null;
        if ($topAlertRow !== null) {
            /* Best human name first: agent hostname, then asset display name,
             * then the probe label, and only then the raw targetId. */
            $node = panelText($topAlertRow['hostFqdn'] ?? $topAlertRow['targetAsset']
                ?? $topAlertRow['targetLabel'] ?? $topAlertRow['targetId'] ?? 'SYSTEM', 12);
            $metric = panelText($topAlertRow['metric'] ?? 'ALERT', 16);
            $topAlert = ['id' => (int) $topAlertRow['eventId'], 'severity' => (string) $topAlertRow['severity'],
                'subject' => panelText($node . ' ' . $metric, 24), 'detail' => panelText('CHECK ' . $metric . ' ON ' . $node, 48)];
        }

        sort($breachingPools, SORT_NUMERIC);
        $poolSet = implode(',', $breachingPools);
        $poolEpisodeId = 0x80000000 | (crc32($poolSet) & 0x7fffffff);
        $alarmActive = $critEpisodeId > 0 || $score >= 100;
        if ($alarmActive && $topAlert === null && $breachingPools !== []) {
            // High-bit namespace cannot collide with normal auto-increment IDs.
            $episodeId = $poolEpisodeId;
            $topAlert = [
                'id' => $episodeId,
                'severity' => 'crit',
                'subject' => panelText('POOL BREACH', 24),
                'detail' => panelText('CHECK ' . $poolSet, 48),
            ];
        } elseif ($critEpisodeId > 0) {
            // The first active triggering crit event anchors this episode.
            $episodeId = $critEpisodeId;
        } elseif ($breachingPools !== []) {
            // A warn event may be displayable while the pool breach owns alarm state.
            $episodeId = $poolEpisodeId;
        } else {
            $episodeId = 0;
        }

        $pools = [];
        $poolIndex = [];
        foreach (array_slice($poolAgg, 0, 8, true) as $poolId => $pool) {
            $poolIndex[(int) $poolId] = count($pools);
            $pools[] = [
                'name' => panelText($pool['name'], 8), 'tier' => $pool['tier'],
                'up' => $pool['up'], 'degraded' => $pool['degraded'], 'down' => $pool['down'],
                'unknown' => $pool['unknown'], 'maint' => $pool['maint'],
                'loadPct' => $pool['loadPct'], 'total' => $pool['total'],
            ];
        }
        usort($systemsAll, static fn (array $a, array $b): int => [$a['tier'], $a['name']] <=> [$b['tier'], $b['name']]);
        $systems = [];
        foreach ($systemsAll as $system) {
            if (!isset($poolIndex[$system['poolId']]) || count($systems) >= 64) {
                continue;
            }
            $systems[] = ['name' => $system['name'], 'pool' => $poolIndex[$system['poolId']],
                'tier' => $system['tier'], 'state' => $system['state'], 'loadPct' => $system['loadPct']];
        }

        /* stateRoll counts NODE rows verbatim — the exact population and
         * derivation /api/summary reports, so the two can never disagree.
         * (Pools/systems above additionally carry adopted probe-only assets;
         * that is deliberate — the design's pools include them, but the
         * headline roll must mirror the dashboard's.) */
        $stateRoll = ['up' => 0, 'degraded' => 0, 'down' => 0, 'unknown' => 0, 'maint' => 0];
        foreach ($nodeRollRows as $row) {
            $state = (string) $row['state'];
            if (array_key_exists($state, $stateRoll)) {
                $stateRoll[$state] += (int) $row['count'];
            }
        }
        $loadSum = 0;
        foreach ($systemsAll as $system) {
            $loadSum += $system['loadPct'];
        }
        $payload = [
            'ts' => (int) ($nowRow['ts'] ?? time()), 'score' => $score,
            'stateRoll' => $stateRoll, 'alerts' => $alerts,
            'meanLoadPct' => $systemsAll === [] ? 0 : (int) round($loadSum / count($systemsAll)),
            'rxKbps' => $rxKbps, 'txKbps' => $txKbps,
            'rttTenthMs' => max(0, min(65535, (int) round((float) ($probeRow['rttTenthMs'] ?? 0)))),
            'lossPermille' => max(0, min(65535, (int) round((float) ($probeRow['lossPermille'] ?? 0)))),
            'pools' => $pools, 'systems' => $systems, 'topAlert' => $topAlert,
            'alarmActive' => $alarmActive, 'episodeId' => $episodeId,
            'dataStale' => ($nowRow['newestSampleTs'] ?? null) !== null && ((int) $nowRow['ts'] - (int) $nowRow['newestSampleTs']) > 30,
            'panelState' => $panelStateRow === null ? null : [
                'theme' => (int) $panelStateRow['theme'],
                'screen' => (int) $panelStateRow['screen'],
                'brightnessPct' => (int) $panelStateRow['brightnessPct'],
                'autoBright' => (int) $panelStateRow['autoBright'],
                'sleeping' => (int) $panelStateRow['sleeping'],
                'alarmArmed' => (int) $panelStateRow['alarmArmed'],
                'alarmAcked' => (int) $panelStateRow['alarmAcked'],
                'dwellSec' => (int) $panelStateRow['dwellSec'],
                'ackedEpisodeId' => (int) $panelStateRow['ackedEpisodeId'],
                'lastCmdId' => (int) $panelStateRow['lastCmdId'],
                'reportedAt' => $panelStateRow['reportedAt'],
            ],
        ];
        if ($isOperator) {
            $payload['recentCommands'] = array_map(static function (array $row): array {
                /* R4: the expiry sweep only runs on service-principal polls, so
                 * with the daemon down nothing writes 'expired'. Present overdue
                 * pendings as expired read-side — status stays the authority in
                 * the DB, the observer just isn't lied to about liveness. */
                $status = (string) $row['status'];
                if ($status === 'pending'
                    && strtotime((string) $row['createdAt']) < time() - 120) {
                    $status = 'expired';
                }
                return [
                    'id' => (int) $row['cmdId'], 'kind' => (int) $row['kind'], 'arg' => (int) $row['arg'],
                    'status' => $status, 'createdAt' => $row['createdAt'],
                    'appliedAt' => $row['appliedAt'], 'expiredAt' => $row['expiredAt'],
                    'failureReason' => $status === 'expired' ? 'expired' : null,
                ];
            }, $recentCommandRows);
        }
        if ($isServicePrincipal) {
            $payload['commands'] = array_map(static fn (array $row): array => [
                'id' => (int) $row['cmdId'], 'kind' => (int) $row['kind'], 'arg' => (int) $row['arg'],
            ], $commandRows);
        }
        Response::ok($payload);
    });

    /**
     * Queue a validated CONTROL command for the physical panel.
     *
     * Input: authenticated admin/operator JSON {kind:int,arg:int}.
     * Output: {cmdId} on success; 400 validation, 403 authorization, or 409 cap.
     */
    $router->post('/api/panel/command', static function (): void {
        $principal = Auth::current();
        if (panelIsServicePrincipal($principal ?? [])) {
            panelLogRejectedWrite('command', $principal);
            Response::error('forbidden', 'The panel service principal is read-only for commands.', 403);
        }
        if (!is_array($principal) || !in_array($principal['role'] ?? null, ['operator', 'admin'], true)) {
            panelLogRejectedWrite('command', $principal);
            Response::error('forbidden', 'This action requires an operator role.', 403);
        }
        [$kind, $arg] = panelCommandInput(solari_json_body());

        // A command submission must not report a stale full queue merely
        // because no principal has polled since the oldest command expired.
        Db::exec(
            "UPDATE panelCommand
                SET status = 'expired', expiredAt = NOW(6)
              WHERE status = 'pending'
                AND createdAt < DATE_SUB(NOW(6), INTERVAL 120 SECOND)"
        );

        // Keep the pending cap race-free: the indexed pending range is locked
        // until this request inserts or rejects its command.
        $pdo = Db::pdo();
        $pdo->beginTransaction();
        try {
            $pending = (int) Db::scalar(
                "SELECT COUNT(*) FROM panelCommand WHERE status = 'pending' FOR UPDATE"
            );
            if ($pending >= 16) {
                $pdo->rollBack();
                Response::error('queue_full', 'The panel command queue already has 16 pending commands.', 409);
            }
            Db::exec(
                'INSERT INTO panelCommand (kind, arg, createdBy) VALUES (:kind, :arg, :createdBy)',
                [':kind' => $kind, ':arg' => $arg, ':createdBy' => (string) $principal['username']]
            );
            $cmdId = (int) $pdo->lastInsertId();
            $pdo->commit();
        } catch (Throwable $e) {
            if ($pdo->inTransaction()) {
                $pdo->rollBack();
            }
            throw $e;
        }
        Response::ok(['cmdId' => $cmdId]);
    });

    /**
     * Persist the newest physical-panel STATE report and confirm consumed commands.
     *
     * Input: local panel service JSON STATE frame fields.
     * Output: stored panelState summary; commands through lastCmdId become applied.
     */
    $router->post('/api/panel/state', static function (): void {
        $principal = Auth::current();
        if (!panelIsServicePrincipal($principal ?? [])) {
            panelLogRejectedWrite('state', $principal);
            Response::error('forbidden', 'Only the local panel service principal may report state.', 403);
        }
        $state = panelStateInput(solari_json_body());
        $pdo = Db::pdo();
        $pdo->beginTransaction();
        try {
            Db::exec(
                'INSERT INTO panelState
                    (stateId, theme, screen, brightnessPct, autoBright, sleeping,
                     alarmArmed, alarmAcked, dwellSec, ackedEpisodeId, lastCmdId, reportedAt)
                 VALUES
                    (1, :theme, :screen, :brightnessPct, :autoBright, :sleeping,
                     :alarmArmed, :alarmAcked, :dwellSec, :ackedEpisodeId, :lastCmdId, NOW(6))
                 ON DUPLICATE KEY UPDATE
                    theme = VALUES(theme), screen = VALUES(screen),
                    brightnessPct = VALUES(brightnessPct), autoBright = VALUES(autoBright),
                    sleeping = VALUES(sleeping), alarmArmed = VALUES(alarmArmed),
                    alarmAcked = VALUES(alarmAcked), dwellSec = VALUES(dwellSec), ackedEpisodeId = VALUES(ackedEpisodeId),
                    lastCmdId = VALUES(lastCmdId), reportedAt = VALUES(reportedAt)',
                array_combine(array_map(static fn (string $key): string => ':' . $key, array_keys($state)), $state)
            );
            Db::exec(
                "UPDATE panelCommand
                    SET status = 'applied', appliedAt = NOW(6)
                  /* Late confirmation wins only BRIEFLY (§10/R1): a firmware
                   * reboot resets lastCmdId, so an unbounded sweep would
                   * resurrect all expired history as falsely applied. */
                  WHERE (status = 'pending'
                         OR (status = 'expired'
                             AND expiredAt > DATE_SUB(NOW(6), INTERVAL 15 SECOND)))
                    AND cmdId <= :lastCmdId",
                [':lastCmdId' => $state['lastCmdId']]
            );
            $pdo->commit();
        } catch (Throwable $e) {
            if ($pdo->inTransaction()) {
                $pdo->rollBack();
            }
            throw $e;
        }
        $stored = Db::row(
            'SELECT theme, screen, brightnessPct, autoBright, sleeping,
                    alarmArmed, alarmAcked, dwellSec, ackedEpisodeId, lastCmdId, reportedAt
               FROM panelState WHERE stateId = 1'
        );
        Response::ok(['panelState' => $stored]);
    });
};
