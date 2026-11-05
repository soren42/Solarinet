<?php
declare(strict_types=1);

require_once __DIR__ . '/../../dashboard/api/lib/ManualAssetInput.php';

function manual_assert(bool $ok, string $message): void
{
    if (!$ok) { fwrite(STDERR, "FAIL: $message\n"); exit(1); }
}

foreach (['10.1.2.3', '172.16.1.2', '192.168.2.3', '127.0.0.1', '169.254.1.3'] as $ip) {
    manual_assert(ManualAssetInput::allowedIp($ip), "private target rejected: $ip");
}
foreach (['8.8.8.8', '192.0.2.1', '10.0.0.0/24', '-oX/tmp/x', '10.0.0.1;id', '2001:db8::1'] as $ip) {
    manual_assert(!ManualAssetInput::allowedIp($ip), "invalid or external target accepted: $ip");
}
manual_assert(ManualAssetInput::target('10.0.0.2') === ['ip' => '10.0.0.2', 'host' => ''], 'single IPv4 target did not normalize');
foreach (['', '10.0.0.2/32', '-oX', 'host name', 'user@host', 'host;id', 'https://host'] as $target) {
    manual_assert(ManualAssetInput::target($target) === null, "invalid target accepted: $target");
}
manual_assert(ManualAssetInput::ports([22, 443]) === [22, 443], 'valid port list rejected');
foreach ([[0], [65536], ['22'], [22, 22], array_fill(0, 65, 80)] as $ports) {
    manual_assert(ManualAssetInput::ports($ports) === null, 'invalid port list accepted');
}
echo "manual asset scan target validation passed\n";
