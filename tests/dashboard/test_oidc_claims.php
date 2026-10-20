<?php
declare(strict_types=1);

require_once __DIR__ . '/../../dashboard/api/lib/Auth.php';
require_once __DIR__ . '/../../dashboard/api/lib/Oidc.php';

function check(bool $condition, string $message): void
{
    if (!$condition) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

$now = 1_800_000_000;
$config = ['issuer' => 'https://id.example/realm', 'clientId' => 'solarinet'];
$claims = ['iss' => $config['issuer'], 'aud' => 'solarinet', 'sub' => 'u-1',
           'iat' => $now - 30, 'exp' => $now + 300, 'nonce' => 'opaque'];
check(Oidc::claimError($claims, $config, 'opaque', $now) === null,
      'valid claims should pass');
foreach (['exp', 'iat', 'sub', 'nonce'] as $field) {
    $invalid = $claims;
    unset($invalid[$field]);
    check(Oidc::claimError($invalid, $config, 'opaque', $now) !== null,
          "missing $field should fail");
}
$invalid = $claims;
$invalid['exp'] = (string) ($now + 300);
check(Oidc::claimError($invalid, $config, 'opaque', $now) !== null,
      'string exp should fail');
$invalid = $claims;
$invalid['iat'] = $now + 120;
check(Oidc::claimError($invalid, $config, 'opaque', $now) !== null,
      'future issued-at should fail');
$invalid = $claims;
$invalid['aud'] = ['solarinet', 'other'];
check(Oidc::claimError($invalid, $config, 'opaque', $now) !== null,
      'multiple audiences without azp should fail');
$invalid['azp'] = 'solarinet';
check(Oidc::claimError($invalid, $config, 'opaque', $now) === null,
      'matching authorized party should pass');
$tx = ['state' => 'secret', 'nonce' => 'opaque', 'ts' => $now - 20];
check(Oidc::validTransaction($tx, 'secret', $now), 'fresh state should pass');
check(!Oidc::validTransaction($tx, 'wrong', $now), 'wrong state should fail');
check(!Oidc::validTransaction($tx, 'secret', $now + 601), 'old state should fail');
check(!Oidc::validTransaction($tx, 'secret', $now - 21), 'future state should fail');
echo "OIDC claim and state checks passed\n";
