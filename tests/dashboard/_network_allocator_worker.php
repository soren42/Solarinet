<?php
declare(strict_types=1);

error_reporting(E_ALL);
ini_set('display_errors', '0');
ini_set('log_errors', '0');

$root = dirname(__DIR__, 2);
require $root . '/dashboard/api/lib/Response.php';
require $root . '/dashboard/api/lib/Router.php';
require $root . '/dashboard/api/lib/Auth.php';
require $root . '/dashboard/api/lib/Operator.php';
require $root . '/dashboard/api/lib/Sor.php';

function solari_json_body(): array
{
    $raw = (string) (getenv('NA_BODY') ?: '');
    $body = json_decode($raw, true);
    return is_array($body) ? $body : [];
}

final class NaFakeStatement extends PDOStatement
{
    private string $sql = '';
    private ?NaFakePdo $db = null;
    private array $rows = [];
    private int $affected = 0;

    public static function make(string $sql, NaFakePdo $db): self
    {
        $self = (new ReflectionClass(self::class))->newInstanceWithoutConstructor();
        $self->sql = $sql;
        $self->db = $db;
        return $self;
    }

    public function execute(?array $params = null): bool
    {
        [$this->rows, $this->affected] = $this->db->resolve($this->sql, $params ?? []);
        return true;
    }

    public function fetchAll(int $mode = PDO::FETCH_DEFAULT, ...$args): array|false
    {
        if ($mode === PDO::FETCH_COLUMN) {
            return array_map(static fn(array $r) => array_values($r)[0] ?? null, $this->rows);
        }
        return $this->rows;
    }

    public function fetch(int $mode = PDO::FETCH_DEFAULT, ...$args): mixed
    {
        return $this->rows[0] ?? false;
    }

    public function fetchColumn(int $column = 0): mixed
    {
        return isset($this->rows[0]) ? (array_values($this->rows[0])[$column] ?? false) : false;
    }

    public function rowCount(): int
    {
        return $this->affected;
    }
}

final class NaFakePdo extends PDO
{
    private bool $transaction = false;

    public function __construct(private string $scenario) {}

    public function prepare(string $query, array $options = []): PDOStatement|false
    {
        return NaFakeStatement::make($query, $this);
    }

    public function query(string $query, ?int $fetchMode = null, mixed ...$fetchModeArgs): PDOStatement|false
    {
        $statement = NaFakeStatement::make($query, $this);
        $statement->execute();
        return $statement;
    }

    public function resolve(string $sql, array $params): array
    {
        $sql = preg_replace('/\s+/', ' ', trim($sql)) ?? $sql;
        if (str_contains($sql, 'FROM networks n LEFT JOIN vlans')) {
            return [[
                ['id' => 1, 'name' => 'review-a', 'purpose' => 'review fixture', 'enabled' => 1,
                 'management' => 'GATEWAY', 'is_default' => 0, 'allocator_eligible' => 0,
                 'allocator_approved_by' => null, 'allocator_approved_at' => null,
                 'vlan_tag' => 44, 'vlan_name' => 'review-vlan-a'],
                ...($this->scenario === 'get-overlap' ? [[
                    'id' => 2, 'name' => 'review-b', 'purpose' => null, 'enabled' => 1,
                    'management' => 'SWITCH', 'is_default' => 0, 'allocator_eligible' => 1,
                    'allocator_approved_by' => 'reviewer', 'allocator_approved_at' => '2026-10-22 12:00:00',
                    'vlan_tag' => null, 'vlan_name' => null,
                ]] : []),
            ], 0];
        }
        if (str_contains($sql, 'FROM subnets s LEFT JOIN dhcp_ranges')) {
            $rows = [[
                'id' => 11, 'network_id' => 1, 'cidr' => '172.31.44.0/24',
                'gateway_ip' => '::ffff:172.31.44.1', 'dhcp_pools' => '172.31.44.50–172.31.44.99',
            ]];
            if ($this->scenario === 'get-overlap') {
                $rows[] = ['id' => 12, 'network_id' => 2, 'cidr' => '172.31.44.128/25',
                           'gateway_ip' => '172.31.44.129', 'dhcp_pools' => ''];
            }
            return [$rows, 0];
        }
        if (str_contains($sql, 'SELECT id,enabled,deleted_at FROM networks')) {
            return [[['id' => 1, 'enabled' => $this->scenario === 'post-disabled' ? 0 : 1,
                      'deleted_at' => null]], 1];
        }
        if (str_contains($sql, 'SELECT cidr FROM subnets WHERE network_id=:id')) {
            return [$this->scenario === 'post-no-subnet' ? [] : [['cidr' => '172.31.44.0/24']], 1];
        }
        if (str_contains($sql, 'SELECT n.name,s.cidr FROM subnets s JOIN networks n')) {
            return [$this->scenario === 'post-overlap'
                ? [['name' => 'review-b', 'cidr' => '172.31.44.128/25']] : [], 0];
        }
        if (str_contains($sql, 'UPDATE networks SET allocator_eligible=1')) {
            if (($params[':operator'] ?? null) !== 'fixture-operator') {
                throw new RuntimeException('approval update did not bind the authenticated operator');
            }
            return [[], 1];
        }
        throw new RuntimeException('Unstubbed SQL in network allocator route test: ' . $sql);
    }

    public function beginTransaction(): bool { $this->transaction = true; return true; }
    public function commit(): bool { $this->transaction = false; return true; }
    public function rollBack(): bool { $this->transaction = false; return true; }
    public function inTransaction(): bool { return $this->transaction; }
}

register_shutdown_function(static function (): void {
    fwrite(STDERR, 'STATUS:' . http_response_code() . "\n");
});

$scenario = (string) (getenv('NA_SCENARIO') ?: '');
if ($scenario === 'sor-disabled') {
    putenv('SOR_DB_USER');
} elseif ($scenario !== 'live-read') {
    putenv('SOR_DB_USER=review-test');
    $property = (new ReflectionClass(Sor::class))->getProperty('pdo');
    $property->setAccessible(true);
    $property->setValue(null, new NaFakePdo($scenario));
}

if (getenv('NA_AUTH') === '1') {
    $sessionDir = (string) (getenv('NA_SESSION_DIR') ?: sys_get_temp_dir());
    session_save_path($sessionDir);
    session_id('network-allocator-test-' . getmypid());
    session_start();
    $_SESSION['solari'] = ['username' => 'fixture-operator', 'role' => (string) (getenv('NA_ROLE') ?: 'operator'), 'source' => 'directory'];
    $_SESSION['solari_created_at'] = time();
    $_SESSION['solari_seen_at'] = time();
}

$router = new Router();
$register = require $root . '/dashboard/api/routes/network_allocator.php';
$register($router);
$router->dispatch((string) (getenv('NA_METHOD') ?: 'GET'), (string) (getenv('NA_PATH') ?: '/api/networks/allocator-eligibility'));
