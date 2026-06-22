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
     * Reduce the raw request URI to a routable path. Every API route lives under
     * "/api", so we anchor on the first "/api" segment and discard whatever
     * mount prefix precedes it. This makes routing independent of where the app
     * is mounted — document root, an "/api" Apache alias, a subdirectory, or a
     * rewritten "index.php" left in the path — without depending on SCRIPT_NAME
     * (which the PHP built-in server and some SAPIs report inconsistently).
     */
    private function normalizePath(string $uri): string
    {
        $path = parse_url($uri, PHP_URL_PATH);
        if (!is_string($path) || $path === '') {
            return '/';
        }

        $pos = strpos($path, '/api/');
        if ($pos !== false) {
            return substr($path, $pos);          // ".../api/foo" -> "/api/foo"
        }
        if (str_ends_with($path, '/api')) {
            return '/api';
        }
        return $path;
    }
}
