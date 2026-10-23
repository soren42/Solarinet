<?php
declare(strict_types=1);

/**
 * _rw_worker.php — subprocess helper for test_rackwire_api.php.
 *
 * NOT a standalone test (no "test_" prefix, never invoked directly by CI).
 * Response::ok()/Response::error() both call exit(), so the only way to
 * observe one HTTP-shaped response per case without killing the whole test
 * run is to run each case in its own PHP process. test_rackwire_api.php
 * spawns this file once per case via proc_open, driving it entirely through
 * environment variables (RW_METHOD/RW_PATH/RW_QUERY/RW_BODY/RW_AUTH/RW_ROLE/
 * RW_SCENARIO) and reads the result back off stdout (the JSON envelope,
 * exactly as Response::send() emits it) and stderr (a trailing "STATUS:nnn"
 * line written by a shutdown handler, since exit() runs before control would
 * otherwise return to a caller).
 *
 * Sor::db()'s real PDO is swapped for FakePdo (below) via reflection on
 * Sor's private static $pdo — this is the "stubbed DB/Sor layer" the WP-4
 * assignment calls for. No real DB is ever contacted.
 */

error_reporting(E_ALL);
ini_set('display_errors', '0');
ini_set('log_errors', '0');

$ROOT = dirname(__DIR__, 2);

require $ROOT . '/dashboard/api/lib/Response.php';
require $ROOT . '/dashboard/api/lib/Router.php';
require $ROOT . '/dashboard/api/lib/Coerce.php';
require $ROOT . '/dashboard/api/lib/Auth.php';
require $ROOT . '/dashboard/api/lib/Operator.php';
require $ROOT . '/dashboard/api/lib/Sor.php';

// solari_json_body() normally reads php://input (lib/bootstrap.php), which is
// not fed from stdin under the CLI SAPI. We do not load bootstrap.php at all
// (its exception/error handlers are irrelevant here) and instead define our
// own copy that reads the RW_BODY env var, so rackwire.php's real handlers
// run unmodified against a body the test controls.
function solari_json_body(): array
{
    $raw = getenv('RW_BODY');
    if ($raw === false || $raw === '') {
        return [];
    }
    $decoded = json_decode($raw, true);
    return is_array($decoded) ? $decoded : [];
}

// ---------------------------------------------------------------- FakePdo --

/**
 * FakeStatement — a PDOStatement that never touches a real connection.
 * Constructed only via newInstanceWithoutConstructor() (PDOStatement has no
 * public constructor); execute() asks the owning FakePdo to resolve the
 * prepared SQL text against the active scenario's canned result set.
 */
final class RwFakeStatement extends PDOStatement
{
    /** @var array<int,array<string,mixed>> */
    private array $rows = [];
    private int $affected = 0;
    private string $sql = '';
    private ?RwFakePdo $owner = null;

    public static function make(string $sql, RwFakePdo $owner): self
    {
        /** @var self $st */
        $st = (new ReflectionClass(self::class))->newInstanceWithoutConstructor();
        $st->sql   = $sql;
        $st->owner = $owner;
        return $st;
    }

    public function execute(?array $params = null): bool
    {
        [$this->rows, $this->affected] = $this->owner->resolve($this->sql, $params ?? []);
        return true;
    }

    public function fetchAll(int $mode = PDO::FETCH_ASSOC, ...$args): array|false
    {
        return $this->rows;
    }

    public function fetch(int $mode = PDO::FETCH_ASSOC, ...$args): mixed
    {
        return $this->rows[0] ?? false;
    }

    public function fetchColumn(int $column = 0): mixed
    {
        if (!isset($this->rows[0])) {
            return false;
        }
        $vals = array_values($this->rows[0]);
        return $vals[$column] ?? false;
    }

    public function rowCount(): int
    {
        return $this->affected;
    }
}

/**
 * FakePdo — a PDO that never connects. $resolvers is an ordered list of
 * {match: normalized-SQL substring, rows: array, affected?: int, insertId?:
 * int|string}; the FIRST entry whose `match` appears in the whitespace-
 * normalized query text wins (put more specific matches earlier). An
 * unmatched query is a scenario-authoring bug, not a legitimate "no rows"
 * result — it would otherwise mean the route ran SQL the test never
 * anticipated (wrong table, missing WHERE, a write the scenario forgot to
 * stub) and still passed. resolve() throws rather than silently returning
 * an empty set, so such a mismatch fails the worker loudly instead of
 * letting a case pass by accident.
 */
