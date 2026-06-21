<?php
declare(strict_types=1);

/**
 * Router — tiny path router for the SolariNet dashboard API.
 *
 * Routes are registered per HTTP method against a path pattern. Patterns use
 * "{name}" segments which are captured and passed to the handler as an
 * associative array of params, e.g. "/api/nodes/{id}/history".
 *
 * EXTENSION POINT FOR THE FOLLOW-UP AGENT
 * ---------------------------------------
 * This router is method-aware and already supports every verb. The read layer
 * registers only GET routes (see routes.php). To add the mutating endpoints and
 * SSE described in §6 / §11.2, register them on the same $router instance:
 *
 *     $router->post('/api/control/decommission', function (array $p) { ... });
 *     $router->sse ('/api/stream',                function (array $p) { ... });
 *
 * Handlers receive ($params, Router $router) and are expected to emit their own
 * response (typically via Response::ok()/Response::error(), which exit). POST
 * handlers should pull/validate the JSON body themselves and route mutations
 * through solariCtl over its Unix socket — PHP never touches cert/CA material
 * and never writes the monitoring tables directly (§11.1).
 */
final class Router
{
    /** @var array<int,array{method:string,regex:string,vars:string[],handler:callable}> */
    private array $routes = [];

    /**
     * Register a route.
     *
     * @param string   $method  HTTP method (GET/POST/...); "*" matches any.
     * @param string   $pattern Path pattern with optional "{name}" segments.
     * @param callable $handler fn(array $params, Router $self): void
     */
    public function add(string $method, string $pattern, callable $handler): void
    {
        $vars  = [];
        $regex = preg_replace_callback(
            '/\{([a-zA-Z_][a-zA-Z0-9_]*)\}/',
            static function (array $m) use (&$vars): string {
                $vars[] = $m[1];
                return '([^/]+)';
            },
            $pattern
        );
        $this->routes[] = [
            'method'  => strtoupper($method),
            'regex'   => '#^' . $regex . '/?$#',
            'vars'    => $vars,
            'handler' => $handler,
        ];
    }

    /** Convenience: register a GET route. */
    public function get(string $pattern, callable $handler): void
    {
        $this->add('GET', $pattern, $handler);
    }

    /** Convenience: register a POST route (for the follow-up mutation layer). */
    public function post(string $pattern, callable $handler): void
    {
        $this->add('POST', $pattern, $handler);
    }

    /**
     * Match the current request and dispatch. Emits a JSON error envelope for
     * unknown paths (404) and unsupported methods on a known path (405).
     */
    public function dispatch(string $method, string $path): void
    {
        $method = strtoupper($method);
        $path   = $this->normalizePath($path);

        $pathMatchedAnyMethod = false;

        foreach ($this->routes as $route) {
            if (!preg_match($route['regex'], $path, $m)) {
                continue;
            }
            $pathMatchedAnyMethod = true;

            if ($route['method'] !== $method && $route['method'] !== '*') {
                continue;
            }

            $params = [];
            foreach ($route['vars'] as $i => $name) {
                $params[$name] = rawurldecode($m[$i + 1]);
            }
            ($route['handler'])($params, $this);
            return; // handler is expected to have emitted a response and exited
        }

        if ($pathMatchedAnyMethod) {
            Response::error('method_not_allowed', "Method $method not allowed for $path", 405);
        }
        Response::error('not_found', "No route for $path", 404);
    }

    /**
     * Strip the script/base prefix and query string from the raw request URI so
     * routing works whether the API is served from the document root or a
     * subdirectory, and whether or not rewrites collapse "index.php".
     */
    private function normalizePath(string $uri): string
    {
        $path = parse_url($uri, PHP_URL_PATH);
        if (!is_string($path)) {
            $path = '/';
        }

        // Drop a trailing "/index.php" or the script's own directory prefix.
        $script = $_SERVER['SCRIPT_NAME'] ?? '';
        if ($script !== '') {
            $base = rtrim(str_replace('\\', '/', dirname($script)), '/');
            if ($base !== '' && $base !== '/' && str_starts_with($path, $base)) {
                $path = substr($path, strlen($base));
            }
        }
        $path = preg_replace('#/index\.php$#', '', $path) ?? $path;

        if ($path === '' ) {
            $path = '/';
        }
        return $path;
    }
}
