<?php
declare(strict_types=1);

/**
 * _prov_worker.php — subprocess helper for test_provisioning_api.php.
 *
 * NOT a standalone test. Response::ok()/error() exit(), so each simulated
 * request runs in its own PHP process, driven by env vars:
 *
 *   PV_METHOD / PV_PATH / PV_QUERY   request line
 *   PV_BODY        raw JSON body ('' = none)
 *   PV_AUTH=1      establish a session; PV_ROLE (default operator)
 *   PV_CSRF=0      omit the X-Solari-CSRF header (default: send the right one)
 *   PV_ORIGIN      'self' (default) | 'none' | 'evil'
 *   PV_IDEM        Idempotency-Key header
 *   PV_RESOLVERS   JSON list of FakePdo resolvers {match, rows?, affected?,
 *                  insertId?, throw?, driverCode?, sqlState?, message?}
 *   PV_CAPTURE=1   dump executed SQL + params to stderr
 *   PV_NODES       JSON rows for a fake monitoring Db::rows(); unset = the
 *                  monitoring Db class is absent (models "monitoring DB down")
 *
 * The real Policy::enforce guard is installed (default-deny + CSRF), the real
 * routes/provisioning.php and lib/ProvCatalog.php run unmodified. Sor's PDO
 * is swapped for a FakePdo (no DB is ever contacted) and SolariCtl is a fake
 * that records callQueued() to stderr as "CTL:<json>" instead of touching the
 * control socket. Unmatched SQL throws — a case can't pass on SQL it never
 * stubbed.
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
require $ROOT . '/dashboard/api/lib/Policy.php';
require $ROOT . '/dashboard/api/lib/Sor.php';

/** Body from PV_BODY, same contract as lib/bootstrap.php minus php://input. */
function solari_json_body(?int $maxBytes = null): array
{
    $raw = getenv('PV_BODY');
    if ($raw === false || $raw === '') {
        return [];
    }
    if ($maxBytes !== null && strlen($raw) > $maxBytes) {
        Response::error('payload_too_large', 'Request body exceeds the allowed size.', 413);
    }
    $decoded = json_decode($raw, true);
    if (!is_array($decoded)) {
        Response::error('bad_request', 'Request body must be a JSON object.', 400);
    }
    return $decoded;
}

/** Fake control client: records the enqueue, never opens a socket. */
if (getenv('PV_NODES') !== false) {
    final class Db
    {
        public static function rows(string $sql, array $params = []): array
        {
            fwrite(STDERR, 'MONSQL:' . preg_replace('/\s+/', ' ', $sql) . "\n");
            return json_decode((string) getenv('PV_NODES'), true) ?: [];
        }
    }
}

final class SolariCtl
{
    public static function callQueued(string $verb, array $args, string $op, ?string $idem = null): array
    {
        fwrite(STDERR, 'CTL:' . json_encode(['verb' => $verb, 'args' => $args, 'op' => $op, 'idem' => $idem]) . "\n");
        return ['request' => '42', 'status' => 'queued'];
    }

    public static function call(string $verb, array $args = []): array
    {
        fwrite(STDERR, "CTLDIRECT:$verb\n");
        throw new RuntimeException('direct SolariCtl::call is forbidden in provisioning routes');
    }

    public static function send(string $verb, array $args = []): array
    {
        fwrite(STDERR, "CTLDIRECT:$verb\n");
        throw new RuntimeException('direct SolariCtl::send is forbidden in provisioning routes');
    }
}

final class PvFakeStatement extends PDOStatement
{
    private array $rows = [];
    private int $affected = 0;
    private string $sql = '';
    private ?PvFakePdo $owner = null;

    public static function make(string $sql, PvFakePdo $owner): self
    {
        /** @var self $st */
        $st = (new ReflectionClass(self::class))->newInstanceWithoutConstructor();
        $st->sql = $sql;
        $st->owner = $owner;
        return $st;
    }

    public function execute(?array $params = null): bool
    {
        [$this->rows, $this->affected] = $this->owner->resolve($this->sql, $params ?? []);
        return true;
    }

    public function fetchAll(int $mode = PDO::FETCH_ASSOC, ...$args): array
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
        return array_values($this->rows[0])[$column] ?? false;
    }

    public function rowCount(): int
    {
        return $this->affected;
    }
}

final class PvFakePdo extends PDO
{
    private array $resolvers;
    private string $insertId = '1';
    private bool $inTx = false;
    public array $executed = [];
    public array $txLog = [];

    public function __construct(array $resolvers)
    {
        $this->resolvers = $resolvers;
    }

    public function prepare(string $query, array $options = []): PDOStatement|false
    {
        return PvFakeStatement::make($query, $this);
    }

