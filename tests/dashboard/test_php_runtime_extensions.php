<?php
declare(strict_types=1);

$required = ['ctype', 'openssl', 'pdo', 'pdo_mysql', 'session', 'json'];
$missing = array_values(array_filter($required, static fn (string $extension): bool =>
    !extension_loaded($extension)));
if (!function_exists('ctype_digit')) $missing[] = 'ctype_digit()';
if (!function_exists('openssl_verify')) $missing[] = 'openssl_verify()';
if (class_exists(PDO::class) && !in_array('mysql', PDO::getAvailableDrivers(), true)) {
    $missing[] = 'PDO MySQL driver';
}
if ($missing) {
    fwrite(STDERR, 'Missing PHP runtime requirements: ' . implode(', ', $missing) . PHP_EOL);
    exit(1);
}

fwrite(STDOUT, 'PHP runtime extensions ready: ' . implode(', ', $required) . PHP_EOL);
