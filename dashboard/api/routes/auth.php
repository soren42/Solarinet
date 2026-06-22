<?php
declare(strict_types=1);

/**
 * Auth routes (§11.1) — login / logout / whoami.
 *
 * These are the ONLY routes reachable without an established session (the gate
 * in index.php lets /api/auth/* through); every other endpoint requires a
 * logged-in principal. Single-admin local mode for now; the directory-service
 * path is stubbed in Auth.php for the next pass.
 */

return static function (Router $router): void {

    // POST /api/auth/login  body: { username, password }
    $router->post('/api/auth/login', static function (): void {
        $body = solari_json_body();
        $user = (string) ($body['username'] ?? '');
        $pass = (string) ($body['password'] ?? '');

        $principal = Auth::login($user, $pass);
        if ($principal === null) {
            // Uniform message: do not reveal whether the user or password was wrong.
            Response::error('invalid_credentials', 'Invalid username or password.', 401);
        }
        Response::ok([
            'operator'    => $principal['username'],
            'role'        => $principal['role'],
            'displayName' => $principal['displayName'],
            'source'      => $principal['source'],
        ]);
    });

    // POST /api/auth/logout
    $router->post('/api/auth/logout', static function (): void {
        Auth::logout();
        Response::ok(['loggedOut' => true]);
    });

    // GET /api/auth/whoami — the SPA polls this on boot to decide login vs app.
    $router->get('/api/auth/whoami', static function (): void {
        $p = Auth::current();
        if ($p === null) {
            Response::error('unauthorized', 'Not authenticated.', 401);
        }
        Response::ok([
            'operator'    => $p['username'],
            'role'        => $p['role'],
            'displayName' => $p['displayName'],
            'source'      => $p['source'],
            'directoryEnabled' => (bool) (Auth::directoryConfig()['enabled'] ?? false),
        ]);
    });
};
