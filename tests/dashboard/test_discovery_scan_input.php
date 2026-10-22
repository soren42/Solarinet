<?php
declare(strict_types=1);

require_once __DIR__ . '/../../dashboard/api/lib/DiscoveryScanInput.php';

final class TestResponseError extends RuntimeException
{
    public function __construct(public int $status)
    {
        parent::__construct('response error');
    }
}

final class Router
{
    public array $posts = [];
    public array $gets = [];
    public function post(string $path, callable $handler): void { $this->posts[$path] = $handler; }
    public function get(string $path, callable $handler, array $policy = []): void { $this->gets[$path] = $handler; }
}

final class Response
{
    public static array $result = [];
    public static int $status = 200;
    public static function error(string $code, string $message, int $status): never
    {
        throw new TestResponseError($status);
    }
    public static function ok(array $data, int $status = 200): void { self::$result = $data; self::$status = $status; }
}

final class Operator
{
    public static function name(): string { return 'test-operator'; }
    public static function requireOperator(): string { return 'test-operator'; }
}

final class Db
{
    public static array $rows = [];
    public static string $sql = '';
    public static array $params = [];
    public static function rows(string $sql, array $params = []): array
    { self::$sql = $sql; self::$params = $params; return self::$rows; }
}

final class SolariCtl
{
    public static array $call = [];
    public static array $queued = [];
    public static function call(string $verb, array $args = []): array
    {
        self::$call = [$verb, $args];
        return ['scan' => '42', 'status' => 'completed', 'done' => '254',
                'total' => '254', 'found' => '2', 'errorCode' => '0', 'error' => ''];
    }
    public static function callQueued(string $verb, array $args, string $op, ?string $idem = null,
                                      float $waitSec = 4.0): array
    { self::$queued = [$verb, $args, $op, $idem, $waitSec]; return ['scan' => '42', 'hosts' => '254']; }
    public static function poll(int $requestId, string $op): array
    { self::$call = ['REQUEST_GET', ['request' => $requestId, 'op' => $op]]; return ['state' => 'done', 'result' => 'OK scan=42 status=queued hosts=254']; }
}

function solari_json_body(): array { return $GLOBALS['test_body']; }

