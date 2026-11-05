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

/** Return the contract's first-label, upper-case short host label. */
function panelShortHost($hostname): string
{
    $hostname = trim((string) $hostname);
    if (filter_var($hostname, FILTER_VALIDATE_IP, FILTER_FLAG_IPV4) !== false) {
        return panelTrailingOctets($hostname, 12);
    }
    $first = explode('.', $hostname, 2)[0] ?? '';
    return panelText($first !== '' ? $first : 'SYSTEM', 12);
}

/** Drop leading IPv4 octets until the address fits; the tail identifies a host. */
function panelTrailingOctets(string $ip, int $width): string
{
    $octets = explode('.', $ip);
    while (count($octets) > 1 && strlen(implode('.', $octets)) > $width) {
        array_shift($octets);
    }
    return implode('.', $octets);
}

/** A name cut hard to a small field: the panel's dots would only cost a glyph. */
function panelCutName($value, int $width): string
{
    return rtrim(substr(panelText($value, 255), 0, $width), '. ');
}

/** Pool label: the first word, at most seven glyphs, so a row keeps room for counts. */
function panelPoolName($name): string
{
    $first = preg_split('/\s+/', trim((string) $name))[0] ?? '';
    return $first === '' ? 'NO POOL' : panelCutName($first, 7);
}

/**
 * Name one probe row within the wire's eight-glyph field.
 *
 * Input: owning asset display name and the target's host.
 * Output: the asset's first label, or the address tail when there is no name.
 */
function panelProbeName($displayName, $host): string
{
    $name = trim((string) ($displayName ?? ''));
    if ($name === '') $name = trim((string) ($host ?? ''));
    if (filter_var($name, FILTER_VALIDATE_IP, FILTER_FLAG_IPV4) !== false) {
        return panelTrailingOctets($name, 8);
    }
    return panelCutName(explode('.', $name, 2)[0] ?? 'PROBE', 8);
}

/**
 * Split an alertEvent targetId ("<proto ordinal>:<host>[:<port>]").
 *
 * @return array{0:?string,1:?int} host and port, each null when absent
 */
function panelAlertTarget($targetId): array
{
    $parts = explode(':', (string) ($targetId ?? ''));
    $host = ($parts[1] ?? '') !== '' ? $parts[1] : null;
    $port = isset($parts[2]) && ctype_digit($parts[2]) ? (int) $parts[2] : null;
    return [$host, $port];
}

/** Say which service stopped answering; a bare host is NO RESPONSE. */
function panelServiceDown(?int $port): string
{
    if ($port === null || $port === 0) return 'NO RESPONSE';
    $service = match ($port) {
        22 => 'SSH', 53 => 'DNS', 80 => 'HTTP', 88 => 'KERBEROS', 389 => 'LDAP',
        443 => 'HTTPS', 445 => 'SMB', 636 => 'LDAPS', 3000 => 'GIT', 3306 => 'MARIADB',
        5672 => 'AMQP', 15672 => 'RABBITMQ', default => 'PORT ' . $port,
    };
    return $service . ' DOWN';
}

/**
 * Reduce live probe rows to the panel's probe list and its two aggregates.
 *
 * Input: one row per (target, monitor) with targetId, host, proto, displayName,
 * outcome, rttMicros, lossPermille, sampledTs (null when never sampled); the
 * targetIds that succeeded inside 24 hours; the current time.
 * Output: per-host probe rows, in an order stable while state is stable since
 * the firmware keeps history by position, plus mean RTT and loss. A target is
 * live on any vantage's fresh success, failing when fresh and unanswered but
 * alive inside 24 hours, and dead (UNKNOWN, outside the aggregates) otherwise.
 *
 * @return array{probes:list<array<string,mixed>>,rttTenthMs:int,lossPermille:int}
 */
function panelProbeSummary(array $rows, array $recentlyAlive, int $now): array
{
    $targets = [];
    foreach ($rows as $row) {
        $id = (string) $row['targetId'];
        $targets[$id] ??= ['id' => $id, 'host' => (string) ($row['host'] ?? ''),
            'icmp' => (string) ($row['proto'] ?? '') === 'icmp',
            'name' => $row['displayName'] ?? null, 'fresh' => false, 'rtt' => [], 'loss' => []];
        if ($row['sampledTs'] === null || (int) $row['sampledTs'] < $now - 300) continue;
        $targets[$id]['fresh'] = true;
        if ((string) $row['outcome'] === 'ok') {
            $targets[$id]['rtt'][] = (float) ($row['rttMicros'] ?? 0);
            $targets[$id]['loss'][] = (float) ($row['lossPermille'] ?? 0);
        }
    }
    $rttSum = $lossSum = 0.0;
    $rttCount = $lossCount = 0;
    $hosts = [];
    foreach ($targets as $id => $target) {
        if ($target['rtt'] !== []) {
            $target['state'] = 0;
            $target['rttTenth'] = array_sum($target['rtt']) / count($target['rtt']) / 100;
            $target['lossP'] = array_sum($target['loss']) / count($target['loss']);
            $rttSum += $target['rttTenth'];
            $rttCount++;
            $lossSum += $target['lossP'];
            $lossCount++;
        } elseif ($target['fresh'] && isset($recentlyAlive[$id])) {
            $target['state'] = 2;
            $lossSum += 1000;
            $lossCount++;
        } else {
            $target['state'] = 3;
        }
        $hosts[$target['host']][] = $target;
    }
    $list = [];
    foreach ($hosts as $members) {
        usort($members, static fn (array $a, array $b): int =>
            [$a['state'] === 3, !$a['icmp'], $a['id']] <=> [$b['state'] === 3, !$b['icmp'], $b['id']]);
        $lead = $members[0];
        $state = $lead['state'];
        foreach ($members as $member) {
            if ($state === 0 && $member['state'] === 2) $state = 1; // Answers, but a service does not.
        }
        $list[] = ['alive' => $lead['state'] !== 3, 'icmp' => $lead['icmp'], 'id' => $lead['id'],
            'row' => ['name' => panelProbeName($lead['name'], $lead['host']),
                'rttTenthMs' => $lead['state'] === 0 ? max(0, min(65534, (int) round($lead['rttTenth']))) : null,
                'lossPermille' => $lead['state'] === 0 ? max(0, min(1000, (int) round($lead['lossP']))) : null,
                'state' => $state]];
    }
    usort($list, static fn (array $a, array $b): int =>
        [!$a['alive'], !$a['icmp'], $a['id']] <=> [!$b['alive'], !$b['icmp'], $b['id']]);
    return ['probes' => array_map(static fn (array $h): array => $h['row'], array_slice($list, 0, 11)),
        'rttTenthMs' => $rttCount === 0 ? 0 : (int) round($rttSum / $rttCount),
        'lossPermille' => $lossCount === 0 ? 0 : (int) round($lossSum / $lossCount)];
}

