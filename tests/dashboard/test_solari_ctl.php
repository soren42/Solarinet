<?php
declare(strict_types=1);

require_once __DIR__ . '/../../dashboard/api/lib/SolariCtl.php';

function assert_true(bool $cond, string $message): void
{
    if (!$cond) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

$rc = new ReflectionClass(SolariCtl::class);
assert_true($rc->getConstant('REPLY_CAP') === 32768,
    'SolariCtl::REPLY_CAP must match src/server/solariCtl.c CTL_REPLY_CAP');
assert_true($rc->getConstant('REQUEST_CAP') === 16384,
    'SolariCtl::REQUEST_CAP must match src/server/solariCtl.c CTL_REQ_CAP');

$parse = $rc->getMethod('parseReply');
$parse->setAccessible(true);

$ok = $parse->invoke(null, "OK cert=" . rawurlencode("-----BEGIN CERTIFICATE-----\nabc\n-----END CERTIFICATE-----\n") . "\n");
assert_true($ok['ok'] === true, 'OK reply should parse as success');
assert_true(str_contains($ok['fields']['cert'] ?? '', 'BEGIN CERTIFICATE'),
    'URL-encoded cert field should be decoded');

$err = $parse->invoke(null, "ERR -61 operator required\n");
assert_true($err['ok'] === false, 'ERR reply should parse as failure');
assert_true($err['code'] === -61, 'ERR code should be preserved');
assert_true($err['message'] === 'operator required', 'ERR message should be preserved');

$statusForCode = $rc->getMethod('statusForCode');
$statusForCode->setAccessible(true);
assert_true($statusForCode->invoke(null, -1, 'active server required for discovery adoption') === 503,
    'standby/lease role failures should be exposed as retryable service-unavailable responses');

echo "SolariCtl parser/cap tests passed\n";
