<?php
declare(strict_types=1);

/**
 * DNS route module — per-server dig query/trace (hero), resolver/zone parity
 * health, and a read-only SoR record browser for the DNS dashboard page.
 *
 * STORAGE: zones/records/hosts views live only in the SoR (`sor` DB on
 * cesium) — there is no local dns_zones/dns_records table in `solarinet`.
 * All SoR reads go through Sor::db(); /api/dns/health additionally needs a
 * FAIL-SOFT path (Sor::db() 503s+exits on failure, which would take the whole
 * health check down even though the dig half is still useful), so that one
 * handler opens its own guarded PDO connection instead of calling Sor::db().
 *
 * SECURITY: every dig is run against a server-side IP whitelist
 * (DnsRoutes::servers()) keyed by opaque server id — the client can only ever
 * select an id, never supply an IP. $name/$type are validated against strict
 * whitelists/charsets before Dig ever sees them, and Dig itself shell-quotes
 * every argv element regardless.
 *
 * Routes (all GET, read-only; every handler also explicitly re-asserts the
 * session via Auth::requireSession(), on top of the blanket gate index.php
 * already applies to every non-/api/auth/* path):
 *   GET /api/dns/query                     hero: fan out one query to N servers
 *   GET /api/dns/health                    zone/RPZ parity across resolvers
 *   GET /api/dns/zones                     SoR zone list + record counts
 *   GET /api/dns/zones/{id}/records        SoR record browser for one zone
 *   GET /api/dns/hosts                     "next render" preview (v_dns_* views)
 *   GET /api/dns/rpz                       RPZ zone parity + local file stats
 *
 * Future CRUD (documented, NOT built here): POST /api/dns/zones/{id}/records,
 * PUT/DELETE /api/dns/records/{id} belong in routes_mutations.php, first line
 * Operator::requireOperator(), writes stamped source_id => Sor::dashboardSourceId(),
 * asserted_kind => 'human', is_generated => 0; rows with is_generated = 1 must
 * reject PUT/DELETE with 409 conflict ("IPAM-generated; edit the entity/IP
 * instead"). Propagation is then free via the existing CDC outbox -> MQ ->
 * sorsync -> BIND/AD/Pi-hole pipeline.
 */

require_once __DIR__ . '/../lib/Auth.php';
require_once __DIR__ . '/../lib/Sor.php';
require_once __DIR__ . '/../lib/Coerce.php';
require_once __DIR__ . '/../lib/Dig.php';

