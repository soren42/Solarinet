<?php
declare(strict_types=1);

/**
 * Db — PDO/MariaDB connection helper for the read layer.
 *
 * Credentials come EXCLUSIVELY from environment variables; nothing is hardcoded
 * and no secret is ever written to disk by this layer (§11.1: PHP never holds
 * cert/CA material, and the read tier is SELECT-only on the monitoring tables).
 *
 * Recognised environment variables:
 *   SOLARI_DB_DSN   — full PDO DSN; if set it wins outright.
 *   SOLARI_DB_HOST  — default "127.0.0.1"
 *   SOLARI_DB_PORT  — default "3306"
 *   SOLARI_DB_NAME  — default "solarinet"
 *   SOLARI_DB_SOCK  — unix socket path (alternative to host/port)
 *   SOLARI_DB_USER  — required
 *   SOLARI_DB_PASS  — required
 *   SOLARI_DB_CHARSET — default "utf8mb4"
 */
final class Db
{
    private static ?PDO $pdo = null;

    /** Lazily open (once per request) and return the shared PDO handle. */
    public static function pdo(): PDO
    {
        if (self::$pdo instanceof PDO) {
            return self::$pdo;
        }

        $user = getenv('SOLARI_DB_USER');
        $pass = getenv('SOLARI_DB_PASS');
        if ($user === false || $user === '') {
            throw new RuntimeException('SOLARI_DB_USER is not configured');
        }
        // An empty password is permissible only if explicitly set to "".
        $pass = ($pass === false) ? '' : $pass;

        $dsn = self::buildDsn();

        $options = [
            PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            PDO::ATTR_EMULATE_PREPARES   => false,
            PDO::ATTR_STRINGIFY_FETCHES  => false,
        ];

        self::$pdo = new PDO($dsn, $user, $pass, $options);
        return self::$pdo;
    }

    /** Construct the DSN from env (explicit DSN wins; else socket; else host/port). */
    private static function buildDsn(): string
    {
        $explicit = getenv('SOLARI_DB_DSN');
        if ($explicit !== false && $explicit !== '') {
            return $explicit;
        }

        $name    = self::envOr('SOLARI_DB_NAME', 'solarinet');
        $charset = self::envOr('SOLARI_DB_CHARSET', 'utf8mb4');
        $sock    = getenv('SOLARI_DB_SOCK');

        if ($sock !== false && $sock !== '') {
            return sprintf('mysql:unix_socket=%s;dbname=%s;charset=%s', $sock, $name, $charset);
        }

        $host = self::envOr('SOLARI_DB_HOST', '127.0.0.1');
        $port = self::envOr('SOLARI_DB_PORT', '3306');
        return sprintf('mysql:host=%s;port=%s;dbname=%s;charset=%s', $host, $port, $name, $charset);
    }

    private static function envOr(string $key, string $default): string
    {
        $v = getenv($key);
        return ($v === false || $v === '') ? $default : $v;
    }

    /**
     * Run a parameterised SELECT and return all rows.
     *
     * @param string              $sql    SQL with named/positional placeholders.
     * @param array<string,mixed> $params Bound parameters.
     * @return array<int,array<string,mixed>>
     */
    public static function rows(string $sql, array $params = []): array
    {
        $stmt = self::pdo()->prepare($sql);
        $stmt->execute($params);
        return $stmt->fetchAll();
    }

    /**
     * Run a parameterised SELECT and return the first row, or null.
     *
     * @param array<string,mixed> $params
     * @return array<string,mixed>|null
     */
    public static function row(string $sql, array $params = []): ?array
    {
        $stmt = self::pdo()->prepare($sql);
        $stmt->execute($params);
        $row = $stmt->fetch();
        return $row === false ? null : $row;
    }

    /**
     * Run a parameterised SELECT and return a single scalar from the first row.
     *
     * @param array<string,mixed> $params
     * @return mixed|null
     */
    public static function scalar(string $sql, array $params = [])
    {
        $stmt = self::pdo()->prepare($sql);
        $stmt->execute($params);
        $v = $stmt->fetchColumn();
        return $v === false ? null : $v;
    }

    /**
     * Run a parameterised write (UPDATE/INSERT/DELETE) and return the affected
     * row count.
     *
     * NOTE on §11.1: the read tier is SELECT-only on the MONITORING telemetry
     * tables and never touches cert/CA material. This helper exists solely for
     * the dashboard's own CONTROL-PLANE config tables (alertRule), which the
     * Phase-3 handoff §6 maps directly to the web tier ("POST /api/rules/{ruleId}
     * → alertRule"). Lifecycle/provisioning mutations still go through solariCtl;
     * this is never used to write node/host/probe telemetry.
     *
     * @param array<string,mixed> $params
     */
    public static function exec(string $sql, array $params = []): int
    {
        $stmt = self::pdo()->prepare($sql);
        $stmt->execute($params);
        return $stmt->rowCount();
    }
}
