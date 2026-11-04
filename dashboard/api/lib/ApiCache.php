<?php
declare(strict_types=1);

/**
 * Small private cache for read-only upstream summaries. Prefer APCu; when it is
 * unavailable, use short-lived files in the host's temporary runtime directory.
 */
final class ApiCache
{
    /** @return mixed */
    public static function remember(string $key, int $ttlSeconds, callable $load)
    {
        $cacheKey = 'solari-api:' . hash('sha256', $key);
        if (function_exists('apcu_fetch')
            && (!function_exists('apcu_enabled') || apcu_enabled())) {
            $found = false;
            $value = @apcu_fetch($cacheKey, $found);
            if ($found) {
                return $value;
            }
            $value = $load();
            @apcu_store($cacheKey, $value, max(1, $ttlSeconds));
            return $value;
        }

        $dir = rtrim(sys_get_temp_dir(), DIRECTORY_SEPARATOR) . DIRECTORY_SEPARATOR . 'solari-api-cache';
        if (!is_dir($dir) && !@mkdir($dir, 0700, true) && !is_dir($dir)) {
            return $load();
        }
        @chmod($dir, 0700);
        $path = $dir . DIRECTORY_SEPARATOR . hash('sha256', $key) . '.cache';
        $cached = self::readFile($path);
        if ($cached !== null && ($cached['expires'] ?? 0) >= time()) {
            return $cached['value'];
        }

        $value = $load();
        $temporary = @tempnam($dir, 'summary-');
        if (is_string($temporary)) {
            @chmod($temporary, 0600);
            $bytes = @file_put_contents($temporary, serialize([
                'expires' => time() + max(1, $ttlSeconds),
                'value' => $value,
            ]), LOCK_EX);
            if ($bytes !== false && @rename($temporary, $path)) {
                @chmod($path, 0600);
            } else {
                @unlink($temporary);
            }
        }
        return $value;
    }

    /** @return array{expires:int,value:mixed}|null */
    private static function readFile(string $path): ?array
    {
        if (!is_file($path) || !is_readable($path)) {
            return null;
        }
        $raw = @file_get_contents($path);
        if (!is_string($raw)) {
            return null;
        }
        $value = @unserialize($raw, ['allowed_classes' => false]);
        return is_array($value) && isset($value['expires']) && array_key_exists('value', $value)
            ? $value : null;
    }
}