return static function (Router $router): void {

    // GET /api/dns/query?name=&type=&servers=
    $router->get('/api/dns/query', static function (): void {
        Auth::requireSession();

        $name      = DnsRoutes::requireName((string) ($_GET['name'] ?? ''));
        $type      = DnsRoutes::requireType((string) ($_GET['type'] ?? 'A'));
        $serverIds = DnsRoutes::requireServerIds(isset($_GET['servers']) ? (string) $_GET['servers'] : null);

        $servers = DnsRoutes::servers();
        $jobs    = [];
        foreach ($serverIds as $id) {
            $jobs[$id] = ['ip' => $servers[$id]['ip'], 'name' => $name, 'type' => $type];
        }
        $digResults = Dig::many($jobs, 2.0);

        $results = [];
        foreach ($serverIds as $id) {
            $r         = $digResults[$id];
            $results[] = [
                'server' => [
                    'id'    => $id,
                    'label' => $servers[$id]['label'],
                    'ip'    => $servers[$id]['ip'],
                    'kind'  => $servers[$id]['kind'],
                ],
                'status'      => $r['status'],
                'flags'       => $r['flags'],
                'answers'     => $r['answers'],
                'authority'   => $r['authority'],
                'queryTimeMs' => $r['queryTimeMs'],
                'rpzBlocked'  => Dig::isRpzBlocked($r),
                'error'       => $r['error'],
            ];
        }

        Response::ok([
            'name'    => $name,
            'type'    => $type,
            'ts'      => Response::nowIso8601Utc(),
            'results' => $results,
        ]);
    });

    // GET /api/dns/health
    $router->get('/api/dns/health', static function (): void {
        Auth::requireSession();

        $sor   = DnsRoutes::zonesFailSoft();
        $zones = $sor['zones'];

        $servers = DnsRoutes::servers();
        $jobs    = [];
        foreach ($zones as $zone) {
            foreach (DnsRoutes::expectedServersForZone($zone) as $sid) {
                $jobs[$zone['name'] . '|' . $sid] = [
                    'ip'   => $servers[$sid]['ip'],
                    'name' => $zone['name'],
                    'type' => 'SOA',
                ];
            }
        }
        $digResults = Dig::many($jobs, 2.0);

        $reachable = ['xenon' => false, 'steel' => false, 'radium' => false];
        foreach ($digResults as $key => $r) {
            [, $sid] = explode('|', $key, 2);
            if (isset($reachable[$sid]) && $r['status'] !== 'TIMEOUT') {
                $reachable[$sid] = true;
            }
        }

        $zoneOut = [];
        foreach ($zones as $zone) {
            $expected = DnsRoutes::expectedServersForZone($zone);
            $soa      = [];
            $serials  = [];
            $allNoErr = true;
            foreach ($expected as $sid) {
                $r      = $digResults[$zone['name'] . '|' . $sid];
                $serial = DnsRoutes::parseSoaSerial($r);
                $soa[$sid] = [
                    'serial'      => $serial,
                    'status'      => $r['status'],
                    'queryTimeMs' => $r['queryTimeMs'],
                ];
                $serials[$sid] = $serial;
                if ($r['status'] !== 'NOERROR') {
                    $allNoErr = false;
                }
            }
            $nonNull       = array_filter($serials, static fn ($v) => $v !== null);
            $uniqueSerials = array_unique($nonNull);
            // In sync only if EVERY expected server actually returned a serial and
            // they all match — a NOERROR reply with no parseable serial (null) must
            // not be silently dropped into a false "in sync".
            $parity        = $allNoErr && $expected !== []
                             && count($nonNull) === count($expected)
                             && count($uniqueSerials) === 1;

            $zoneOut[] = [
                'name'            => $zone['name'],
                'kind'            => $zone['kind'],
                'authority'       => $zone['authority'],
                'expectedServers' => $expected,
                'soa'             => $soa,
                'parity'          => $parity,
            ];
        }

        $serversOut = [];
        foreach (['xenon', 'steel', 'radium'] as $sid) {
            $serversOut[] = [
                'id'        => $sid,
                'label'     => $servers[$sid]['label'],
                'ip'        => $servers[$sid]['ip'],
                'reachable' => $reachable[$sid],
            ];
        }

        Response::ok([
            'sorAvailable' => $sor['available'],
            'servers'      => $serversOut,
            'zones'        => $zoneOut,
            'rpz'          => DnsRoutes::fetchRpz(),
        ]);
    });

    // GET /api/dns/zones
    $router->get('/api/dns/zones', static function (): void {
        Auth::requireSession();

        $rows = DnsRoutes::sorRows(
            "SELECT z.id, z.name, z.kind, z.authority, e.name AS primary_ns,
                    z.default_ttl, z.hostmaster, z.notes, z.updated_at,
                    COALESCE(rc.record_count, 0)    AS record_count,
                    COALESCE(rc.generated_count, 0) AS generated_count
               FROM dns_zones z
               LEFT JOIN entities e ON e.id = z.primary_ns_entity_id AND e.deleted_at IS NULL
               LEFT JOIN (
                     SELECT zone_id,
                            COUNT(*)               AS record_count,
                            SUM(is_generated)       AS generated_count
                       FROM dns_records
                      WHERE deleted_at IS NULL
                      GROUP BY zone_id
                   ) rc ON rc.zone_id = z.id
              WHERE z.deleted_at IS NULL
              ORDER BY z.name"
        );

        Response::ok(array_map(static function (array $r): array {
            $recordCount    = Coerce::int($r['record_count']) ?? 0;
            $generatedCount = Coerce::int($r['generated_count']) ?? 0;
            return [
                'id'              => Coerce::id($r['id']),
                'name'            => $r['name'],
                'kind'            => $r['kind'],
                'authority'       => $r['authority'],
                'primaryNs'       => $r['primary_ns'],
                'defaultTtl'      => Coerce::int($r['default_ttl']),
                'hostmaster'      => $r['hostmaster'],
                'recordCount'     => $recordCount,
                'generatedCount'  => $generatedCount,
                'explicitCount'   => $recordCount - $generatedCount,
                'updatedAt'       => Coerce::iso($r['updated_at']),
                'notes'           => $r['notes'],
            ];
        }, $rows));
    });

    // GET /api/dns/zones/{id}/records?type=&q=&generated=&limit=
    $router->get('/api/dns/zones/{id}/records', static function (array $p): void {
        Auth::requireSession();

        if (!ctype_digit((string) $p['id'])) {
            Response::error('bad_request', 'zone id must be numeric.', 400);
        }
        $zoneId = (string) $p['id'];

        $zone = DnsRoutes::sorRow('SELECT id, name FROM dns_zones WHERE id = :i AND deleted_at IS NULL', [':i' => $zoneId]);
        if ($zone === null) {
            Response::error('not_found', "No zone {$zoneId}", 404);
        }

        $where  = ['zone_id = :zone_id', 'deleted_at IS NULL'];
        $params = [':zone_id' => $zoneId];

        if (isset($_GET['type']) && $_GET['type'] !== '') {
            $where[]          = 'rrtype = :type';
            $params[':type']  = DnsRoutes::requireType((string) $_GET['type']);
        }
        if (isset($_GET['q']) && trim((string) $_GET['q']) !== '') {
            $where[]         = '(name LIKE :q OR rdata LIKE :q)';
            $params[':q']    = '%' . trim((string) $_GET['q']) . '%';
        }
        if (isset($_GET['generated']) && $_GET['generated'] !== '') {
            if ($_GET['generated'] !== '0' && $_GET['generated'] !== '1') {
                Response::error('bad_request', 'generated must be 0 or 1.', 400);
            }
            $where[]              = 'is_generated = :generated';
            $params[':generated'] = (int) $_GET['generated'];
        }

        $limit = 500;
        if (isset($_GET['limit']) && $_GET['limit'] !== '') {
            if (!ctype_digit((string) $_GET['limit'])) {
                Response::error('bad_request', 'limit must be a non-negative integer.', 400);
            }
            $limit = min(2000, max(1, (int) $_GET['limit']));
        }

        $whereSql = implode(' AND ', $where);
        $total    = (int) (DnsRoutes::sorScalar("SELECT COUNT(*) FROM dns_records WHERE {$whereSql}", $params) ?? 0);

        $rows = DnsRoutes::sorRows(
            "SELECT id, name, rrtype, rdata, ttl, priority, is_generated,
                    ip_address_id, target_entity_id, asserted_kind, updated_at
               FROM dns_records
              WHERE {$whereSql}
              ORDER BY name, rrtype, rdata
              LIMIT {$limit}",
            $params
        );

        $records = array_map(static function (array $r): array {
            return [
                'id'             => Coerce::id($r['id']),
                'name'           => $r['name'],
                'rrtype'         => $r['rrtype'],
                'rdata'          => $r['rdata'],
                'ttl'            => Coerce::int($r['ttl']),
                'priority'       => Coerce::int($r['priority']),
                'isGenerated'    => Coerce::bool($r['is_generated']),
                'ipAddressId'    => Coerce::id($r['ip_address_id']),
                'targetEntityId' => Coerce::id($r['target_entity_id']),
                'assertedKind'   => $r['asserted_kind'],
                'updatedAt'      => Coerce::iso($r['updated_at']),
            ];
        }, $rows);

        Response::ok([
            'zone'       => ['id' => Coerce::id($zone['id']), 'name' => $zone['name']],
            'records'    => $records,
            'total'      => $total,
            'truncated'  => $total > count($records),
        ]);
    });

    // GET /api/dns/hosts
    $router->get('/api/dns/hosts', static function (): void {
        Auth::requireSession();

        $hosts  = Sor::db()->query('SELECT host, ip, role, lifecycle FROM v_dns_hosts')->fetchAll();
        $ifaces = Sor::db()->query('SELECT label, ip FROM v_dns_ifaces')->fetchAll();
        $cnames = Sor::db()->query('SELECT alias, target_host FROM v_dns_cnames')->fetchAll();

        Response::ok([
            'hosts'  => array_map(static fn (array $r): array => [
                'host'      => $r['host'],
                'ip'        => $r['ip'],
                'role'      => $r['role'],
                'lifecycle' => $r['lifecycle'],
            ], $hosts),
            'ifaces' => array_map(static fn (array $r): array => [
                'label' => $r['label'],
                'ip'    => $r['ip'],
            ], $ifaces),
            'cnames' => array_map(static fn (array $r): array => [
                'alias'      => $r['alias'],
                'targetHost' => $r['target_host'],
            ], $cnames),
        ]);
    });

    // GET /api/dns/rpz
    $router->get('/api/dns/rpz', static function (): void {
        Auth::requireSession();
        Response::ok(DnsRoutes::fetchRpz());
    });
};