final class RwFakePdo extends PDO
{
    /** @var array<int,array{match:string,rows?:array,affected?:int,insertId?:int|string}> */
    private array $resolvers;
    private string $insertId = '1';
    private bool $inTx = false;
    /** @var list<array{sql:string,params:array<string,mixed>}> */
    public array $executed = [];

    public function __construct(array $resolvers)
    {
        $this->resolvers = $resolvers;
    }

    public function prepare(string $query, array $options = []): PDOStatement|false
    {
        return RwFakeStatement::make($query, $this);
    }

    /** @return array{0:array<int,array<string,mixed>>,1:int} [rows, affectedRowCount] */
    public function resolve(string $sql, array $params): array
    {
        $this->executed[] = ['sql' => $sql, 'params' => $params];
        $norm = preg_replace('/\s+/', ' ', trim($sql)) ?? $sql;
        if ($norm === 'SET SESSION innodb_lock_wait_timeout = 5') {
            return [[], 0];
        }
        foreach ($this->resolvers as $r) {
            if (str_contains($norm, $r['match'])) {
                if (isset($r['throw'])) {
                    $error = new PDOException((string) ($r['message'] ?? 'simulated database error'), (int) ($r['driverCode'] ?? 0));
                    $error->errorInfo = [(string) ($r['sqlState'] ?? 'HY000'), (int) ($r['driverCode'] ?? 0), (string) ($r['message'] ?? 'simulated database error')];
                    throw $error;
                }
                if (isset($r['insertId'])) {
                    $this->insertId = (string) $r['insertId'];
                }
                return [$r['rows'] ?? [], $r['affected'] ?? count($r['rows'] ?? [])];
            }
        }
        // Unmatched SQL is a hard failure, not an empty result -- see the
        // class doc comment. Written to stderr (display_errors is off) so a
        // failing case's raw query text is visible in the CI log.
        fwrite(STDERR, "RwFakePdo: unmatched query, no scenario stub for it:\n  $norm\n");
        throw new RuntimeException('RwFakePdo: unmatched query — ' . $norm);
    }

    public function lastInsertId(?string $name = null): string|false
    {
        return $this->insertId;
    }

    public function beginTransaction(): bool
    {
        $this->inTx = true;
        return true;
    }

    public function commit(): bool
    {
        $this->inTx = false;
        return true;
    }

    public function rollBack(): bool
    {
        $this->inTx = false;
        return true;
    }

    public function inTransaction(): bool
    {
        return $this->inTx;
    }
}

// ---------------------------------------------------------------- scenarios --