function assert_true(bool $condition, string $message): void
{
    if (!$condition) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

/* Pure helper tests above are paired with this handler-level contract check. */
function assert_route_status(array $body, int $expectedStatus): void
{
    $GLOBALS['test_body'] = $body;
    try {
        ($GLOBALS['scan_handler'])();
        assert_true(Response::$status === $expectedStatus, 'scan handler should return expected success status');
    } catch (TestResponseError $error) {
        assert_true($error->status === $expectedStatus, 'scan handler should reject with expected status');
    }
}

function check(bool $condition, string $message): void
{
    if (!$condition) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

foreach (['10.0.0.0/24', '192.0.2.4/31', '192.0.2.9/32'] as $cidr) {
    check(DiscoveryScanInput::validCidr($cidr), "expected valid CIDR: $cidr");
}
foreach (['999.1.1.1/24', '10.0.0.0/19', '10.0.0.0/0', '10.0.0.0/33', '10.0.0.0/24x', '10.0.0.0/024'] as $cidr) {
    check(!DiscoveryScanInput::validCidr($cidr), "expected invalid CIDR: $cidr");
}

check(DiscoveryScanInput::ports('') === [], 'empty port string should select server defaults');
check(DiscoveryScanInput::ports('22,80,443') === [22, 80, 443], 'valid port list should preserve order');
foreach (['0', '65536', '22,,80', '22x,80', '22,22', '22,', ' 22', '022'] as $ports) {
    check(DiscoveryScanInput::ports($ports) === null, "expected invalid port list: $ports");
}
check(DiscoveryScanInput::ports(implode(',', range(1, 64))) !== null, '64 ports should be accepted');
check(DiscoveryScanInput::ports(implode(',', range(1, 65))) === null, 'more than 64 ports should be rejected');

$_SERVER['REQUEST_METHOD'] = 'POST';
$register = require __DIR__ . '/../../dashboard/api/routes/discovery_mut.php';
$router = new Router();
$register($router);
$GLOBALS['scan_handler'] = $router->posts['/api/discovery/scan'];

$_SERVER['HTTP_IDEMPOTENCY_KEY'] = 'scan-regression-key-001';
assert_route_status(['cidr' => '10.0.0.0/24', 'ports' => '22,443'], 202);
assert_true(SolariCtl::$queued[0] === 'DISCOVER', 'valid scan should enter the privileged C control queue');
assert_true(SolariCtl::$queued[1]['ports'] === '22,443', 'validated ports should preserve request order');
assert_true(SolariCtl::$queued[3] === 'scan-regression-key-001',
    'scan idempotency key should reach the durable control queue');
assert_true(Response::$result === ['cidr' => '10.0.0.0/24', 'scanId' => 42,
    'hostsTotal' => 254, 'status' => 'queued'], 'scan API should return a durable job id and accepted status');
assert_true(Response::$status === 202, 'scan admission must return HTTP 202');

$statusHandler = $router->gets['/api/discovery/scans/{scanId}'];
$statusHandler(['scanId' => '42']);
assert_true(SolariCtl::$call[0] === 'DISCOVER_STATUS' && SolariCtl::$call[1]['scan'] === '42',
    'scan status should use the C requester-scoped status verb');
assert_true(Response::$result['status'] === 'completed' && Response::$result['hostsFound'] === 2,
    'scan status response should expose terminal state and counts');

$requestHandler = $router->gets['/api/discovery/scan-requests/{requestId}'];
$requestHandler(['requestId' => '9']);
assert_true(SolariCtl::$call[0] === 'REQUEST_GET' && SolariCtl::$call[1]['op'] === 'test-operator',
    'queue admission status must be scoped to the authenticated operator');
assert_true(Response::$result['scanId'] === 42 && Response::$result['state'] === 'done',
    'completed queue admission should recover its durable scan id');

Db::$rows = [[
    'scanId' => '73', 'cidr' => '10.0.0.0/24', 'ports' => '', 'status' => 'running',
    'hostsTotal' => '254', 'hostsDone' => '96', 'hostsFound' => '4',
    'errorText' => null, 'createdAt' => '2026-10-22 12:34:56.000',
]];
($router->gets['/api/discovery/scan-jobs'])();
assert_true(str_contains(Db::$sql, 'WHERE requestedBy = :requestedBy'),
    'recent scan jobs must be scoped in SQL to the authenticated requester');
assert_true(Db::$params === [':requestedBy' => 'test-operator'],
    'recent scan jobs must bind the authenticated requester');
assert_true(Response::$result === [[
    'scanId' => 73, 'cidr' => '10.0.0.0/24', 'ports' => '', 'status' => 'running',
    'hostsTotal' => 254, 'hostsDone' => 96, 'hostsFound' => 4, 'error' => '',
    'createdAt' => '2026-10-22 12:34:56.000',
]], 'recent scan jobs should expose bounded progress without leaking another operator identity');

foreach ([
    ['cidr' => '999.1.1.1/24'],
    ['cidr' => '10.0.0.0/19'],
    ['cidr' => '10.0.0.0/024'],
    ['cidr' => '10.0.0.0/24', 'ports' => '22,,80'],
    ['cidr' => '10.0.0.0/24', 'ports' => 22],
] as $body) {
    $before = SolariCtl::$call;
    assert_route_status($body, 400);
    assert_true(SolariCtl::$call === $before, 'invalid input must not reach solariCtl');
}

$_SERVER['HTTP_IDEMPOTENCY_KEY'] = 'invalid/key';
$beforeQueued = SolariCtl::$queued;
assert_route_status(['cidr' => '10.0.0.0/24'], 400);
assert_true(SolariCtl::$queued === $beforeQueued, 'invalid idempotency key must not reach solariCtl');

echo "Discovery scan input and route bounds passed\n";