/**
 * File-local helpers for the DNS route module: the server registry, input
 * validation, the SoR fail-soft zone lookup used only by /api/dns/health, and
 * shared SOA/RPZ computation. Kept in a final class so the route closures can
 * reach them by name without leaking free functions into the global namespace.
 */
final class DnsRoutes
{
    /** Type whitelist accepted anywhere a $type query param is read. */
    private const TYPES = ['A', 'AAAA', 'CNAME', 'PTR', 'SRV', 'TXT', 'MX', 'NS', 'SOA', 'CAA', 'ANY'];

    private static ?array $serversCache = null;
    private static ?array $rpzFileCache = null;

    /**
     * Server-side resolver registry, keyed by opaque id. IPs never come from
     * the client — only DNS_SERVERS['id']['ip'] is ever handed to Dig.
     */
    public static function servers(): array
    {
        if (self::$serversCache === null) {
            self::$serversCache = [
                'steel'    => ['label' => 'steel',   'ip' => '10.0.0.11', 'kind' => 'internal'],
                'xenon'    => ['label' => 'xenon',   'ip' => '10.0.0.20', 'kind' => 'internal'],
                'radium'   => ['label' => 'radium',  'ip' => '10.1.0.10', 'kind' => 'internal'],
                'external' => ['label' => 'control', 'ip' => (getenv('SOLARI_DNS_EXTERNAL') ?: '1.1.1.1'), 'kind' => 'external'],
            ];
        }
        return self::$serversCache;
    }

