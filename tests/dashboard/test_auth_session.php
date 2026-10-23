<?php
declare(strict_types=1);

require_once __DIR__ . '/../../dashboard/api/lib/Auth.php';

function requireTrue(bool $condition, string $message): void
{
    if (!$condition) {
        fwrite(STDERR, "FAIL: $message\n");
        exit(1);
    }
}

Auth::establishSession(['username' => 'alice', 'role' => 'admin', 'source' => 'oidc']);
requireTrue(Auth::current() !== null, 'new OIDC session should be valid');
$_SESSION['solari_created_at'] = time() - 3601;
requireTrue(Auth::current() === null, 'OIDC session must expire after one hour');

Auth::establishSession(['username' => 'bob', 'role' => 'operator', 'source' => 'oidc']);
$_SESSION['solari_seen_at'] = time() - 1801;
requireTrue(Auth::current() === null, 'idle session must expire');

$file = tempnam(sys_get_temp_dir(), 'solari-auth-test-');
requireTrue(is_string($file), 'temp file creation');
$rateDir = sys_get_temp_dir() . '/solari-session-rate-' . bin2hex(random_bytes(8));
mkdir($rateDir, 0700);
putenv('SOLARI_LOCAL_LOGIN=1');
putenv('SOLARI_AUTH_RATE_DIR=' . $rateDir);
file_put_contents($file, json_encode(['users' => [
    ['username' => 'carol', 'role' => 'viewer', 'displayName' => 'Carol']
]], JSON_THROW_ON_ERROR));
putenv('SOLARI_AUTH_FILE=' . $file);
Auth::establishSession(['username' => 'carol', 'role' => 'admin', 'source' => 'local']);
$current = Auth::current();
requireTrue($current !== null && $current['role'] === 'viewer',
            'local role downgrade must take effect on the next request');
putenv('SOLARI_LOCAL_LOGIN');
requireTrue(Auth::current() === null, 'disabling break-glass must revoke local sessions');
unlink($file);
rmdir($rateDir);
echo "Session expiry and local role revalidation passed\n";
