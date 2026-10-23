<?php
declare(strict_types=1);

if (getenv('SOLARI_STREAM_CHILD') === '1') {
    final class Router {
        public $handler;
        public function get(string $path, callable $handler): void {
            if ($path !== '/api/stream') exit(2);
            $this->handler = $handler;
        }
    }
    register_shutdown_function(static function (): void {
        fwrite(STDERR, 'STATUS:' . http_response_code() . "\n");
    });
    $register = require __DIR__ . '/../../dashboard/api/routes/stream.php';
    $router = new Router();
    $register($router);
    ($router->handler)();
    exit(3);
}

$environment = $_ENV;
$environment['SOLARI_STREAM_CHILD'] = '1';
$start = microtime(true);
$process = proc_open([PHP_BINARY, __FILE__],
                     [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes,
                     null, $environment);
if (!is_resource($process)) exit(1);
$output = stream_get_contents($pipes[1]);
$error = stream_get_contents($pipes[2]);
fclose($pipes[1]);
fclose($pipes[2]);
$status = proc_close($process);
if ($status !== 0 || trim((string) $output) !== ''
    || !str_contains((string) $error, 'STATUS:204')
    || microtime(true) - $start > 2) {
    fwrite(STDERR, "FAIL: stream should return immediate empty 204: $error\n");
    exit(1);
}
echo "Stream worker release passed\n";