    /** Validate a query-name param; 400s (exits) on failure. */
    public static function requireName(string $name): string
    {
        if (preg_match('/^(?=.{1,253}$)[A-Za-z0-9]([A-Za-z0-9._-]*[A-Za-z0-9.])?$/', $name) !== 1) {
            Response::error('bad_request', 'name must be a valid hostname/FQDN.', 400);
        }
        return $name;
    }

    /** Validate + uppercase a record-type param; 400s (exits) on failure. */
    public static function requireType(string $type): string
    {
        $type = strtoupper(trim($type));
        if (!in_array($type, self::TYPES, true)) {
            Response::error('bad_request', 'type must be one of: ' . implode(', ', self::TYPES), 400);
        }
        return $type;
    }

    /**
     * Validate a comma-separated servers= param against the registry;
     * defaults to all four known ids when omitted. 400s (exits) on any
     * unknown id — never falls back to a caller-supplied IP.
     *
     * @return list<string>
     */
    public static function requireServerIds(?string $csv): array
    {
        $known = array_keys(self::servers());
        if ($csv === null || trim($csv) === '') {
            return $known;
        }
        $ids = array_filter(array_map('trim', explode(',', $csv)), static fn ($v) => $v !== '');
        if ($ids === []) {
            return $known;
        }
        foreach ($ids as $id) {
            if (!in_array($id, $known, true)) {
                Response::error('bad_request', "Unknown server id \"{$id}\". Valid ids: " . implode(', ', $known), 400);
            }
        }
        return array_values($ids);
    }

    /** Which server ids should serve a given zone row (name/authority). */
    public static function expectedServersForZone(array $zone): array
    {
        if ($zone['name'] === 'rpz.akoria') {
            return ['xenon', 'steel'];
        }
        return $zone['authority'] === 'external' ? ['radium'] : ['xenon', 'steel'];
    }

    /**
     * Fail-soft zone lookup for /api/dns/health only: guards Sor::enabled()
     * and opens its own PDO try/catch instead of Sor::db() (which 503s+exits
     * on failure) so a down SoR degrades this one endpoint instead of killing
     * it — the dig half of the health check still has value on its own.
     *
     * @return array{available:bool, zones:array<int,array{name:string,kind:string,authority:string}>}
     */
    public static function zonesFailSoft(): array
    {
        if (!Sor::enabled()) {
            return ['available' => false, 'zones' => self::fallbackZones()];
        }
        try {
            $host = getenv('SOR_DB_HOST') ?: '10.1.0.200';
            $port = getenv('SOR_DB_PORT') ?: '3306';
            $name = getenv('SOR_DB_NAME') ?: 'sor';
            $dsn  = "mysql:host={$host};port={$port};dbname={$name};charset=utf8mb4";
            $pdo  = new PDO($dsn, (string) getenv('SOR_DB_USER'), (string) (getenv('SOR_DB_PASS') ?: ''), [
                PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
                PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
                PDO::ATTR_TIMEOUT            => 4,
            ]);
            $rows = $pdo->query('SELECT name, kind, authority FROM dns_zones WHERE deleted_at IS NULL')->fetchAll();
            return ['available' => true, 'zones' => $rows];
        } catch (\Throwable $e) {
            error_log('DnsRoutes::zonesFailSoft SoR unavailable: ' . $e->getMessage());
            return ['available' => false, 'zones' => self::fallbackZones()];
        }
    }

