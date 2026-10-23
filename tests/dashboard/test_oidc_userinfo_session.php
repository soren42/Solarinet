<?php
declare(strict_types=1);

require_once __DIR__ . '/../../dashboard/api/lib/Auth.php';
require_once __DIR__ . '/../../dashboard/api/lib/Oidc.php';

function checkUserInfo(bool $condition, string $message): void
{
    if (!$condition) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

function writeState(string $path, array $state): void
{
    file_put_contents($path, json_encode($state, JSON_THROW_ON_ERROR));
}

$dir = sys_get_temp_dir() . '/solari-oidc-userinfo-' . bin2hex(random_bytes(8));
mkdir($dir, 0700);
$key = $dir . '/server.key';
$cert = $dir . '/server.pem';
$state = $dir . '/state.json';
$authFile = $dir . '/auth.json';
$proc = null;
$pipes = [];

try {
    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $openssl = proc_open([
        'openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
        '-keyout', $key, '-out', $cert, '-subj', '/CN=localhost',
        '-addext', 'subjectAltName=IP:127.0.0.1',
        '-addext', 'extendedKeyUsage=serverAuth',
    ], $descriptors, $certPipes);
    if (!is_resource($openssl)) throw new RuntimeException('could not launch openssl');
    fclose($certPipes[0]);
    stream_get_contents($certPipes[1]);
    $opensslError = stream_get_contents($certPipes[2]);
    fclose($certPipes[1]);
    fclose($certPipes[2]);
    if (proc_close($openssl) !== 0) throw new RuntimeException('openssl fixture failed: ' . $opensslError);

    writeState($state, ['sub' => 'subject-1', 'groups' => ['/domain admins']]);
    $proc = proc_open([
        'python3', __DIR__ . '/oidc_userinfo_fixture.py', $cert, $key, $state,
    ], $descriptors, $pipes);
    if (!is_resource($proc)) throw new RuntimeException('could not launch HTTPS fixture');
    fclose($pipes[0]);
    $port = (int) trim((string) fgets($pipes[1]));
    if ($port < 1 || $port > 65535) throw new RuntimeException('HTTPS fixture did not report a port');

    $issuer = 'https://127.0.0.1:' . $port;
    writeState($authFile, ['oidc' => [
        'enabled' => true,
        'issuer' => $issuer,
        'clientId' => 'solarinet',
        'clientSecret' => 'test-only',
        'redirectUri' => $issuer . '/callback',
        'caBundle' => $cert,
        'defaultRole' => 'viewer',
        'operatorGroups' => ['network operators'],
        'adminGroups' => ['domain admins'],
    ]]);
    putenv('SOLARI_AUTH_FILE=' . $authFile);
    session_save_path($dir);
    session_id(bin2hex(random_bytes(16)));
    Auth::establishSession([
        'username' => 'alice', 'role' => 'admin', 'source' => 'oidc',
        'oidcSubject' => 'subject-1',
    ]);
    $_SESSION['solari_oidc_access_token'] = 'access-current';
    $_SESSION['solari_oidc_refresh_token'] = 'refresh-current';
    $_SESSION['solari_oidc_access_expires_at'] = time() + 300;
    $_SESSION['solari_oidc_role_checked_at'] = time() - 61;
    $principal = Auth::current();
    checkUserInfo(is_array($principal) && $principal['role'] === 'admin',
                  'fresh UserInfo admin membership should preserve admin');

    writeState($state, ['sub' => 'subject-1', 'groups' => ['/network operators']]);
    $_SESSION['solari_oidc_role_checked_at'] = time() - 61;
    $principal = Auth::current();
    checkUserInfo(is_array($principal) && $principal['role'] === 'operator',
                  'removed admin group and current operator group should downgrade the role');

    writeState($state, ['sub' => 'subject-1', 'groups' => ['/domain admins']]);
    $_SESSION['solari_oidc_access_expires_at'] = time() - 1;
    $_SESSION['solari_oidc_role_checked_at'] = time() - 61;
    $principal = Auth::current();
    checkUserInfo(is_array($principal) && $principal['role'] === 'admin',
                  'refresh grant should obtain a new access token before UserInfo');
    checkUserInfo($_SESSION['solari_oidc_refresh_token'] === 'refresh-rotated',
                  'rotated refresh token should replace the prior token');

    writeState($state, ['sub' => 'subject-1', 'groups' => ['/domain admins'], 'status' => 503]);
    $_SESSION['solari_oidc_role_checked_at'] = time() - 61;
    $principal = Auth::current();
    checkUserInfo(is_array($principal) && $principal['role'] === 'viewer',
                  'provider error must remove privileged role while preserving read access');

    writeState($state, ['oversized' => true]);
    $_SESSION['solari_oidc_role_checked_at'] = time() - 61;
    $principal = Auth::current();
    checkUserInfo(is_array($principal) && $principal['role'] === 'viewer',
                  'oversized provider responses must be bounded and remove privileged access');

    writeState($state, ['sub' => 'different-subject', 'groups' => ['/domain admins']]);
    $_SESSION['solari_oidc_role_checked_at'] = time() - 61;
    checkUserInfo(Auth::current() === null,
                  'changed UserInfo subject must revoke the session');
    echo "OIDC UserInfo role refresh, token rotation, and fail-closed tests passed\n";
} finally {
    if (is_resource($proc)) {
        proc_terminate($proc);
        fclose($pipes[1]);
        fclose($pipes[2]);
        proc_close($proc);
    }
    foreach ([$key, $cert, $state, $authFile] as $file) {
        if (is_file($file)) unlink($file);
    }
    if (is_dir($dir)) rmdir($dir);
}
