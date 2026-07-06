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

    // GET /api/auth/config — public (unauthenticated) surface the login screen
    // reads to decide which sign-in options to show. Carries NO secrets: just
    // whether SSO is on and its button label. Without this the logged-out SPA
    // (which 401s on whoami) could never know an SSO option exists.
    $router->get('/api/auth/config', static function (): void {
        Response::ok([
            'localEnabled' => true,                  // local login is always available
            'oidcEnabled'  => Oidc::isEnabled(),
            'oidcLabel'    => Oidc::buttonLabel(),
            'oidcLoginUrl' => '/api/auth/oidc/login',
        ]);
    });

    // GET /api/auth/oidc/login — begin the authorization-code flow (302 to IdP).
    // A browser navigation, not an XHR: emits a redirect, not a JSON envelope.
    $router->get('/api/auth/oidc/login', static function (): void {
        Oidc::beginLogin();   // redirects + exits (or fails closed with a redirect)
    });

    // GET /api/auth/oidc/callback — IdP redirect target. Exchanges the code,
    // validates the ID token, establishes the session, then 302s into the app.
    $router->get('/api/auth/oidc/callback', static function (): void {
        Oidc::handleCallback($_GET);   // redirects + exits
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
            'oidcEnabled'      => Oidc::isEnabled(),
        ]);
    });
};