/** @return array<int,array{match:string,rows?:array,affected?:int,insertId?:int|string}> */
function rw_worker_scenario(string $name): array
{
    $sourceRow = ['rows' => [['id' => 7]], 'match' => "FROM sources WHERE slug = 'rackwire'"];

    switch ($name) {
        case 'empty-list':
            return [
                ['match' => 'FROM rw_plans WHERE deleted_at IS NULL ORDER BY updated_at DESC', 'rows' => []],
            ];
        case 'plan-list-two':
            return [
                ['match' => 'FROM rw_plans WHERE deleted_at IS NULL ORDER BY updated_at DESC', 'rows' => [
                    ['id' => 1, 'name' => 'Rack A', 'slug' => 'rack-a', 'updated_at' => '2026-08-01 00:00:00', 'created_at' => '2026-07-01 00:00:00'],
                    ['id' => 2, 'name' => 'Rack B', 'slug' => 'rack-b', 'updated_at' => '2026-08-02 00:00:00', 'created_at' => '2026-07-02 00:00:00'],
                ]],
            ];
        case 'plan-found':
            return [
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL', 'rows' => [
                    ['id' => 5, 'name' => 'Rack A', 'slug' => 'rack-a', 'plan_json' => '{"devices":[],"cables":[]}',
                     'thumbnail' => null, 'updated_at' => '2026-08-01 00:00:00', 'created_at' => '2026-07-01 00:00:00'],
                ]],
            ];
        case 'plan-not-found':
            return [
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL', 'rows' => []],
            ];
        case 'library-list':
            return [
                ['match' => 'FROM hardware_models hm', 'rows' => [
                    ['id' => 3, 'make' => 'Ubiquiti', 'model' => 'USW-Pro-24', 'hw_type' => 'switch', 'notes' => null,
                     'groups_json' => null, 'device_json' => null, 'unitCount' => 2],
                ]],
            ];
        case 'sor-links':
            return [
                ['match' => 'FROM interconnects ic', 'rows' => [[
                    'id' => 30, 'link_kind' => 'fiber', 'label' => 'uplink', 'length_m' => '2.50',
                    'media_standard' => 'om3', 'jacket_color' => 'aqua', 'notes' => 'to core',
                    'asserted_kind' => 'human', 'asserted_at' => '2026-10-22 10:00:00', 'sourceSlug' => 'rackwire',
                    'aName' => 'switch-a', 'bName' => 'switch-b', 'aIfName' => 'Ethernet1/0/1', 'bIfName' => 'Ethernet1/0/2',
                    'aAssetTag' => 'SI-0001', 'bAssetTag' => 'SI-0002',
                ]]],
            ];
        case 'create-plan':
            return [
                $sourceRow,
                ['match' => 'INSERT INTO rw_plans', 'rows' => [], 'affected' => 1, 'insertId' => 555],
                ['match' => 'FROM rw_plans WHERE id = :id', 'rows' => [
                    ['id' => 555, 'name' => 'New Rack', 'slug' => 'new-rack', 'plan_json' => '{"devices":[],"cables":[]}',
                     'thumbnail' => null, 'updated_at' => '2026-08-07 00:00:00', 'created_at' => '2026-08-07 00:00:00'],
                ]],
            ];
        // The write scenarios below stub each ownership-scoped UPDATE with its
        // FULL "... AND source_id = :src" WHERE clause as the match string, so
        // an UPDATE that lost its source_id scoping stops matching and
        // resolve() throws — the scoping is asserted by the stub itself, not
        // just by the status code.
        case 'update-plan':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [
                    ['id' => 5, 'source_id' => 7, 'name' => 'Rack A', 'plan_json' => '{"devices":[],"cables":[]}', 'thumbnail' => null, 'notes' => null],
                ]],
                ['match' => 'WHERE id = :id AND deleted_at IS NULL AND source_id = :src', 'rows' => [], 'affected' => 1],
                ['match' => 'FROM rw_plans WHERE id = :id', 'rows' => [
                    ['id' => 5, 'name' => 'Rack A2', 'slug' => 'rack-a2', 'plan_json' => '{"devices":[],"cables":[]}',
                     'thumbnail' => null, 'updated_at' => '2026-08-07 00:00:00', 'created_at' => '2026-07-01 00:00:00'],
                ]],
            ];
        case 'update-plan-stale':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [
                    ['id' => 5, 'source_id' => 7, 'name' => 'Rack A', 'plan_json' => '{"devices":[{"uid":"newer"}],"cables":[]}', 'thumbnail' => null, 'notes' => null],
                ]],
            ];
        case 'update-plan-lock-timeout':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'throw' => true,
                 'driverCode' => 1205, 'sqlState' => 'HY000', 'message' => 'Lock wait timeout exceeded'],
            ];
        case 'update-plan-idempotent':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [[
                    'id' => 5, 'source_id' => 7, 'name' => 'Rack A2', 'slug' => 'rack-a2', 'plan_json' => '{"devices":[],"cables":[]}',
                    'thumbnail' => null, 'notes' => null,
                ]]],
                ['match' => 'FROM rw_plans WHERE id = :id', 'rows' => [[
                    'id' => 5, 'name' => 'Rack A2', 'slug' => 'rack-a2', 'plan_json' => '{"devices":[],"cables":[]}',
                    'thumbnail' => null, 'notes' => null, 'updated_at' => '2026-08-07 00:00:00', 'created_at' => '2026-07-01 00:00:00',
                ]]],
            ];
        case 'update-plan-no-hash':
            return [$sourceRow];
        case 'restore-plan-stale':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [
                    ['id' => 5, 'source_id' => 7, 'name' => 'Rack A', 'plan_json' => '{"devices":[{"uid":"newer"}],"cables":[]}', 'thumbnail' => null, 'notes' => null],
                ]],
                ['match' => 'FROM rw_plan_versions WHERE id = :vid AND plan_id = :pid', 'rows' => [['id' => 77, 'plan_json' => '{"devices":[{"uid":"restored"}],"cables":[]}']]],
            ];
        case 'restore-plan-deadlock':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'throw' => true,
                 'driverCode' => 1213, 'sqlState' => '40001', 'message' => 'Deadlock found when trying to get lock'],
            ];
        // Update and restore validate ownership inside their row-locking CAS
        // query. Delete and version still use the ordinary write guard query.
        case 'plan-for-write-foreign':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [['id' => 5, 'source_id' => 99, 'name' => 'Rack A', 'plan_json' => '{"devices":[],"cables":[]}', 'thumbnail' => null, 'notes' => null]]],
            ];
        case 'plan-foreign':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL', 'rows' => [['id' => 5, 'source_id' => 99]]],
            ];
        case 'restore-plan-foreign':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [['id' => 5, 'source_id' => 99, 'name' => 'Rack A', 'plan_json' => '{"devices":[],"cables":[]}', 'thumbnail' => null, 'notes' => null]]],
            ];
        case 'restore-plan-success':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [
                    ['id' => 5, 'source_id' => 7, 'name' => 'Rack A', 'plan_json' => '{"devices":[],"cables":[]}', 'thumbnail' => null, 'notes' => null],
                ]],
                ['match' => 'FROM rw_plan_versions WHERE id = :vid AND plan_id = :pid', 'rows' => [['id' => 77, 'plan_json' => '{"devices":[{"uid":"restored"}],"cables":[]}']]],
                ['match' => 'UPDATE rw_plans SET plan_json = :pj', 'rows' => [], 'affected' => 1],
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL', 'rows' => [[
                    'id' => 5, 'name' => 'Rack A', 'slug' => 'rack-a', 'plan_json' => '{"devices":[{"uid":"restored"}],"cables":[]}',
                    'thumbnail' => null, 'notes' => null, 'updated_at' => '2026-08-07 00:00:00', 'created_at' => '2026-07-01 00:00:00',
                ]]],
            ];
        case 'restore-plan-idempotent':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [[
                    'id' => 5, 'source_id' => 7, 'name' => 'Rack A', 'plan_json' => '{"devices":[{"uid":"restored"}],"cables":[]}', 'thumbnail' => null, 'notes' => null,
                ]]],
                ['match' => 'FROM rw_plan_versions WHERE id = :vid AND plan_id = :pid', 'rows' => [['id' => 77, 'plan_json' => '{"devices":[{"uid":"restored"}],"cables":[]}']]],
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL', 'rows' => [[
                    'id' => 5, 'name' => 'Rack A', 'slug' => 'rack-a', 'plan_json' => '{"devices":[{"uid":"restored"}],"cables":[]}',
                    'thumbnail' => null, 'notes' => null, 'updated_at' => '2026-08-07 00:00:00', 'created_at' => '2026-07-01 00:00:00',
                ]]],
            ];
        case 'delete-found':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL', 'rows' => [['id' => 5, 'source_id' => 7]]],
                ['match' => 'UPDATE rw_plans SET deleted_at = NOW() WHERE id = :id AND deleted_at IS NULL AND source_id = :src', 'rows' => [], 'affected' => 1],
            ];
        case 'delete-lock-timeout':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL', 'rows' => [['id' => 5, 'source_id' => 7]]],
                ['match' => 'UPDATE rw_plans SET deleted_at = NOW() WHERE id = :id AND deleted_at IS NULL AND source_id = :src',
                 'throw' => true, 'driverCode' => 1205, 'sqlState' => 'HY000', 'message' => 'Lock wait timeout exceeded'],
            ];
        case 'delete-not-found':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL', 'rows' => []],
            ];
        case 'save-version':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [
                    ['id' => 5, 'source_id' => 7, 'plan_json' => '{"devices":[],"cables":[]}'],
                ]],
                ['match' => 'INSERT INTO rw_plan_versions', 'rows' => [], 'affected' => 1, 'insertId' => 77],
                ['match' => 'SELECT COUNT(*) FROM rw_plan_versions WHERE plan_id = :pid', 'rows' => [['c' => 3]]],
                ['match' => 'FROM rw_plan_versions WHERE id = :id', 'rows' => [
                    ['id' => 77, 'plan_id' => 5, 'version_name' => 'before rewire',
                     'created_by' => 'tester', 'created_at' => '2026-08-07 12:00:00'],
                ]],
            ];
        case 'save-version-lock-timeout':
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'throw' => true,
                 'driverCode' => 1205, 'sqlState' => 'HY000', 'message' => 'Lock wait timeout exceeded'],
            ];
        case 'save-version-prune':
            // 27 rows on file: the cap-25 prune must run (and must run against
            // this stub) or the DELETE would be an unmatched query.
            return [
                $sourceRow,
                ['match' => 'FROM rw_plans WHERE id = :id AND deleted_at IS NULL FOR UPDATE', 'rows' => [
                    ['id' => 5, 'source_id' => 7, 'plan_json' => '{"devices":[],"cables":[]}'],
                ]],
                ['match' => 'INSERT INTO rw_plan_versions', 'rows' => [], 'affected' => 1, 'insertId' => 78],
                ['match' => 'SELECT COUNT(*) FROM rw_plan_versions WHERE plan_id = :pid', 'rows' => [['c' => 27]]],
                ['match' => 'DELETE FROM rw_plan_versions WHERE plan_id = :pid', 'rows' => [], 'affected' => 2],
                ['match' => 'FROM rw_plan_versions WHERE id = :id', 'rows' => [
                    ['id' => 78, 'plan_id' => 5, 'version_name' => null,
                     'created_by' => 'tester', 'created_at' => '2026-08-07 12:01:00'],
                ]],
            ];
        case 'groups-foreign':
            return [
                ['match' => 'FROM hardware_models WHERE id = :id AND deleted_at IS NULL', 'rows' => [['id' => 3]]],
                $sourceRow,
                ['match' => 'FROM rw_model_groups WHERE hardware_model_id = :mid AND deleted_at IS NULL', 'rows' => [
                    ['id' => 11, 'source_id' => 99],
                ]],
            ];
        case 'groups-create':
            return [
                ['match' => 'FROM hardware_models WHERE id = :id AND deleted_at IS NULL', 'rows' => [['id' => 3]]],
                $sourceRow,
                ['match' => 'FROM rw_model_groups WHERE hardware_model_id = :mid AND deleted_at IS NULL', 'rows' => []],
                ['match' => 'INSERT INTO rw_model_groups', 'rows' => [], 'affected' => 1, 'insertId' => 12],
                ['match' => 'JOIN rw_model_groups g ON g.hardware_model_id = hm.id', 'rows' => [[
                    'id' => 3, 'make' => 'Acme', 'model' => 'Switch S1', 'hw_type' => 'switch', 'notes' => null,
                    'groups_json' => '[{"k":"ethernet","label":"Ethernet ×8","glyph":"rj45","count":8,"rows":1,"role":"lan","gbps":1,"domain":"net","conn":"rj45"}]',
                    'device_json' => '{"name":"Acme Switch S1","kind":"switch","drawW":18}', 'unitCount' => 1,
                ]]],
            ];
        case 'commit-empty':
            return [$sourceRow];
        case 'commit-cables':
            // Every plan device resolves to the same entity by name, and no
            // interconnect exists yet, so each committable cable takes the
            // create branch and the case is really about link_kind.
            return [
                $sourceRow,
                ['match' => 'SELECT id FROM entities WHERE LOWER(name) = LOWER(:n)', 'rows' => [['id' => 100]]],
                ['match' => 'FROM interfaces WHERE entity_id = :eid AND name = :name', 'rows' => []],
                ['match' => 'FROM interconnects WHERE deleted_at IS NULL AND link_kind = :lk', 'rows' => []],
            ];
        case 'commit-port':
            return [
                $sourceRow,
                ['match' => 'hu.asset_tag = :tag', 'rows' => [['id' => 100]]],
                ['match' => 'FROM interfaces WHERE entity_id = :eid AND name = :name', 'rows' => [['id' => 500]]],
                ['match' => 'FROM interconnects WHERE deleted_at IS NULL AND link_kind = :lk', 'rows' => []],
                ['match' => 'INSERT INTO interconnects', 'rows' => [], 'affected' => 1, 'insertId' => 88],
            ];
        case 'commit-ambiguous-name':
            return [
                $sourceRow,
                ['match' => 'SELECT id FROM entities WHERE LOWER(name) = LOWER(:n)', 'rows' => [['id' => 100], ['id' => 101]]],
            ];
        default:
            return [];
    }
}

