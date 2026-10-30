<?php

declare(strict_types=1);

/**
 * Sor — mirror dashboard mutations into the System of Record (MariaDB `sor` on
 * cesium). Writing the SoR fires its CDC triggers, so an operator create / alter /
 * remove flows onward through the sorsync subsystem (MQ → DNS, and future render
 * targets) automatically.
 *
 * SoR host mutations commit here before SolariCtl updates the derived
 * solarinet asset mirror. Callers must stop before changing that mirror when
 * this method returns false; replaying the requested state is safe by IP.
 *
 * Connection: SOR_DB_HOST/PORT/NAME/USER/PASS (php-fpm pool env, alongside
 * SOLARI_DB_*). Provenance: the `dashboard` source, human-asserted. Correlation
 * is by IP (v4-mapped, matching the SoR's storage), never by name.
 */
final class Sor
{
    private static ?PDO $pdo = null;
    private static ?int $srcId = null;

    public static function enabled(): bool
    {
        $u = getenv('SOR_DB_USER');
        return $u !== false && $u !== '';
    }

    /**
     * Public PDO handle onto the SoR `sor` DB, for callers (the inventory
     * routes) for whom the SoR is the AUTHORITATIVE store, not a best-effort
     * mirror — unlike upsertHost/removeHost below, inventory has no local
     * fallback table, so a missing/unreachable SoR is a real 503, not a
     * swallowed no-op. Emits the error envelope itself and exits (matching the
     * Response::error contract used throughout the route layer).
     */
    public static function db(): PDO
    {
        if (!self::enabled()) {
            Response::error('service_unavailable', 'Inventory storage (SoR) is not configured.', 503);
        }
        try {
            $pdo = self::pdo();
        } catch (\Throwable $e) {
            error_log('Sor::db connect failed: ' . $e->getMessage());
            Response::error('service_unavailable', 'Inventory storage (SoR) is unreachable.', 503);
        }
        if ($pdo === null) {
            Response::error('service_unavailable', 'Inventory storage (SoR) is not configured.', 503);
        }
        return $pdo;
    }

    /**
     * The fixed 'dashboard' source id (provenance for rows the operator writes
     * through this UI) — id 2 per the seeded `sources` table, resolved by slug
     * with a hardcoded fallback so a reseeded/renumbered `sources` table can't
     * silently break every insert.
     */
    public static function dashboardSourceId(): int
    {
        return self::sourceId(self::db());
    }

    private static function pdo(): ?PDO
    {
        if (self::$pdo instanceof PDO) {
            return self::$pdo;
        }
        if (!self::enabled()) {
            return null;
        }
        $host = getenv('SOR_DB_HOST') ?: '10.1.0.200';
        $port = getenv('SOR_DB_PORT') ?: '3306';
        $name = getenv('SOR_DB_NAME') ?: 'sor';
        $dsn  = "mysql:host={$host};port={$port};dbname={$name};charset=utf8mb4";
        self::$pdo = new PDO($dsn, (string) getenv('SOR_DB_USER'), (string) (getenv('SOR_DB_PASS') ?: ''), [
            PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            PDO::ATTR_EMULATE_PREPARES   => false,
            PDO::ATTR_TIMEOUT            => 4,
        ]);
        return self::$pdo;
    }

    /** INET6_ATON argument: IPv4 as v4-mapped ::ffff: (the SoR's convention). */
    private static function aton(string $ip): string
    {
        return strpos($ip, ':') !== false ? $ip : "::ffff:{$ip}";
    }

    /** First DNS label of a display name, lowercased; null if not a valid label. */
    private static function label(?string $name): ?string
    {
        if ($name === null) {
            return null;
        }
        $short = strtolower(explode('.', trim($name))[0]);
        return preg_match('/^[a-z0-9][a-z0-9-]{0,62}$/', $short) === 1 ? $short : null;
    }

    private static function sourceId(PDO $db): int
    {
        if (self::$srcId !== null) {
            return self::$srcId;
        }
        $st = $db->query("SELECT id FROM sources WHERE slug = 'dashboard'");
        self::$srcId = (int) ($st->fetchColumn() ?: 2);
        return self::$srcId;
    }

    private static function entityIdForIp(PDO $db, string $ip): ?int
    {
        $st = $db->prepare('SELECT entity_id FROM ip_addresses WHERE address = INET6_ATON(:a) AND deleted_at IS NULL');
        $st->execute([':a' => self::aton($ip)]);
        $eid = $st->fetchColumn();
        return $eid === false ? null : (int) $eid;
    }

