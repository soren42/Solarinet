<?php
declare(strict_types=1);

/**
 * solari-passwd.php — create/update a local dashboard credential.
 *
 * Writes a salted password hash (PHP password_hash / bcrypt — the modern
 * "$2y$salt$hash" shadow form) into the JSON credential file the dashboard
 * reads (solari-auth.json in the project root, or $SOLARI_AUTH_FILE).
 *
 * Usage:
 *   php solari-passwd.php [--user NAME] [--role admin|operator|viewer]
 *                        [--password PASS | --random] [--file PATH]
 *
 *   --user NAME      username to create/update            (default: admin)
 *   --role ROLE      authorization role                   (default: admin)
 *   --password PASS  set this password (else prompt, or --random)
 *   --random         generate a strong random password and print it once
 *   --file PATH      credential file                      (default: project root)
 *
 * Re-running for an existing user updates that user's password/role in place;
 * other users and the [directory] stub block are preserved.
 */

function fail(string $m): void { fwrite(STDERR, "error: $m\n"); exit(1); }

$opts = [
    'user'     => 'admin',
    'role'     => 'admin',
    'password' => null,
    'random'   => false,
    'file'     => getenv('SOLARI_AUTH_FILE') ?: (dirname(__DIR__, 3) . '/solari-auth.json'),
];

$argvv = array_slice($argv, 1);
for ($i = 0; $i < count($argvv); $i++) {
    $a = $argvv[$i];
    switch ($a) {
        case '--user':     $opts['user']     = $argvv[++$i] ?? fail('--user needs a value'); break;
        case '--role':     $opts['role']     = $argvv[++$i] ?? fail('--role needs a value'); break;
        case '--password': $opts['password'] = $argvv[++$i] ?? fail('--password needs a value'); break;
        case '--random':   $opts['random']   = true; break;
        case '--file':     $opts['file']      = $argvv[++$i] ?? fail('--file needs a value'); break;
        case '-h': case '--help':
            fwrite(STDOUT, file_get_contents(__FILE__, false, null, 0, 1400)); exit(0);
        default: fail("unknown arg: $a");
    }
}

$role = strtolower((string) $opts['role']);
if (!in_array($role, ['admin', 'operator', 'viewer'], true)) {
    fail("role must be admin|operator|viewer");
}

// Resolve the plaintext password.
$plain = $opts['password'];
$generated = false;
if ($plain === null) {
    if ($opts['random']) {
        // 18 url-safe bytes -> ~24 char password.
        $plain = rtrim(strtr(base64_encode(random_bytes(18)), '+/', '-_'), '=');
        $generated = true;
    } else {
        // Interactive prompt (no echo if a TTY).
        fwrite(STDOUT, "New password for '{$opts['user']}': ");
        @shell_exec('stty -echo 2>/dev/null');
        $plain = trim((string) fgets(STDIN));
        @shell_exec('stty echo 2>/dev/null');
        fwrite(STDOUT, "\n");
        if ($plain === '') fail('empty password');
    }
}

$hash = password_hash($plain, PASSWORD_DEFAULT);
if ($hash === false) fail('password_hash failed');

// Load or create the store.
$store = ['version' => 1, 'users' => [], 'directory' => null];
if (is_file($opts['file'])) {
    $decoded = json_decode((string) file_get_contents($opts['file']), true);
    if (is_array($decoded)) $store = $decoded + $store;
}
if (!is_array($store['users'] ?? null)) $store['users'] = [];

// Default directory-service stub block (preserved if already present).
if (empty($store['directory']) || !is_array($store['directory'])) {
    $store['directory'] = [
        'enabled'        => false,
        '_comment'       => 'Directory-service (LDAP/AD) integration stub. NOT yet implemented; see dashboard/api/lib/Auth.php::directoryAuthenticate(). Fill these in and set enabled=true once the DS is deployed.',
        'type'           => 'ldap',
        'host'           => '',
        'port'           => 636,
        'useTls'         => true,
        'baseDn'         => '',
        'bindDnTemplate' => 'uid=%s,ou=people,dc=example,dc=intranet',
        'roleAttribute'  => 'memberOf',
        'roleMap'        => ['cn=solari-admins,*' => 'admin', 'cn=solari-ops,*' => 'operator'],
    ];
}

// Upsert the user.
$found = false;
foreach ($store['users'] as &$u) {
    if (is_array($u) && ($u['username'] ?? null) === $opts['user']) {
        $u['passwordHash'] = $hash;
        $u['role']         = $role;
        if (empty($u['displayName'])) $u['displayName'] = $opts['user'];
        $found = true;
        break;
    }
}
unset($u);
if (!$found) {
    $store['users'][] = [
        'username'     => $opts['user'],
        'passwordHash' => $hash,
        'role'         => $role,
        'displayName'  => $opts['user'],
    ];
}

$json = json_encode($store, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n";
$old = umask(0077);
if (file_put_contents($opts['file'], $json, LOCK_EX) === false) {
    umask($old); fail("could not write {$opts['file']}");
}
@chmod($opts['file'], 0600);
umask($old);

fwrite(STDOUT, "wrote {$opts['file']} (user='{$opts['user']}', role='{$role}')\n");
if ($generated) {
    fwrite(STDOUT, "generated password: {$plain}\n");
    fwrite(STDOUT, "  ^ store this now; it is not recoverable from the hash.\n");
}