// --------------------------------------------------------------- wire it up --

register_shutdown_function(static function (): void {
    fwrite(STDERR, "STATUS:" . http_response_code() . "\n");
    if (getenv('RW_CAPTURE') && isset($GLOBALS['rwTestPdo'])) {
        fwrite(STDERR, 'CAPTURE:' . json_encode($GLOBALS['rwTestPdo']->executed) . "\n");
    }
});

$scenario = (string) (getenv('RW_SCENARIO') ?: '');
if ($scenario === 'sor-disabled') {
    // Leave SOR_DB_USER unset: Sor::enabled() is false, Sor::db() 503s on the
    // first real query — exercises the documented failure envelope with no
    // FakePdo at all.
} elseif ($scenario === 'real-db') {
    // This mode exists only for the disposable integration script. Refuse
    // anything except its loopback scratch database before the route runs.
    if (getenv('RW_REAL_DB_TEST') !== '1' || getenv('SOR_DB_HOST') !== '127.0.0.1'
        || getenv('SOR_DB_NAME') !== getenv('RW_TEST_DB')
        || !in_array(getenv('SOR_DB_NAME'), ['sor_test', 'solarinet_stage'], true)) {
        fwrite(STDERR, "real-db worker refused: expected an allowlisted loopback scratch DB\n");
        exit(2);
    }
} else {
    putenv('SOR_DB_USER=rwtest');
    $ref  = new ReflectionClass(Sor::class);
    $prop = $ref->getProperty('pdo');
    $prop->setAccessible(true);
    $rwTestPdo = new RwFakePdo(rw_worker_scenario($scenario));
    $GLOBALS['rwTestPdo'] = $rwTestPdo;
    $prop->setValue(null, $rwTestPdo);
}