    /** Hardcoded degrade target when the SoR is unreachable for /api/dns/health. */
    private static function fallbackZones(): array
    {
        return [
            ['name' => 'akoria.net', 'kind' => 'forward', 'authority' => 'sor_rendered'],
            ['name' => 'akoria.org', 'kind' => 'forward', 'authority' => 'external'],
        ];
    }

    /** Extract the serial (field 3) from a NOERROR SOA dig result. */
    public static function parseSoaSerial(array $digResult): ?int
    {
        if ($digResult['status'] !== 'NOERROR') {
            return null;
        }
        foreach (array_merge($digResult['answers'], $digResult['authority']) as $rec) {
            if (strtoupper((string) ($rec['type'] ?? '')) === 'SOA') {
                $fields = preg_split('/\s+/', trim((string) $rec['data'])) ?: [];
                if (isset($fields[2]) && ctype_digit($fields[2])) {
                    return (int) $fields[2];
                }
            }
        }
        return null;
    }

    /** rpz.akoria SOA parity (xenon/steel) + local zone-file stats. Shared by /api/dns/health and /api/dns/rpz. */
    public static function fetchRpz(): array
    {
        $servers = self::servers();
        $jobs    = [
            'xenon' => ['ip' => $servers['xenon']['ip'], 'name' => 'rpz.akoria', 'type' => 'SOA'],
            'steel' => ['ip' => $servers['steel']['ip'], 'name' => 'rpz.akoria', 'type' => 'SOA'],
        ];
        $digResults = Dig::many($jobs, 2.0);

        $serials = [];
        foreach (['xenon', 'steel'] as $sid) {
            $r             = $digResults[$sid];
            $serials[$sid] = ['serial' => self::parseSoaSerial($r), 'status' => $r['status']];
        }
        $parity = $serials['xenon']['status'] === 'NOERROR'
            && $serials['steel']['status'] === 'NOERROR'
            && $serials['xenon']['serial'] !== null
            && $serials['xenon']['serial'] === $serials['steel']['serial'];

        $file = self::rpzFileInfo();

        return [
            'zone'         => 'rpz.akoria',
            'serials'      => $serials,
            'parity'       => $parity,
            'blockedCount' => $file['blockedCount'],
            'lastRefresh'  => $file['lastRefresh'],
            'sourceUrl'    => 'https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts',
        ];
    }

    /**
     * Local RPZ zone-file inspection: count `CNAME .` policy lines + mtime.
     * Fail-soft to nulls when the file is missing/unreadable — never throws.
     * Cached per request (the dashboard fpm pool runs on xenon, so the file
     * is normally local; other pools simply always get the null shape).
     *
     * @return array{blockedCount:?int, lastRefresh:?string}
     */
    private static function rpzFileInfo(): array
    {
        if (self::$rpzFileCache !== null) {
            return self::$rpzFileCache;
        }
        $path = getenv('SOLARI_RPZ_ZONE_FILE') ?: '/etc/bind/zones/db.rpz.akoria';
        if (!is_file($path) || !is_readable($path)) {
            return self::$rpzFileCache = ['blockedCount' => null, 'lastRefresh' => null];
        }
        $fh = @fopen($path, 'r');
        if ($fh === false) {
            return self::$rpzFileCache = ['blockedCount' => null, 'lastRefresh' => null];
        }
        $count = 0;
        while (($line = fgets($fh)) !== false) {
            if (preg_match('/\bCNAME\s+\.\s*$/i', rtrim($line)) === 1) {
                $count++;
            }
        }
        fclose($fh);

        $mtime       = @filemtime($path);
        $lastRefresh = $mtime !== false ? Coerce::iso(gmdate('Y-m-d H:i:s', $mtime)) : null;

        return self::$rpzFileCache = ['blockedCount' => $count, 'lastRefresh' => $lastRefresh];
    }

    // ---- thin Sor::db() wrappers (mirrors the Inv:: pattern in inventory.php) ----

    public static function sorRows(string $sql, array $params = []): array
    {
        $st = Sor::db()->prepare($sql);
        $st->execute($params);
        return $st->fetchAll();
    }

    public static function sorRow(string $sql, array $params = []): ?array
    {
        $st = Sor::db()->prepare($sql);
        $st->execute($params);
        $row = $st->fetch();
        return $row === false ? null : $row;
    }

    public static function sorScalar(string $sql, array $params = [])
    {
        $st = Sor::db()->prepare($sql);
        $st->execute($params);
        $v = $st->fetchColumn();
        return $v === false ? null : $v;
    }
}
