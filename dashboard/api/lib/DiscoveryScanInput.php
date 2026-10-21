<?php
declare(strict_types=1);

/** Input bounds shared by the HTTP discovery route and its regression tests. */
final class DiscoveryScanInput
{
    public static function validCidr(string $cidr): bool
    {
        $parts = explode('/', $cidr);
        if (count($parts) !== 2 || filter_var($parts[0], FILTER_VALIDATE_IP, FILTER_FLAG_IPV4) === false
            || $parts[1] === '' || !ctype_digit($parts[1])) {
            return false;
        }
        $prefix = (int) $parts[1];
        if ((string) $prefix !== $parts[1]) {
            // Leading zeros are ambiguous across parsers; reject them.
            return false;
        }
        if ($prefix < 1 || $prefix > 32) {
            return false;
        }
        $hosts = $prefix === 32 ? 1 : ($prefix === 31 ? 2 : (2 ** (32 - $prefix)) - 2);
        return $hosts <= 4096;
    }

    /** Empty input means use the server's default port set; null means invalid. */
    public static function ports(string $ports): ?array
    {
        if ($ports === '') {
            return [];
        }
        $tokens = explode(',', $ports);
        if (count($tokens) > 64) {
            return null;
        }
        $parsed = [];
        foreach ($tokens as $token) {
            if ($token === '' || !ctype_digit($token)) {
                return null;
            }
            $port = (int) $token;
            if ($port < 1 || $port > 65535 || (string) $port !== $token) {
                return null;
            }
            if (isset($parsed[$port])) {
                return null;
            }
            $parsed[$port] = true;
        }
        return array_keys($parsed);
    }
}