    public function query(string $query, ?int $fetchMode = null, mixed ...$fetchModeArgs): PDOStatement|false
    {
        $st = PvFakeStatement::make($query, $this);
        $st->execute([]);
        return $st;
    }

    public function resolve(string $sql, array $params): array
    {
        $this->executed[] = ['sql' => $sql, 'params' => $params];
        $norm = preg_replace('/\s+/', ' ', trim($sql)) ?? $sql;
        if (str_contains($norm, "FROM sources WHERE slug = 'dashboard'")) {
            return [[['id' => 2]], 1];
        }
        foreach ($this->resolvers as $r) {
            if (str_contains($norm, (string) $r['match'])) {
                if (!empty($r['throw'])) {
                    $e = new PDOException((string) ($r['message'] ?? 'simulated'), (int) ($r['driverCode'] ?? 0));
                    $e->errorInfo = [(string) ($r['sqlState'] ?? 'HY000'), (int) ($r['driverCode'] ?? 0),
                        (string) ($r['message'] ?? 'simulated')];
                    throw $e;
                }
                if (isset($r['insertId'])) {
                    $this->insertId = (string) $r['insertId'];
                }
                return [$r['rows'] ?? [], (int) ($r['affected'] ?? count($r['rows'] ?? []))];
            }
        }
        fwrite(STDERR, "PvFakePdo: unmatched query:\n  $norm\n");
        throw new RuntimeException('PvFakePdo: unmatched query');
    }

    public function lastInsertId(?string $name = null): string|false
    {
        return $this->insertId;
    }

    public function beginTransaction(): bool
    {
        $this->inTx = true;
        $this->txLog[] = 'begin';
        return true;
    }

    public function commit(): bool
    {
        $this->inTx = false;
        $this->txLog[] = 'commit';
        return true;
    }

    public function rollBack(): bool
    {
        $this->inTx = false;
        $this->txLog[] = 'rollback';
        return true;
    }

    public function inTransaction(): bool
    {
        return $this->inTx;
    }
}

register_shutdown_function(static function (): void {
    fwrite(STDERR, 'STATUS:' . http_response_code() . "\n");
    if (getenv('PV_CAPTURE') && isset($GLOBALS['pvPdo'])) {
        fwrite(STDERR, 'CAPTURE:' . json_encode(['sql' => $GLOBALS['pvPdo']->executed,
            'tx' => $GLOBALS['pvPdo']->txLog]) . "\n");
    }
});

putenv('SOR_DB_USER=pvtest');
$resolvers = json_decode((string) (getenv('PV_RESOLVERS') ?: '[]'), true);
$pvPdo = new PvFakePdo(is_array($resolvers) ? $resolvers : []);
$GLOBALS['pvPdo'] = $pvPdo;
$prop = (new ReflectionClass(Sor::class))->getProperty('pdo');
$prop->setAccessible(true);
$prop->setValue(null, $pvPdo);

$method = (string) (getenv('PV_METHOD') ?: 'GET');
$_SERVER['REQUEST_METHOD'] = $method;
$_SERVER['HTTP_HOST'] = 'dash.test';
$_SERVER['CONTENT_TYPE'] = 'application/json';

$sessionDir = (string) (getenv('PV_SESSION_DIR') ?: sys_get_temp_dir() . '/pv-test-sessions-' . getmypid());
if (!is_dir($sessionDir)) {
    @mkdir($sessionDir, 0700, true);
}
session_save_path($sessionDir);
ini_set('session.save_path', $sessionDir);

if (getenv('PV_AUTH')) {
    Auth::bootSession();
    $_SESSION['solari'] = ['username' => 'tester',
                          'role' => (string) (getenv('PV_ROLE') ?: 'operator'),
                          'source' => 'directory'];
    $_SESSION['solari_created_at'] = time();
    $_SESSION['solari_seen_at'] = time();
    $token = Auth::csrfToken();
    if (getenv('PV_CSRF') !== '0') {
        $_SERVER['HTTP_X_SOLARI_CSRF'] = $token;
    }
}
$origin = (string) (getenv('PV_ORIGIN') ?: 'self');
if ($origin === 'self') {
    $_SERVER['HTTP_ORIGIN'] = 'http://dash.test';
} elseif ($origin === 'evil') {
    $_SERVER['HTTP_ORIGIN'] = 'http://evil.test';
}
$idem = getenv('PV_IDEM');
if (is_string($idem) && $idem !== '') {
    $_SERVER['HTTP_IDEMPOTENCY_KEY'] = $idem;
}

$_GET = [];
$query = (string) (getenv('PV_QUERY') ?: '');
if ($query !== '') {
    parse_str($query, $_GET);
}

$router = new Router();
$router->setGuard([Policy::class, 'enforce']);
(require $ROOT . '/dashboard/api/routes/provisioning.php')($router);
$router->dispatch($method, (string) (getenv('PV_PATH') ?: '/'));
