<?php
declare(strict_types=1);

require_once __DIR__ . '/../../dashboard/api/lib/Auth.php';

$before = getenv('SOLARI_LOCAL_LOGIN');
$beforeDir = getenv('SOLARI_AUTH_RATE_DIR');
putenv('SOLARI_LOCAL_LOGIN');
putenv('SOLARI_AUTH_RATE_DIR');
if (Auth::localLoginEnabled() || Auth::login('anyone', 'anything') !== null) {
    fwrite(STDERR, "FAIL: local password login should default closed\n");
    exit(1);
}
putenv('SOLARI_LOCAL_LOGIN=1');
if (Auth::localLoginEnabled()) {
    fwrite(STDERR, "FAIL: missing rate-limit directory must fail closed\n");
    exit(1);
}
$dir = sys_get_temp_dir() . '/solari-rate-test-' . bin2hex(random_bytes(8));
mkdir($dir, 0700);
putenv('SOLARI_AUTH_RATE_DIR=' . $dir);
if (!Auth::localLoginEnabled()) {
    fwrite(STDERR, "FAIL: explicit local login switch was ignored\n");
    exit(1);
}
for ($i = 0; $i < 5; $i++) {
    if (!Auth::consumeLocalAttempt('alice', '127.0.0.1')) {
        fwrite(STDERR, "FAIL: attempt $i should be permitted\n");
        exit(1);
    }
}
if (Auth::consumeLocalAttempt('alice', '127.0.0.1')) {
    fwrite(STDERR, "FAIL: sixth attempt should be limited\n");
    exit(1);
}
foreach (glob($dir . '/*') ?: [] as $file) unlink($file);
rmdir($dir);
if ($before === false) {
    putenv('SOLARI_LOCAL_LOGIN');
} else {
    putenv('SOLARI_LOCAL_LOGIN=' . $before);
}
if ($beforeDir === false) {
    putenv('SOLARI_AUTH_RATE_DIR');
} else {
    putenv('SOLARI_AUTH_RATE_DIR=' . $beforeDir);
}
echo "Local login gate passed\n";