/**
 * Derive a CRC-namespace alarm episode id from a triggering-set seed string.
 *
 * Episode-ID namespace (CONTRACT-SW §4/§13 D17): auto-increment eventIds
 * live below 0x80000000; server CRC episodes occupy 0x80000000-0xBFFFFFFF,
 * so the CRC is masked to 30 bits; 0xC0000000+ is reserved for
 * firmware-local episodes and must never be produced here.
 *
 * Input: the sorted, comma-joined triggering set (with any namespace prefix).
 * Output: integer in [0x80000000, 0xBFFFFFFF].
 */
function panelCrcEpisode(string $seed): int
{
    return 0x80000000 | (crc32($seed) & 0x3fffffff);
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

function panelHasInterfaceRate($ifaces): bool
{
    $entries = is_string($ifaces) ? json_decode($ifaces, true) : $ifaces;
    if (!is_array($entries)) return false;
    foreach ($entries as $entry) {
        if (is_array($entry) && (is_numeric($entry['rxKbps'] ?? null) || is_numeric($entry['txKbps'] ?? null))) {
            return true;
        }
    }
    return false;
}

function panelResourcePct($used, $total): int
{
    $used = is_numeric($used) ? (float) $used : 0.0;
    $total = is_numeric($total) ? (float) $total : 0.0;
    return $total <= 0 ? 0 : max(0, min(100, (int) round(100 * $used / $total)));
}

function panelDiskPct($disks): int
{
    $entries = is_string($disks) ? json_decode($disks, true) : $disks;
    $fullest = 0;
    if (is_array($entries)) {
        foreach ($entries as $disk) {
            if (is_array($disk) && (float) ($disk['totalKb'] ?? 0) > 0) {
                $fullest = max($fullest, panelResourcePct($disk['usedKb'] ?? 0, $disk['totalKb']));
            }
        }
    }
    return $fullest;
}

function panelFaultCode(?string $metric): int
{
    return match ($metric) {
        'reachable' => 1, 'health.failedUnits' => 2, 'health.fsReadonly' => 3,
        'health.smartFail' => 4, 'health.dmesgCrit' => 5,
        'health.blockDevMissing' => 6, default => 0,
    };
}

function panelAlertMessage(?string $metric, $detail, $failedUnitCount, $failedUnitList, ?int $port = null): string
{
    $count = is_numeric($failedUnitCount) ? max(0, (int) $failedUnitCount) : 0;
    if ($metric === 'reachable') return panelServiceDown($port);
    if ($metric === 'health.failedUnits') {
        if ($count > 1) return panelText($count . ' SVCS FAILED', 13);
        $units = trim((string) $failedUnitList);
        if ($units !== '' && $units[0] === '[') {
            $decodedUnits = json_decode($units, true);
            if (is_array($decodedUnits)) $units = implode(' ', array_map('strval', $decodedUnits));
        }
        if ($units !== '') {
            $unit = preg_split('/[,;\s]+/', $units)[0] ?? '';
            if ($unit !== '') return rtrim(panelText($unit, 6), '.') . ' FAILED';
        }
        if ($count === 1 && preg_match('/detail=([^ ]+)/', (string) $detail, $match)) {
            return rtrim(panelText($match[1], 6), '.') . ' FAILED';
        }
        return 'UNIT FAILED';
    }
    return match ($metric) {
        'health.fsReadonly' => 'DISK READONLY',
        'health.smartFail' => 'DISK FAILING',
        'health.dmesgCrit' => 'KERNEL ERRORS',
        'health.blockDevMissing' => 'DISK MISSING',
        default => panelCutName(substr(strrchr('.' . (string) $metric, '.'), 1) ?: 'ALERT', 13),
    };
}

function panelStateRank(string $state): int
{
    return match ($state) { 'down' => 0, 'degraded' => 1, 'unknown' => 2, 'maint' => 3, default => 4 };
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
 * Map a networkGear.kind value to the CONTRACT-AW §3.1 gear role byte.
 *
 * Input: the gear row's kind enum value.
 * Output: 0=router 1=switch 2=hub 3=ap 4=wanBackup; null for kinds A1 has no
 * gate for ('other') — those rows are dropped from the gear section entirely
 * rather than mis-rendered as a phantom router/switch.
 */
function panelGearRole(string $kind): ?int
{
    return match ($kind) {
        'gateway', 'router' => 0,
        'switch' => 1,
        'hub' => 2,
        'ap' => 3,
        'wanBackup' => 4,
        default => null,
    };
}

/**
 * Log-scale a Kbps rate into the wire's 0..7 brightness/particle-rate level.
 *
 * Input: a non-negative Kbps rate.
 * Output: 0 (idle) through 7 (>=20 Mbps); doubling roughly every level so a
 * home LAN's realistic 0..~20 Mbps aggregate range uses the whole scale
 * instead of pinning at the top or bottom.
 */
function panelGearLogScale(int $kbps): int
{
    if ($kbps < 1) {
        return 0;
    }
    $thresholds = [1, 10, 50, 200, 1000, 5000, 20000];
    $level = 1;
    foreach ($thresholds as $threshold) {
        if ($kbps >= $threshold) {
            $level++;
        }
    }
    return min(7, $level - 1);
}

/**
 * Map gearInterfaceCurrent.operStatus (IF-MIB) to the §3.1 wire gear state.
 *
 * Input: the raw IF-MIB operStatus value (1=up..7=lowerLayerDown, nullable).
 * Output: 0=down 1=up 2=degraded, per CONTRACT-AW §9 A-4 (NORMATIVE).
 * FAIL DARK: any unrecognised or null value maps to down, never degraded —
 * an unmapped/unknown status must never render healthier than a known-down
 * device. Passing operStatus through unmapped previously made every down
 * device render DEGRADED (amber-solid) and wire state 0 unreachable,
 * defeating the A-3 internet-column ruling for a downed router.
 */
function panelGearState(?int $operStatus): int
{
    return match ($operStatus) {
        1 => 1,
        3, 5 => 2,
        default => 0,
    };
}

/**
 * Map IF-MIB operStatus to the links section's PanelState (OK 0, DOWN 2,
 * UNKNOWN 3). The gear section above keeps its own older wire numbering.
 */
function panelLinkState(?int $operStatus): int
{
    return match ($operStatus) {
        1 => 0,
        2, 6, 7 => 2,
        default => 3,
    };
}

/** Map a roster system's state word to PanelState for its host link. */
function panelStateCode(string $state): int
{
    return match ($state) { 'up' => 0, 'degraded' => 1, 'down' => 2, 'maint' => 4, default => 3 };
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
    if ($kind < 1 || $kind > 9 || $arg < 0 || $arg > 255) {
        Response::error('bad_request', 'Unsupported command kind or argument.', 400);
    }
    $valid = match ($kind) {
        1 => $arg <= 3,
        2 => $arg <= 3,
        3 => $arg <= 100,
        4, 5 => true, // Protocol reserves but deliberately ignores this byte.
        6 => in_array($arg, [3, 6, 30], true),
        7 => $arg <= 1,
        // CONTRACT-AW §3.2 + D2: PANEL_CTL_SCREENEN arg = (screenIdx<<1)|enabled,
        // screenIdx = theme*4+slot in 0..15 -> arg <= 31 covers the whole range.
        8 => $arg <= 31,
        // PANEL_CTL_SCREENWT arg = (screenIdx<<3)|weightCode; D2: screenIdx <= 15
        // AND weightCode <= 5, checked as the two decomposed fields.
        9 => ($arg >> 3) <= 15 && ($arg & 0x7) <= 5,
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
        'theme' => [0, 3], 'screen' => [0, 3], 'brightnessPct' => [0, 100],
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

/**
 * Validate a decoded CONFIG report frame (CONTRACT-AW §3.3) before upsert.
 *
 * Input: decoded JSON body {screenCfg:[{enabled,weightCode} x12], flags}.
 * Output: [screenCfgJson, flags] or a 400 JSON response for malformed input.
 *
 * @return array{0:string,1:int}
 */
function panelConfigInput(array $body): array
{
    $screenCfg = $body['screenCfg'] ?? null;
    $flags = $body['flags'] ?? null;
    if (!is_array($screenCfg) || !in_array(count($screenCfg), [12, 16], true) || !is_int($flags) || $flags < 0 || $flags > 255) {
        Response::error('bad_request', 'screenCfg (12 or 16 entries) and flags are required.', 400);
    }
    $normalised = [];
    foreach (array_values($screenCfg) as $entry) {
        $enabled = is_array($entry) ? ($entry['enabled'] ?? null) : null;
        $weightCode = is_array($entry) ? ($entry['weightCode'] ?? null) : null;
        if (!is_int($enabled) || $enabled < 0 || $enabled > 1
            || !is_int($weightCode) || $weightCode < 0 || $weightCode > 5) {
            Response::error('bad_request', 'Each screenCfg entry needs enabled (0/1) and weightCode (0-5).', 400);
        }
        $normalised[] = ['enabled' => $enabled, 'weightCode' => $weightCode];
    }
    $json = json_encode($normalised);
    if ($json === false) {
        Response::error('bad_request', 'screenCfg could not be encoded.', 400);
    }
    return [$json, $flags];
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
            /* CONTRACT-LC §3.2: criticality tiers weight the panel feed.
             * Feature-detected (migration 018 may not be applied): every
             * snippet interpolates with a LEADING comma so the SELECT lists
             * stay syntactically closed either way — the trailing-comma
             * variant of this exact code took /api/panel down for 5 hours
             * on 2026-08-06. Pre-migration, every entity reads as tier 2. */
            $lcCols = Db::row(
                "SELECT COUNT(*) AS n FROM information_schema.COLUMNS
                  WHERE TABLE_SCHEMA = DATABASE()
                    AND ((TABLE_NAME = 'asset' AND COLUMN_NAME = 'criticality')
                      OR (TABLE_NAME = 'node'  AND COLUMN_NAME = 'criticality'))"
            );
            $hasCrit = ((int) ($lcCols['n'] ?? 0)) === 2;
            $selSysCrit   = $hasCrit ? ', n.criticality AS nodeCriticality, a.criticality AS assetCriticality'
                                     : ', 2 AS nodeCriticality, 2 AS assetCriticality';
            /* MAX() form: used inside a GROUP BY query, and MariaDB does not
             * infer functional dependency from the PK under
             * ONLY_FULL_GROUP_BY. Value is constant per group (PK-grouped). */
            $selAssetCrit = $hasCrit ? ', MAX(a.criticality) AS assetCriticality'
                                     : ', 2 AS assetCriticality';
            $selAlertCrit = $hasCrit ? ', COALESCE(n.criticality, a.criticality, 2) AS entityCrit'
                                     : ', 2 AS entityCrit';
            /* Roster is NODE-driven (Lead fix, review round 2 regression):
             * asset.nodeId is unpopulated today, so linkage to pool metadata
             * goes by short-hostname match. stateRoll equivalence with
             * /api/summary requires node.state verbatim over node rows. */
            $systemRows = Db::rows(
                "SELECT n.nodeId, n.hostFqdn, n.role, n.state,
                        hc.cpuLoadMilli, hc.ifaces, hc.ramUsedKb, hc.ramTotalKb, hc.disks,
                        hc.fsReadonlyCount, hc.blockDevMissing, hc.smartFailCount,
                        hc.failedUnitCount, hc.dmesgCritCount,
                        a.assetId, a.poolId, a.displayName, a.host, a.class{$selSysCrit}
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
                        MAX(pc.outcome = 'ok') AS anyOk{$selAssetCrit}
                   FROM asset a
              LEFT JOIN probeTarget pt ON pt.assetId = a.assetId
              LEFT JOIN probeCurrent pc ON pc.targetId = pt.targetId
                  WHERE a.nodeId IS NULL
                    AND a.lifecycle <> 'decommissioned'
                    AND NOT EXISTS (SELECT 1 FROM node n2
                                     WHERE SUBSTRING_INDEX(n2.hostFqdn, '.', 1)
                                           = a.displayName)
               GROUP BY a.assetId, a.poolId, a.displayName, a.host, a.class
               ORDER BY a.assetId"
            );
            $poolRows = Db::rows('SELECT poolId, name FROM pool ORDER BY name');
            $alertCountRows = Db::rows(
                "SELECT e.severity, COUNT(*) AS count
                   FROM alertEvent e
                   JOIN (SELECT ruleId, nodeId, targetId, MIN(eventId) AS eventId
                           FROM alertEvent
                          WHERE clearedAt IS NULL AND eventKind='fired' AND severity IN ('info','warn','crit')
                          GROUP BY ruleId, nodeId, targetId) d ON d.eventId=e.eventId
                  GROUP BY e.severity"
            );
            /* eventId doubles as an alarm episodeId, so the alertEvent
             * auto-increment must stay below 0x80000000 (2147483648) — the
             * CRC-episode namespace floor (CONTRACT-SW §13 D17). Reaching
             * that bound is out of reach at this fleet's alert rates. */
            $critRows = Db::rows(
                "SELECT e.eventId FROM alertEvent e
                  WHERE e.clearedAt IS NULL AND e.severity = 'crit' AND e.eventKind = 'fired'
                  ORDER BY e.eventId ASC"
            );
            $topAlertRow = Db::row(
                "SELECT e.eventId, e.nodeId, e.targetId, e.severity, e.firedAt, e.detail,
                        r.metric, n.hostFqdn, pt.label AS targetLabel,
                        a.displayName AS targetAsset, hc.failedUnitCount, hc.failedUnitList{$selAlertCrit}
                   FROM alertEvent e
              LEFT JOIN alertRule r ON r.ruleId = e.ruleId
              LEFT JOIN node n ON n.nodeId = e.nodeId
              LEFT JOIN hostCurrent hc ON hc.nodeId=e.nodeId
              /* Probe-scoped alerts carry a targetId, not a nodeId: resolve a
               * human label via the probe target and its owning asset. The two
               * tables prefix targetId differently (numeric proto ordinal vs
               * proto name), so match on the host:port suffix. */
              LEFT JOIN probeTarget pt
                     ON SUBSTR(pt.targetId, INSTR(pt.targetId, ':') + 1)
                      = SUBSTR(e.targetId,  INSTR(e.targetId,  ':') + 1)
              LEFT JOIN asset a ON a.assetId = pt.assetId
                  /* Copied from summary.php: dashboard-active alert predicate. */
                  JOIN (SELECT ruleId, nodeId, targetId, MIN(eventId) AS eventId
                          FROM alertEvent
                         WHERE clearedAt IS NULL AND eventKind='fired' AND severity IN ('crit','warn')
                         GROUP BY ruleId, nodeId, targetId) currentCondition ON currentCondition.eventId=e.eventId
                  WHERE e.clearedAt IS NULL AND e.severity IN ('crit', 'warn')
                  /* CONTRACT-LC §3.2: tier outranks severity for the panel's
                   * one visible alert slot (vital-but-warn beats junk-crit). */
                  ORDER BY entityCrit DESC,
                           CASE e.severity WHEN 'crit' THEN 0 WHEN 'warn' THEN 1 ELSE 2 END,
                           e.firedAt DESC, e.eventId DESC, pt.targetId ASC
                  LIMIT 1"
            );
            $alertItemRows = Db::rows(
                "SELECT e.eventId, e.severity, e.firedAt, e.detail, e.ruleId,
                        e.nodeId, e.targetId, r.metric, n.hostFqdn,
                        hc.failedUnitCount, hc.failedUnitList,
                        pt.label AS targetLabel, a.displayName AS targetAsset{$selAlertCrit}
                   FROM alertEvent e
              LEFT JOIN alertRule r ON r.ruleId=e.ruleId
              LEFT JOIN node n ON n.nodeId=e.nodeId
              LEFT JOIN hostCurrent hc ON hc.nodeId=e.nodeId
              LEFT JOIN probeTarget pt
                     ON SUBSTR(pt.targetId, INSTR(pt.targetId, ':') + 1)
                      = SUBSTR(e.targetId, INSTR(e.targetId, ':') + 1)
              LEFT JOIN asset a ON a.assetId=pt.assetId
                   JOIN (SELECT ruleId, nodeId, targetId, MIN(eventId) AS eventId
                           FROM alertEvent WHERE clearedAt IS NULL AND eventKind='fired'
                          GROUP BY ruleId, nodeId, targetId) currentCondition
                     ON currentCondition.eventId=e.eventId
                  WHERE e.clearedAt IS NULL
               ORDER BY entityCrit DESC,
                        CASE e.severity WHEN 'crit' THEN 0 WHEN 'warn' THEN 1 ELSE 2 END,
                        e.firedAt DESC, e.eventId DESC
                  LIMIT 256"
            );
            $alertHeatRows = Db::rows(
                "SELECT r.metric, e.severity, e.firedAt,
                        GREATEST(0, 51 - FLOOR(TIMESTAMPDIFF(MINUTE,e.firedAt,NOW(6))/30)) AS bucket
                   FROM alertEvent e
              LEFT JOIN alertRule r ON r.ruleId=e.ruleId
                   JOIN (SELECT ruleId, nodeId, targetId, MAX(eventId) AS eventId
                           FROM alertEvent
                          WHERE eventKind='fired' AND firedAt >= DATE_SUB(NOW(6), INTERVAL 26 HOUR)
                          GROUP BY ruleId, nodeId, targetId) recentCondition
                     ON recentCondition.eventId=e.eventId
                  WHERE e.eventKind='fired' AND e.firedAt >= DATE_SUB(NOW(6), INTERVAL 26 HOUR)
                    AND e.severity IN ('warn','crit')
               ORDER BY e.firedAt"
            );
            $faultByNodeRows = Db::rows(
                "SELECT nodeId, COUNT(*) AS faults
                   FROM (SELECT nodeId, ruleId, targetId
                           FROM alertEvent
                          WHERE clearedAt IS NULL AND eventKind='fired' AND nodeId IS NOT NULL
                          GROUP BY nodeId, ruleId, targetId) conditions
               GROUP BY nodeId"
            );
            $faultByAssetRows = Db::rows(
                "SELECT pt.assetId, COUNT(*) AS faults
                   FROM (SELECT ruleId, nodeId, targetId, MIN(eventId) AS eventId
                           FROM alertEvent
                          WHERE clearedAt IS NULL AND eventKind='fired' AND targetId IS NOT NULL
                          GROUP BY ruleId, nodeId, targetId) conditions
                   JOIN alertEvent e ON e.eventId=conditions.eventId
                   JOIN probeTarget pt
                     ON SUBSTR(pt.targetId, INSTR(pt.targetId, ':') + 1)
                      = SUBSTR(e.targetId, INSTR(e.targetId, ':') + 1)
               GROUP BY pt.assetId"
            );
            /* probeCurrent only, so a 5 s poll stays cheap and the rows move with
             * the network; history is consulted just for targets failing now. */
            $probeRows = Db::rows(
                "SELECT pt.targetId, pt.host, pt.proto, a.displayName, pc.outcome,
                        pc.rttMicros, pc.lossPermille, UNIX_TIMESTAMP(pc.sampledAt) AS sampledTs
                   FROM probeTarget pt
              LEFT JOIN asset a ON a.assetId = pt.assetId
              LEFT JOIN probeCurrent pc ON pc.targetId = pt.targetId
               ORDER BY pt.targetId"
            );
            $unanswered = [];
            foreach ($probeRows as $row) {
                $unanswered[(string) $row['targetId']] ??= true;
                if ((string) $row['outcome'] === 'ok') $unanswered[(string) $row['targetId']] = false;
            }
            $unanswered = array_keys(array_filter($unanswered));
            $recentlyAlive = [];
            if ($unanswered !== []) {
                $marks = implode(',', array_fill(0, count($unanswered), '?'));
                $aliveRows = Db::rows(
                    "SELECT DISTINCT targetId FROM probeHistory
                      WHERE targetId IN ($marks) AND outcome = 'ok'
                        AND sampledAt >= DATE_SUB(NOW(6), INTERVAL 24 HOUR)",
                    $unanswered
                );
                foreach ($aliveRows as $row) $recentlyAlive[(string) $row['targetId']] = true;
            }
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
            $gearColumns = Db::rows(
                "SELECT COLUMN_NAME FROM information_schema.COLUMNS
                  WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='gearInterfaceCurrent'
                    AND COLUMN_NAME='isUplink'"
            );
            $selIsUplink = $gearColumns === [] ? '0 AS isUplink' : 'i.isUplink';
            /* A1 gear section (CONTRACT-AW §3.1). networkGear/gearInterfaceCurrent
             * are live from migrations 002/006 already — no feature-detect needed
             * for these two. Freshness matches the daemon's 15s poll cadence with
             * headroom; ifIndex=0 is the poller's aggregate-per-device row. */
            $gearRows = Db::rows(
                "SELECT g.gearId, g.name, g.kind, g.uplinkGearId,
                        i.ifIndex, i.ifName, i.inRateKbps, i.outRateKbps,
                        i.operStatus, {$selIsUplink}, i.sampledAt
                   FROM networkGear g
                   JOIN gearInterfaceCurrent i
                     ON i.gearId = g.gearId
                  WHERE g.lastSeenAt > DATE_SUB(NOW(6), INTERVAL 90 DAY)
                    AND i.sampledAt > DATE_SUB(NOW(6), INTERVAL 60 SECOND)
                  ORDER BY g.gearId, i.ifIndex"
            );
            /* panelScreenConfig is new in migration 019, not yet applied live:
             * feature-detect per the live-file discipline (see $lcCols above). */
            $hasScreenCfg = ((int) (Db::row(
                "SELECT COUNT(*) AS n FROM information_schema.TABLES
                  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'panelScreenConfig'"
            )['n'] ?? 0)) === 1;
            $panelScreenConfigRow = $hasScreenCfg ? Db::row(
                'SELECT screenCfg, flags, reportedAt FROM panelScreenConfig WHERE configId = 1'
            ) : null;
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
        $poolNames[0] = $poolNames[0] ?? '';

        /* Node state is canonical; probe health must not re-derive it. */
        $systemsAll = [];
        $rxKbps = 0;
        $txKbps = 0;
        $faultsByNode = [];
        foreach ($faultByNodeRows as $faultRow) {
            $faultsByNode[(int) $faultRow['nodeId']] = min(255, (int) $faultRow['faults']);
        }
        $faultsByAsset = [];
        foreach ($faultByAssetRows as $faultRow) {
            if ($faultRow['assetId'] !== null) $faultsByAsset[(int) $faultRow['assetId']] = min(255, (int) $faultRow['faults']);
        }
        $seenNodeIds = [];
        foreach ($systemRows as $row) {
            $nodeId = $row['nodeId'] === null ? 0 : (int) $row['nodeId'];
            if ($row['nodeId'] !== null) {
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
            // A monitor is a vantage on a host the roster already has; probes show its health.
            if ((string) $row['role'] === 'monitor') {
                continue;
            }
            $host = (string) ($row['hostFqdn'] ?? $row['host'] ?? $row['displayName'] ?? 'SYSTEM');
            [$systemRx, $systemTx] = panelInterfaceKbps($row['ifaces'] ?? null);
            // A silent node's last sample is history: never show it as current traffic.
            $hasInterfaceRate = $state !== 'unknown' && panelHasInterfaceRate($row['ifaces'] ?? null);
            if (!$hasInterfaceRate) {
                $systemRx = $systemTx = 0;
            }
            $rxKbps = min(4294967295, $rxKbps + $systemRx);
            $txKbps = min(4294967295, $txKbps + $systemTx);
            $systemsAll[] = [
                'name' => panelShortHost($row['hostFqdn'] ?? $row['host'] ?? $row['displayName'] ?? 'SYSTEM'),
                'episodeName' => panelText($row['displayName'] ?? $host, 12),
                'poolId' => $row['poolId'] === null ? 0 : (int) $row['poolId'],
                'tier' => panelTier($row['role'] ?? null, $row['class'] ?? null),
                /* CONTRACT-LC §3.2: node tier wins over the linked asset's. */
                'criticality' => (int) ($row['nodeCriticality'] ?? $row['assetCriticality'] ?? 2),
                'state' => $state,
                'loadPct' => $state === 'unknown' ? 0 : panelLoadPct($row['cpuLoadMilli'] ?? null),
                'memPct' => $state === 'unknown' ? 0 : panelResourcePct($row['ramUsedKb'] ?? 0, $row['ramTotalKb'] ?? 0),
                'diskPct' => $state === 'unknown' ? 0 : panelDiskPct($row['disks'] ?? null),
                'faults' => $nodeId !== 0 ? ($faultsByNode[$nodeId] ?? 0)
                    : ($faultsByAsset[(int) ($row['assetId'] ?? 0)] ?? 0),
                'rxKbps' => $hasInterfaceRate ? $systemRx : null,
                'txKbps' => $hasInterfaceRate ? $systemTx : null,
            ];
        }
        /* Adopted probe-only systems join the roster after agent nodes. */
        foreach ($adoptedRows as $row) {
            $probed = (int) ($row['probeCount'] ?? 0) > 0;
            $systemsAll[] = [
                'name' => panelShortHost($row['host'] ?? $row['displayName'] ?? 'SYSTEM'),
                'episodeName' => panelText($row['displayName'] ?? $row['host'] ?? 'SYSTEM', 12),
                'poolId' => $row['poolId'] === null ? 0 : (int) $row['poolId'],
                'tier' => panelTier(null, $row['class'] ?? null),
                'criticality' => (int) ($row['assetCriticality'] ?? 2),
                'state' => $probed ? (((int) ($row['anyOk'] ?? 0)) === 1 ? 'up' : 'down')
                                   : 'unknown',
                'loadPct' => 0,
                'memPct' => 0, 'diskPct' => 0,
                'faults' => $faultsByAsset[(int) $row['assetId']] ?? 0, 'rxKbps' => null, 'txKbps' => null,
            ];
        }

        $poolAgg = [];
        foreach ($systemsAll as $system) {
            $poolId = $system['poolId'];
            if (!isset($poolAgg[$poolId])) {
                $poolAgg[$poolId] = ['name' => $poolNames[$poolId] ?? '', 'tier' => 3,
                    'up' => 0, 'degraded' => 0, 'down' => 0, 'unknown' => 0, 'retired' => 0, 'maint' => 0,
                    'loadSum' => 0, 'total' => 0, 'scoreDown' => 0,
                    'scoreDegraded' => 0, 'scoreTotal' => 0];
            }
            $poolAgg[$poolId]['tier'] = min($poolAgg[$poolId]['tier'], $system['tier']);
            $state = array_key_exists($system['state'], $poolAgg[$poolId]) ? $system['state'] : 'unknown';
            $poolAgg[$poolId][$state]++;
            $poolAgg[$poolId]['loadSum'] += $system['loadPct'];
            $poolAgg[$poolId]['total']++;
            /* CONTRACT-LC §3.2: tier 0/1 entities stay VISIBLE in the counts
             * (stateRoll/totals stay honest) but are excluded from the alarm
             * score — the score* counters are the score's denominator. */
            if ($system['criticality'] <= 1) {
                continue;
            }
            if ($state === 'down') {
                $poolAgg[$poolId]['scoreDown']++;
            } elseif ($state === 'degraded') {
                $poolAgg[$poolId]['scoreDegraded']++;
            }
            $poolAgg[$poolId]['scoreTotal']++;
        }
        uasort($poolAgg, static fn (array $a, array $b): int => [$a['tier'], $a['name']] <=> [$b['tier'], $b['name']]);

        $score = 0;
        $breachingPools = [];
        foreach ($poolAgg as $poolId => &$pool) {
            /* Score denominators exclude tier 0/1 (see aggregation above). */
            $total = $pool['scoreTotal'];
            $downFraction = $total === 0 ? 0.0 : $pool['scoreDown'] / $total;
            $degradedFraction = $total === 0 ? 0.0 : $pool['scoreDegraded'] / $total;
            if ($pool['tier'] <= 1) {
                $poolScore = $pool['scoreDown'] > 0
                    ? 100 + (int) round(40 * $downFraction)
                    : min(80, (int) round(60 * $degradedFraction / 0.1));
            } else {
                $poolScore = ($downFraction >= 0.2 && $pool['scoreDown'] >= 5)
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
        /* Two monitors seeing one dead service are one condition to the reader. */
        $alertListItems = [];
        $alertKeys = [];
        $faultCounts = array_fill(0, 7, 0);
        foreach ($alertItemRows as $row) {
            /* A probe alert's nodeId is the monitor that saw it: name the target. */
            [$targetHost, $targetPort] = panelAlertTarget($row['targetId'] ?? null);
            $metric = $row['metric'] === null ? null : (string) $row['metric'];
            $key = $targetHost !== null ? $metric . '|' . $targetHost . '|' . $targetPort
                : $metric . '|' . (string) $row['nodeId'];
            if (isset($alertKeys[$key])) continue;
            $alertKeys[$key] = true;
            $faultCounts[panelFaultCode($metric)]++;
            if (count($alertListItems) >= 8) continue;
            $alertListItems[] = [
                'severity' => (string) $row['severity'], 'code' => panelFaultCode($metric),
                'ageMin' => max(0, (int) floor(((int) ($nowRow['ts'] ?? time()) - strtotime((string) $row['firedAt'])) / 60)),
                'host' => panelShortHost($targetHost !== null ? ($row['targetAsset'] ?? $targetHost)
                    : ($row['hostFqdn'] ?? 'SYSTEM')),
                'msg' => panelAlertMessage($metric, $row['detail'] ?? '',
                    $row['failedUnitCount'] ?? 0, $row['failedUnitList'] ?? '', $targetPort),
            ];
        }
        $conditionTotal = count($alertKeys);
        $faultCounts = array_map(static fn (int $count): int => min(255, $count), $faultCounts);
        $alertHeat = ['warn' => array_fill(0, 52, 0), 'crit' => array_fill(0, 52, 0)];
        foreach ($alertHeatRows as $row) {
            $severity = (string) $row['severity'];
            $bucket = max(0, min(51, (int) $row['bucket']));
            if (isset($alertHeat[$severity])) $alertHeat[$severity][$bucket]++;
        }
        foreach ($alertHeat as &$buckets) {
            $buckets = array_map(static fn (int $count): int => min(255, $count), $buckets);
        }
        unset($buckets);
        $probeSummary = panelProbeSummary($probeRows, $recentlyAlive, (int) ($nowRow['ts'] ?? time()));
        $critEpisodeId = 0;
        foreach ($critRows as $row) {
            $eventId = (int) $row['eventId'];
            if ($critEpisodeId === 0 || $eventId < $critEpisodeId) $critEpisodeId = $eventId;
        }
        $topAlert = null;
        if ($topAlertRow !== null) {
            /* Probe alerts name their target; the row's node is only the monitor. */
            [$targetHost, $targetPort] = panelAlertTarget($topAlertRow['targetId'] ?? null);
            $node = panelShortHost($targetHost !== null ? ($topAlertRow['targetAsset'] ?? $targetHost)
                : ($topAlertRow['hostFqdn'] ?? 'SYSTEM'));
            $metric = (string) ($topAlertRow['metric'] ?? 'ALERT');
            $message = panelAlertMessage($metric, $topAlertRow['detail'] ?? '',
                $topAlertRow['failedUnitCount'] ?? 0, $topAlertRow['failedUnitList'] ?? '', $targetPort);
            $topAlert = ['id' => (int) $topAlertRow['eventId'], 'severity' => (string) $topAlertRow['severity'],
                'subject' => $node, 'detail' => $message];
        }

        sort($breachingPools, SORT_NUMERIC);
        $poolSet = implode(',', $breachingPools);
        $poolEpisodeId = panelCrcEpisode($poolSet);
        /* CONTRACT-LC §3.2 tier 4: any vital entity down forces the alarm,
         * independent of alert rows and score smoothing ("Paul Revere"). */
        $vitalDown = [];
        foreach ($systemsAll as $system) {
            if ($system['criticality'] === 4 && $system['state'] === 'down') {
                $vitalDown[] = $system['episodeName'];
            }
        }
        sort($vitalDown, SORT_STRING);
        $vitalSet = implode(',', $vitalDown);
        $vitalEpisodeId = panelCrcEpisode('vital:' . $vitalSet);
        $alarmActive = $critEpisodeId > 0 || $score >= 100 || $vitalDown !== [];
        if ($alarmActive && $topAlert === null && $breachingPools !== []) {
            // High-bit namespace cannot collide with normal auto-increment IDs.
            $episodeId = $poolEpisodeId;
            $poolLabels = array_map(static fn (string $id): string =>
                (string) ($poolAgg[(int) $id]['name'] ?? $id), $breachingPools);
            $poolDetail = count($poolLabels) === 1 ? $poolLabels[0]
                : $poolLabels[0] . ' +' . (count($poolLabels) - 1) . ' MORE';
            $topAlert = [
                'id' => $episodeId,
                'severity' => 'crit',
                'subject' => 'POOL BREACH', 'detail' => panelText($poolDetail, 13),
            ];
        } elseif ($critEpisodeId > 0) {
            // The first active triggering crit event anchors this episode.
            $episodeId = $critEpisodeId;
        } elseif ($breachingPools !== []) {
            // A warn event may be displayable while the pool breach owns alarm state.
            $episodeId = $poolEpisodeId;
        } elseif ($vitalDown !== []) {
            // A vital entity down is alarm-worthy even without an alert row
            // (the engine may not have fired yet — the panel must not wait).
            $episodeId = $vitalEpisodeId;
            if ($topAlert === null) {
                $topAlert = [
                    'id' => $episodeId,
                    'severity' => 'crit',
                    'subject' => 'VITAL DOWN', 'detail' => panelShortHost($vitalSet),
                ];
            }
        } else {
            $episodeId = 0;
        }

        $pools = [];
        $poolIndex = [];
        $poolEntries = array_slice($poolAgg, 0, 7, true);
        $overflowPools = array_slice($poolAgg, 7, null, true);
        foreach ($poolEntries as $poolId => $pool) {
            $poolIndex[(int) $poolId] = count($pools);
            $pools[] = [
                'name' => panelPoolName($pool['name']), 'tier' => $pool['tier'],
                'up' => $pool['up'], 'degraded' => $pool['degraded'], 'down' => $pool['down'],
                'unknown' => $pool['unknown'], 'maint' => $pool['maint'],
                'loadPct' => $pool['loadPct'], 'total' => $pool['total'],
            ];
        }
        if ($overflowPools !== []) {
            $overflow = ['name' => 'OTHER', 'tier' => 3, 'up' => 0, 'degraded' => 0,
                'down' => 0, 'unknown' => 0, 'maint' => 0, 'loadSum' => 0, 'total' => 0];
            foreach ($overflowPools as $poolId => $pool) {
                $poolIndex[(int) $poolId] = 7;
                $overflow['tier'] = min($overflow['tier'], $pool['tier']);
                foreach (['up','degraded','down','unknown','maint'] as $key) $overflow[$key] += $pool[$key];
                $overflow['loadSum'] += $pool['loadSum'];
                $overflow['total'] += $pool['total'];
            }
            $overflow['loadPct'] = $overflow['total'] === 0 ? 0 : (int) round($overflow['loadSum'] / $overflow['total']);
            $pools[] = ['name' => 'OTHER', 'tier' => $overflow['tier'], 'up' => $overflow['up'],
                'degraded' => $overflow['degraded'], 'down' => $overflow['down'], 'unknown' => $overflow['unknown'],
                'maint' => $overflow['maint'], 'loadPct' => $overflow['loadPct'], 'total' => $overflow['total']];
        }
        usort($systemsAll, static fn (array $a, array $b): int =>
            [panelStateRank($a['state']), -$a['criticality'], -$a['loadPct'], $a['name']]
            <=> [panelStateRank($b['state']), -$b['criticality'], -$b['loadPct'], $b['name']]);
        $systems = [];
        $popTail = [];
        foreach ($systemsAll as $index => $system) {
            if (!isset($poolIndex[$system['poolId']])) {
                continue;
            }
            if ($index < 64) {
                $systems[] = ['name' => $system['name'], 'pool' => $poolIndex[$system['poolId']],
                    'tier' => $system['tier'], 'state' => $system['state'], 'loadPct' => $system['loadPct'],
                    'memPct' => $system['memPct'], 'diskPct' => $system['diskPct'], 'faults' => $system['faults'],
                    'rxKbps' => $system['rxKbps'], 'txKbps' => $system['txKbps']];
            } elseif (count($popTail) < 191) {
                $stateCode = match ($system['state']) { 'degraded' => 1, 'down' => 2, 'unknown' => 3, 'maint' => 4, default => 0 };
                $popTail[] = ($stateCode << 5) | ((int) floor($system['loadPct'] * 31 / 100));
            }
        }
        $stateRoll = ['up' => 0, 'degraded' => 0, 'down' => 0, 'unknown' => 0, 'maint' => 0];
        foreach ($systemsAll as $system) {
            $state = array_key_exists($system['state'], $stateRoll) ? $system['state'] : 'unknown';
            $stateRoll[$state]++;
        }
        $loadSum = 0;
        foreach ($systemsAll as $system) {
            $loadSum += $system['loadPct'];
        }

        /* A1 gear section: role-priority order (router/switch/hub/ap/wanBackup),
         * then gearId for determinism; 'other'-kind rows are dropped by
         * panelGearRole() returning null (stale SNMP test gear, e.g.). */
        $gearAll = [];
        $gearById = [];
        $portsByGear = [];
        foreach ($gearRows as $row) {
            $gearId = (string) $row['gearId'];
            if ((int) $row['ifIndex'] !== 0) {
                $portsByGear[$gearId][] = $row;
                continue;
            }
            $role = panelGearRole((string) $row['kind']);
            if ($role === null) {
                continue;
            }
            $gearById[$gearId] = $row;
            $gearAll[] = [
                'gearId' => $gearId, 'role' => $role,
                'state' => panelGearState($row['operStatus'] === null ? null : (int) $row['operStatus']),
                'rxLevel' => panelGearLogScale((int) $row['inRateKbps']),
                'txLevel' => panelGearLogScale((int) $row['outRateKbps']),
            ];
        }
        usort($gearAll, static fn (array $a, array $b): int => [$a['role'], $a['gearId']] <=> [$b['role'], $b['gearId']]);
        $gearCount = count($gearAll);
        if ($gearCount > 12) {
            /* E3 (review S7): the panel polls every 5 s, so per-request logging
             * is ~17k identical lines/day. APCu-gate to one line per hour;
             * without APCu, fall back to one per worker per size change. */
            static $lastLoggedCount = null;
            $logIt = function_exists('apcu_add')
                ? apcu_add('panelGearTruncLogged', 1, 3600)
                : ($lastLoggedCount !== $gearCount);
            if ($logIt) {
                $lastLoggedCount = $gearCount;
                error_log("[panel] gear section truncated from $gearCount to 12 entries");
            }
        }
        $gear = array_map(
            static fn (array $g): array => ['role' => $g['role'], 'state' => $g['state'],
                'rxLevel' => $g['rxLevel'], 'txLevel' => $g['txLevel']],
            array_slice($gearAll, 0, 12)
        );

        $links = [];
        $gearLinkIndex = [];
        $gateway = null;
        $backup = null;
        foreach ($gearById as $gearId => $row) {
            if (in_array((string) $row['kind'], ['gateway','router'], true) && $gateway === null) $gateway = [$gearId, $row];
            if ((string) $row['kind'] === 'wanBackup' && $backup === null) $backup = [$gearId, $row];
        }
        $wanUp = $wanDown = 0;
        if ($gateway !== null) {
            [$gearId, $row] = $gateway;
            $wanUp = max(0, (int) ($row['outRateKbps'] ?? 0));
            $wanDown = max(0, (int) ($row['inRateKbps'] ?? 0));
            $links[] = ['name' => 'INTERNET', 'kind' => 0,
                'state' => panelLinkState($row['operStatus'] === null ? null : (int) $row['operStatus']),
                'parent' => 255, 'upKbps' => $wanUp, 'downKbps' => $wanDown];
            $gearLinkIndex[$gearId] = 0;
        } else {
            $links[] = ['name' => 'INTERNET', 'kind' => 0, 'state' => 3, 'parent' => 255,
                'upKbps' => null, 'downKbps' => null];
        }
        if ($backup !== null) {
            [$gearId, $row] = $backup;
            $links[] = ['name' => 'BACKUP', 'kind' => 0,
                'state' => panelLinkState($row['operStatus'] === null ? null : (int) $row['operStatus']),
                'parent' => 255, 'upKbps' => max(0, (int) ($row['outRateKbps'] ?? 0)),
                'downKbps' => max(0, (int) ($row['inRateKbps'] ?? 0))];
            $gearLinkIndex[$gearId] = 1;
        }
        $candidateLinks = [];
        $lanUp = $lanDown = 0;
        foreach ($gearById as $gearId => $row) {
            foreach ($portsByGear[$gearId] ?? [] as $port) {
                if (in_array((string) $row['kind'], ['switch','hub'], true)
                    && (int) ($port['isUplink'] ?? 0) === 0 && (int) ($port['operStatus'] ?? 0) === 1) {
                    $lanUp = min(4294967295, $lanUp + max(0, (int) ($port['outRateKbps'] ?? 0)));
                    $lanDown = min(4294967295, $lanDown + max(0, (int) ($port['inRateKbps'] ?? 0)));
                }
            }
            $parentId = $row['uplinkGearId'] === null ? null : (string) $row['uplinkGearId'];
            if ($parentId === null || !isset($gearById[$parentId]) || in_array((string) $row['kind'], ['gateway','router','wanBackup'], true)) continue;
            $uplinkPorts = array_values(array_filter($portsByGear[$gearId] ?? [], static fn (array $p): bool => (int) ($p['isUplink'] ?? 0) === 1));
            if ($uplinkPorts === []) continue;
            $port = $uplinkPorts[0];
            $depth = 0;
            $walk = $parentId;
            $seen = [];
            while ($walk !== null && isset($gearById[$walk]) && !isset($seen[$walk])) {
                $seen[$walk] = true; $depth++;
                $next = $gearById[$walk]['uplinkGearId'] ?? null;
                $walk = $next === null ? null : (string) $next;
            }
            $candidateLinks[] = ['gearId' => $gearId, 'parentId' => $parentId, 'row' => $row,
                'port' => $port, 'depth' => $depth];
        }
        usort($candidateLinks, static fn (array $a, array $b): int => [$a['depth'], $a['gearId']] <=> [$b['depth'], $b['gearId']]);
        foreach ($candidateLinks as $candidate) {
            if (count($links) >= 12) break;
            $gearId = $candidate['gearId']; $port = $candidate['port']; $row = $candidate['row'];
            $parent = $gearLinkIndex[$candidate['parentId']] ?? 255;
            $gearLinkIndex[$gearId] = count($links);
            $links[] = ['name' => panelText($row['name'] ?? $gearId, 12),
                'kind' => (string) $row['kind'] === 'ap' ? 2 : 1,
                'state' => panelLinkState($port['operStatus'] === null ? null : (int) $port['operStatus']),
                'parent' => $parent, 'upKbps' => max(0, (int) ($port['outRateKbps'] ?? 0)),
                'downKbps' => max(0, (int) ($port['inRateKbps'] ?? 0))];
        }
        $hostLinks = $systemsAll;
        usort($hostLinks, static fn (array $a, array $b): int =>
            [-(int) $a['rxKbps'] - (int) $a['txKbps'], $a['name']]
            <=> [-(int) $b['rxKbps'] - (int) $b['txKbps'], $b['name']]);
        foreach ($hostLinks as $system) {
            if (count($links) >= 12) break;
            if ((int) $system['rxKbps'] === 0 && (int) $system['txKbps'] === 0) continue;
            $links[] = ['name' => $system['name'], 'kind' => 3, 'state' => panelStateCode($system['state']), 'parent' => 255,
                'upKbps' => $system['txKbps'], 'downKbps' => $system['rxKbps']];
        }

        $payload = [
            'ts' => (int) ($nowRow['ts'] ?? time()), 'score' => $score,
            'stateRoll' => $stateRoll, 'alerts' => $alerts,
            'meanLoadPct' => $systemsAll === [] ? 0 : (int) round($loadSum / count($systemsAll)),
            'rxKbps' => $rxKbps, 'txKbps' => $txKbps,
            'rttTenthMs' => $probeSummary['rttTenthMs'],
            'lossPermille' => $probeSummary['lossPermille'],
            'pools' => $pools, 'systems' => $systems, 'topAlert' => $topAlert,
            'popTail' => $popTail,
            'links' => $links,
            'alertList' => ['total' => min(255, $conditionTotal), 'faultCounts' => $faultCounts, 'items' => $alertListItems],
            'probes' => $probeSummary['probes'],
            'alertHeat' => $alertHeat,
            'totals' => ['wanUpKbps' => $wanUp, 'wanDownKbps' => $wanDown,
                'lanUpKbps' => $lanUp, 'lanDownKbps' => $lanDown],
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
            'gear' => $gear, 'gearCount' => min($gearCount, 12),
            'panelScreenConfig' => $panelScreenConfigRow === null ? null : [
                'screenCfg' => json_decode((string) $panelScreenConfigRow['screenCfg'], true),
                'flags' => (int) $panelScreenConfigRow['flags'],
                'reportedAt' => $panelScreenConfigRow['reportedAt'],
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
    }, ['auth' => 'session', 'csrf' => 'panel-service']);   // handler restricts to panel service principal

    /**
     * Upsert the panel's per-screen enable/weight CONFIG report (CONTRACT-AW §3.3).
     *
     * Input: authenticated panel service principal JSON
     *   {screenCfg:[{enabled,weightCode} x12], flags:int}.
     * Output: {panelScreenConfig} on success; 403 for any other principal.
     *
     * Migration 019 (panelScreenConfig) may not be applied live yet: this
     * route feature-detects the table and returns a soft no-op 200 rather
     * than a 500 when it is absent, matching the GET side's discipline.
     */
    $router->post('/api/panel/config', static function (): void {
        $principal = Auth::current();
        if (!panelIsServicePrincipal($principal ?? [])) {
            panelLogRejectedWrite('config', $principal);
            Response::error('forbidden', 'Only the local panel service principal may report config.', 403);
        }
        [$screenCfgJson, $flags] = panelConfigInput(solari_json_body());
        $hasScreenCfg = ((int) (Db::row(
            "SELECT COUNT(*) AS n FROM information_schema.TABLES
              WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'panelScreenConfig'"
        )['n'] ?? 0)) === 1;
        if (!$hasScreenCfg) {
            // Soft no-op: migration 019 not yet applied. Not a client error.
            Response::ok(['panelScreenConfig' => null]);
        }
        Db::exec(
            'INSERT INTO panelScreenConfig (configId, screenCfg, flags, reportedAt)
             VALUES (1, :screenCfg, :flags, NOW(6))
             ON DUPLICATE KEY UPDATE
                screenCfg = VALUES(screenCfg), flags = VALUES(flags), reportedAt = VALUES(reportedAt)',
            [':screenCfg' => $screenCfgJson, ':flags' => $flags]
        );
        $stored = Db::row(
            'SELECT screenCfg, flags, reportedAt FROM panelScreenConfig WHERE configId = 1'
        );
        Response::ok(['panelScreenConfig' => $stored === null ? null : [
            'screenCfg' => json_decode((string) $stored['screenCfg'], true),
            'flags' => (int) $stored['flags'],
            'reportedAt' => $stored['reportedAt'],
        ]]);
    }, ['auth' => 'session', 'csrf' => 'panel-service']);   // handler restricts to panel service principal
};
