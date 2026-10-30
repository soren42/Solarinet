<?php
declare(strict_types=1);

/** Compare the checked-in authz manifest with the real route registration table. */
$API = dirname(__DIR__, 2) . '/dashboard/api';
$_SERVER['REQUEST_METHOD'] = 'GET';
$_SERVER['REQUEST_URI'] = '/';
require_once $API . '/lib/bootstrap.php';
require_once $API . '/lib/Auth.php';
require_once $API . '/lib/Oidc.php';
require_once $API . '/lib/Operator.php';
require_once $API . '/lib/Policy.php';

function fail_manifest(string $message): never
{
    fwrite(STDERR, "FAIL: $message\n");
    exit(1);
}

$manifestPath = dirname(__DIR__, 2) . '/docs/authz/AUTHZ_MANIFEST.json';
$manifest = json_decode((string) file_get_contents($manifestPath), true);
if (!is_array($manifest) || !is_array($manifest['phpRoutes'] ?? null)) {
    fail_manifest('manifest JSON/phpRoutes missing or invalid');
}
$router = solari_router();
(require $API . '/routes/auth.php')($router);
(require $API . '/routes.php')($router);
(require $API . '/routes_mutations.php')($router);
$prop = (new ReflectionClass(Router::class))->getProperty('routes');
$actualRoutes = $prop->getValue($router);
$toKey = static function (array $entry): string {
    return implode("\x1f", [$entry['method'], $entry['pattern'], $entry['roleRequired'],
        $entry['csrf'], $entry['servicePrincipal'] ?? '']);
};
$actual = [];
foreach ($actualRoutes as $route) {
    $pattern = substr($route['regex'], 2, -4);
    foreach ($route['vars'] as $var) {
        $pattern = preg_replace('/\(\[\^\/\]\+\)/', '{' . $var . '}', $pattern, 1);
    }
    $role = Policy::resolveAuth($route['method'], $route['policy']);
    $write = Policy::isWrite($route['method']);
    $csrf = ($role === 'public' || !$write) ? 'none'
        : ((($route['policy']['csrf'] ?? null) === 'panel-service') ? 'panel-service'
            : 'origin+session-token');
    $entry = ['method' => $route['method'], 'pattern' => $pattern,
        'roleRequired' => $role, 'csrf' => $csrf];
    if ($csrf === 'panel-service') {
        $entry['servicePrincipal'] = 'local panel account (viewer)';
    }
    $actual[] = $toKey($entry);
}
$declared = array_map($toKey, $manifest['phpRoutes']);
if (count(array_unique($actual)) !== count($actual)) fail_manifest('duplicate route in runtime route table');
if (count(array_unique($declared)) !== count($declared)) fail_manifest('duplicate route in manifest');
sort($actual);
sort($declared);
if ($actual !== $declared) {
    $missing = array_diff($actual, $declared);
    $stale = array_diff($declared, $actual);
    fail_manifest('route drift; absent from manifest: ' . implode(', ', $missing)
        . '; stale/mismatched manifest entries: ' . implode(', ', $stale));
}
printf("authorization manifest matches %d registered PHP routes\n", count($actual));
