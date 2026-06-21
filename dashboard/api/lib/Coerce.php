<?php
declare(strict_types=1);

/**
 * Coerce — shaping helpers that keep wire output in RAW units and correct types.
 *
 * No display conversion happens here (KB stays KB, micros stay micros, permille
 * stays permille). These helpers only fix representation: DATETIME -> ISO-8601
 * UTC string, JSON columns -> decoded structures, numeric strings -> int/float,
 * and MariaDB BIGINT UNSIGNED node ids -> strings (to survive JS number range).
 */
final class Coerce
{
    /**
     * Format a MariaDB DATETIME ("Y-m-d H:i:s[.u]") as ISO-8601 UTC with a Z.
     * Stored times are already UTC (schema header), so we re-stamp, not convert.
     */
    public static function iso(?string $datetime): ?string
    {
        if ($datetime === null || $datetime === '' || $datetime === '0000-00-00 00:00:00') {
            return null;
        }
        $dt = DateTimeImmutable::createFromFormat(
            'Y-m-d H:i:s',
            substr($datetime, 0, 19),
            new DateTimeZone('UTC')
        );
        if ($dt === false) {
            return null;
        }
        return $dt->format('Y-m-d\TH:i:s\Z');
    }

    /** Decode a JSON-typed column to a structure; null/invalid -> null. */
    public static function json(?string $raw)
    {
        if ($raw === null || $raw === '') {
            return null;
        }
        $decoded = json_decode($raw, true);
        return $decoded === null && json_last_error() !== JSON_ERROR_NONE ? null : $decoded;
    }

    /** Nullable int coercion. */
    public static function int($v): ?int
    {
        return $v === null ? null : (int) $v;
    }

    /** Nullable float coercion. */
    public static function float($v): ?float
    {
        return $v === null ? null : (float) $v;
    }

    /** Nullable bool coercion (MariaDB BOOLEAN/TINYINT 0/1). */
    public static function bool($v): ?bool
    {
        return $v === null ? null : ((int) $v === 1);
    }

    /**
     * BIGINT UNSIGNED id -> string. node ids are FNV-1a-64 values that overflow
     * JS Number.MAX_SAFE_INTEGER, so they travel as strings on the wire.
     */
    public static function id($v): ?string
    {
        return $v === null ? null : (string) $v;
    }
}
