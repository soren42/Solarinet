<?php
declare(strict_types=1);

/**
 * The former SSE route held one PHP-FPM worker per open browser tab for up to
 * 50 seconds. Dashboard state now uses bounded 10-second HTTP polling while a
 * dedicated event bridge and a monotonic source cursor are designed. HTTP 204
 * tells an old EventSource client to stop reconnecting immediately.
 */
return static function (Router $router): void {
    $router->get('/api/stream', static function (): void {
        if (!headers_sent()) {
            http_response_code(204);
            header('Cache-Control: no-store');
        }
        exit;
    });
};
