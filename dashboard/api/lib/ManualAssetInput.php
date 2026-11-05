<?php
declare(strict_types=1);

/** Strict single-host input for manual asset scans. */
final class ManualAssetInput
{
    public static function target(string $value): ?array
    {
        $value = trim($value);
        if ($value === '' || strlen($value) > 253 || preg_match('/[\s\x00-\x20\x7f]/', $value)) return null;
        if (filter_var($value, FILTER_VALIDATE_IP, FILTER_FLAG_IPV4) !== false) {
            return self::allowedIp($value) ? ['ip' => $value, 'host' => ''] : null;
        }
        if (preg_match('/^(?=.{1,253}\.?$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)*[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.?$/D', $value) !== 1) return null;
        $ips = gethostbynamel(rtrim($value, '.'));
        if (!is_array($ips)) return null;
        $ips = array_values(array_unique($ips));
        if (count($ips) !== 1 || !self::allowedIp($ips[0])) return null;
        return ['ip' => $ips[0], 'host' => rtrim($value, '.')];
    }

    public static function allowedIp(string $ip): bool
    {
        if (filter_var($ip, FILTER_VALIDATE_IP, FILTER_FLAG_IPV4) === false) return false;
        $n = unpack('Nvalue', inet_pton($ip));
        if (!is_array($n)) return false;
        $v = $n['value'];
        foreach ([[0x0A000000, 8], [0xAC100000, 12], [0xC0A80000, 16], [0x7F000000, 8], [0xA9FE0000, 16]] as [$network, $prefix]) {
            $mask = (0xFFFFFFFF << (32 - $prefix)) & 0xFFFFFFFF;
            if (($v & $mask) === $network) return true;
        }
        return false;
    }

    public static function ports(mixed $value): ?array
    {
        if (!is_array($value) || count($value) > 64) return null;
        $out = [];
        foreach ($value as $port) {
            if (!is_int($port) || $port < 1 || $port > 65535 || isset($out[$port])) return null;
            $out[$port] = true;
        }
        return array_keys($out);
    }
}
