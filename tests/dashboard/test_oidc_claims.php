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
check(Oidc::endpointAllowedForConfig('https://id.example/token', $config),
      'issuer origin should be allowed by default');
check(!Oidc::endpointAllowedForConfig('https://evil.example/token', $config),
      'cross-origin discovery endpoint should be rejected by default');
check(!Oidc::endpointAllowedForConfig('http://id.example/token', $config),
      'plain HTTP endpoint should be rejected');
check(!Oidc::endpointAllowedForConfig('https://user@id.example/token', $config),
      'credential-bearing endpoint URL should be rejected');
$splitConfig = $config + ['allowedEndpointOrigins' => ['https://login.example:8443']];
check(Oidc::endpointAllowedForConfig('https://login.example:8443/oauth/token', $splitConfig),
      'explicitly allowlisted HTTPS origin should be accepted');
check(!Oidc::endpointAllowedForConfig('https://login.example/oauth/token', $splitConfig),
      'different port should not match an allowlisted origin');
check(!Oidc::endpointAllowedForConfig('https://login.example:8443/path',
      $config + ['allowedEndpointOrigins' => ['https://login.example:8443/path']]),
      'allowlist entries must be origins, not endpoint URLs');
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
$roleConfig = ['defaultRole' => 'viewer', 'operatorGroups' => ['netops'],
               'adminGroups' => ['domain admins']];
check(Oidc::roleForClaims(['groups' => ['/NETOPS']], $roleConfig) === 'operator',
      'fresh UserInfo operator membership should map to operator');
check(Oidc::roleForClaims(['groups' => ['/domain admins']], $roleConfig) === 'admin',
      'fresh UserInfo admin membership should map to admin');
check(Oidc::roleForClaims(['groups' => []], $roleConfig) === 'viewer',
      'removed privileged memberships should downgrade to the default role');
echo "OIDC claim and state checks passed\n";