    /** Serialize desired-state mirror operations for the same address. */
    private static function acquireIpLock(PDO $db, string $ip): string
    {
        // MariaDB lock names are limited to 64 bytes. Hash the canonical address
        // so IPv4 and its mapped IPv6 form share one lock without exposing it.
        $packed = @inet_pton($ip);
        if ($packed === false) {
            throw new InvalidArgumentException('The SoR host address is not a valid IP address.');
        }
        if (strlen($packed) === 4) {
            $packed = str_repeat("\0", 10) . "\xff\xff" . $packed;
        }
        $lock = 'sorhost:' . substr(hash('sha256', $packed), 0, 56);
        $st = $db->prepare('SELECT GET_LOCK(:name, 5)');
        $st->execute([':name' => $lock]);
        if ((int) $st->fetchColumn() !== 1) {
            throw new RuntimeException('Could not acquire the SoR host mutation lock.');
        }
        return $lock;
    }

    private static function releaseIpLock(PDO $db, string $lock): void
    {
        $st = $db->prepare('SELECT RELEASE_LOCK(:name)');
        $st->execute([':name' => $lock]);
    }

    /**
     * Create-or-rename a host entity keyed by IP. Existing entity → refresh name
     * (only if the display name yields a valid DNS label). New IP → insert a
     * host entity + primary IP so it becomes a first-class SoR inventory row
     * (and, being named + primary, a DNS A/PTR record).
     */
    public static function upsertHost(string $ip, ?string $name): bool
    {
        if (!self::enabled() || $ip === '') {
            return false;
        }
        $db = null;
        $lock = null;
        try {
            $db = self::pdo();
            if ($db === null) {
                return false;
            }
            $lock = self::acquireIpLock($db, $ip);
            $db->beginTransaction();
            $label = self::label($name);
            $src   = self::sourceId($db);
            $eid   = self::entityIdForIp($db, $ip);
            if ($eid !== null) {
                if ($label !== null) {
                    $db->prepare("UPDATE entities SET name = :n, source_id = :s, asserted_kind = 'human', asserted_at = NOW() WHERE id = :e AND deleted_at IS NULL AND (name <> :check_name OR NOT (source_id <=> :check_source) OR asserted_kind <> 'human')")
                       ->execute([':n' => $label, ':s' => $src, ':e' => $eid, ':check_name' => $label, ':check_source' => $src]);
                }
            } elseif ($label !== null) {
                $db->prepare("INSERT INTO entities (name, entity_type, source_id, asserted_kind) VALUES (:n, 'other', :s, 'human')")
                   ->execute([':n' => $label, ':s' => $src]);
                $eid = (int) $db->lastInsertId();
                $db->prepare("INSERT INTO ip_addresses (entity_id, address, is_primary, source_id, asserted_kind) VALUES (:e, INET6_ATON(:a), 1, :s, 'human')")
                   ->execute([':e' => $eid, ':a' => self::aton($ip), ':s' => $src]);
            }
            $db->commit();
            return true;
        } catch (\Throwable $e) {
            if ($db instanceof PDO && $db->inTransaction()) {
                try {
                    $db->rollBack();
                } catch (\Throwable $rollbackError) {
                    error_log('Sor::upsertHost rollback failed: ' . $rollbackError->getMessage());
                }
            }
            error_log('Sor::upsertHost failed (' . $ip . '): ' . $e->getMessage());
            return false;
        } finally {
            if ($db instanceof PDO && $lock !== null) {
                try {
                    self::releaseIpLock($db, $lock);
                } catch (\Throwable $e) {
                    error_log('Sor::upsertHost lock release failed: ' . $e->getMessage());
                }
            }
        }
    }

    /**
     * Soft-delete the host entity + its IPs, keyed by IP. Recoverable (sets
     * deleted_at); v_dns_hosts filters deleted_at IS NULL, so the host drops out
     * of DNS on the next sync. No-op if the IP is not in the SoR.
     */
    public static function removeHost(string $ip): bool
    {
        if (!self::enabled() || $ip === '') {
            return false;
        }
        $db = null;
        $lock = null;
        try {
            $db = self::pdo();
            if ($db === null) {
                return false;
            }
            $lock = self::acquireIpLock($db, $ip);
            $db->beginTransaction();
            $eid = self::entityIdForIp($db, $ip);
            if ($eid === null) {
                $db->commit();
                return true;
            }
            $db->prepare('UPDATE ip_addresses SET deleted_at = NOW() WHERE entity_id = :e AND deleted_at IS NULL')->execute([':e' => $eid]);
            $db->prepare('UPDATE entities SET deleted_at = NOW() WHERE id = :e AND deleted_at IS NULL')->execute([':e' => $eid]);
            $db->commit();
            return true;
        } catch (\Throwable $e) {
            if ($db instanceof PDO && $db->inTransaction()) {
                try {
                    $db->rollBack();
                } catch (\Throwable $rollbackError) {
                    error_log('Sor::removeHost rollback failed: ' . $rollbackError->getMessage());
                }
            }
            error_log('Sor::removeHost failed (' . $ip . '): ' . $e->getMessage());
            return false;
        } finally {
            if ($db instanceof PDO && $lock !== null) {
                try {
                    self::releaseIpLock($db, $lock);
                } catch (\Throwable $e) {
                    error_log('Sor::removeHost lock release failed: ' . $e->getMessage());
                }
            }
        }
    }
}
