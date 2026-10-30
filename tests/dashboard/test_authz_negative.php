<?php
declare(strict_types=1);

/** Negative authorization cases run in isolated PHP processes because denial exits. */
$API = dirname(__DIR__, 2) . '/dashboard/api';
if (($scenario = getenv('SOLARI_AUTHZ_NEGATIVE_SCENARIO')) !== false && $scenario !== '') {
    require_once $API . '/lib/bootstrap.php';
    require_once $API . '/lib/Auth.php';
    require_once $API . '/lib/Operator.php';
    require_once $API . '/lib/Policy.php';
    register_shutdown_function(static function (): void {
        echo "\nAUTHZ_HTTP_STATUS:" . http_response_code() . "\n";
    });
    $_SERVER['HTTP_HOST'] = 'dash.test';
    $_SERVER['HTTP_ORIGIN'] = 'http://dash.test';
    $_SERVER['HTTP_X_SOLARI_CSRF'] = str_repeat('a', 64);
    session_save_path((string) getenv('SOLARI_AUTHZ_TEST_DIR'));

    $token = str_repeat('a', 64);
    $principal = ['username' => 'alice', 'role' => 'operator',
        'displayName' => 'Alice', 'source' => 'directory'];
    $setSession = static function (array $p, ?int $created = null) use ($token): void {
        Auth::bootSession();
        $_SESSION['solari'] = $p;
        $_SESSION['solari_csrf'] = $token;
        $_SESSION['solari_created_at'] = $created ?? time();
        $_SESSION['solari_seen_at'] = time();
    };

    switch ($scenario) {
        case 'wrong-username':
            $store = getenv('SOLARI_AUTHZ_TEST_DIR') . '/users.json';
            file_put_contents($store, json_encode(['version' => 1, 'users' => [
                ['username' => 'bob', 'role' => 'operator', 'displayName' => 'Bob'],
            ]]));
            $rate = getenv('SOLARI_AUTHZ_TEST_DIR') . '/rate';
            mkdir($rate, 0700);
            putenv('SOLARI_AUTH_FILE=' . $store);
            putenv('SOLARI_AUTH_RATE_DIR=' . $rate);
            putenv('SOLARI_LOCAL_LOGIN=1');
            $principal['source'] = 'local';
            $setSession($principal); // Alice session replayed after account now belongs only to Bob.
            break;
        case 'expired-session':
            $setSession($principal, time() - 86401);
            $_SESSION['solari_seen_at'] = time() - 1;
            break;
        case 'rotated-session-replay':
            $oldId = bin2hex(random_bytes(20));
            session_id($oldId);
            $setSession($principal);
            session_write_close();
            session_id($oldId);
            Auth::bootSession();
            Auth::establishSession($principal); // deletes the old id during rotation.
            session_write_close();
            session_id($oldId);                // replay the revoked identifier.
            Auth::bootSession();
            break;
        case 'stale-csrf-after-rotation':
            $setSession($principal);
            $oldToken = $_SESSION['solari_csrf'];
            Auth::establishSession($principal);
            $_SERVER['HTTP_X_SOLARI_CSRF'] = $oldToken;
            break;
        case 'demoted-role':
            $store = getenv('SOLARI_AUTHZ_TEST_DIR') . '/users.json';
            file_put_contents($store, json_encode(['version' => 1, 'users' => [
                ['username' => 'alice', 'role' => 'viewer', 'displayName' => 'Alice'],
            ]]));
            $rate = getenv('SOLARI_AUTHZ_TEST_DIR') . '/rate';
            mkdir($rate, 0700);
            putenv('SOLARI_AUTH_FILE=' . $store);
            putenv('SOLARI_AUTH_RATE_DIR=' . $rate);
            putenv('SOLARI_LOCAL_LOGIN=1');
            $principal['source'] = 'local';
            $setSession($principal); // stored viewer role supersedes old operator role.
            break;
        case 'wrong-origin':
            $setSession($principal);
            $_SERVER['HTTP_ORIGIN'] = 'https://evil.example';
            break;
        default:
            fwrite(STDERR, "unknown scenario\n");
            exit(2);
    }
    Policy::enforce('POST', '/api/test', []);
    echo 'HANDLER_EXECUTED';
    exit(0);
}

function negative_assert(bool $ok, string $message): void
{
    if (!$ok) { fwrite(STDERR, "FAIL: $message\n"); exit(1); }
}
$cases = [
    ['wrong-username', 'unauthorized', 401],
    ['expired-session', 'unauthorized', 401],
    ['rotated-session-replay', 'unauthorized', 401],
    ['stale-csrf-after-rotation', 'forbidden', 403],
    ['demoted-role', 'forbidden', 403],
    ['wrong-origin', 'forbidden', 403],
];
foreach ($cases as [$name, $expectedCode, $expectedStatus]) {
    $dir = sys_get_temp_dir() . '/solari-authz-' . bin2hex(random_bytes(8));
    negative_assert(mkdir($dir, 0700), "could not create isolated fixture dir for $name");
    $cmd = [PHP_BINARY, __FILE__];
    $proc = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null,
        $_ENV + ['SOLARI_AUTHZ_NEGATIVE_SCENARIO' => $name, 'SOLARI_AUTHZ_TEST_DIR' => $dir]);
    negative_assert(is_resource($proc), "could not launch $name");
    $out = stream_get_contents($pipes[1]);
    $err = stream_get_contents($pipes[2]);
    fclose($pipes[1]); fclose($pipes[2]);
    $exit = proc_close($proc);
    preg_match('/AUTHZ_HTTP_STATUS:(\d+)/', $out, $statusMatch);
    $status = isset($statusMatch[1]) ? (int) $statusMatch[1] : -1;
    $body = preg_replace('/\s*AUTHZ_HTTP_STATUS:\d+\s*$/', '', $out);
    $json = json_decode(trim($body), true);
    negative_assert($exit === 0, "$name exited $exit: $err");
    negative_assert(is_array($json) && ($json['error']['code'] ?? null) === $expectedCode,
        "$name expected error $expectedCode, got " . trim($body));
    negative_assert($status === $expectedStatus, "$name expected HTTP $expectedStatus, got $status");
    negative_assert(!str_contains($out, 'HANDLER_EXECUTED'), "$name reached handler");
    foreach (glob($dir . '/*') ?: [] as $file) { is_dir($file) ? rmdir($file) : unlink($file); }
    rmdir($dir);
}
printf("negative authorization suite passed (%d denied scenarios; status and handler boundary checked)\n", count($cases));