if (getenv('RW_AUTH')) {
    // Pin session storage to a per-run temp dir before Auth::bootSession()
    // starts the session. PHP's default save path is often a system location
    // (or an unwritable HOME) that a sandboxed/CI runner cannot write, which
    // would fail these cases for reasons that have nothing to do with the
    // routes. RW_SESSION_DIR is set by test_rackwire_api.php; the fallback
    // keeps this worker runnable standalone.
    $sessionDir = (string) (getenv('RW_SESSION_DIR') ?: sys_get_temp_dir() . '/rw-test-sessions-' . getmypid());
    if (!is_dir($sessionDir)) {
        @mkdir($sessionDir, 0700, true);
    }
    if (is_dir($sessionDir)) {
        session_save_path($sessionDir);
        ini_set('session.save_path', $sessionDir);
    }
    Auth::bootSession();
    $_SESSION['solari'] = ['username' => 'tester',
                          'role' => (string) (getenv('RW_ROLE') ?: 'operator'),
                          'source' => 'oidc'];
    $_SESSION['solari_created_at'] = time();
    $_SESSION['solari_seen_at'] = time();
}

$_GET = [];
$query = (string) (getenv('RW_QUERY') ?: '');
if ($query !== '') {
    parse_str($query, $_GET);
}

$router   = new Router();
$register = require $ROOT . '/dashboard/api/routes/rackwire.php';
$register($router);

$method = (string) (getenv('RW_METHOD') ?: 'GET');
$path   = (string) (getenv('RW_PATH') ?: '/');
$router->dispatch($method, $path);
