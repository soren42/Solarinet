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
            // Hand the SPA its CSRF token so subsequent writes carry X-Solari-CSRF.
            'csrfToken'   => Auth::csrfToken(),
        ]);
    }, ['auth' => 'public']);   // pre-session: no role/token required to log in

    // POST /api/auth/logout — authenticated session, CSRF-protected like any write.
    $router->post('/api/auth/logout', static function (): void {
        Auth::logout();
        Response::ok(['loggedOut' => true]);
    }, ['auth' => 'session']);

    // GET /api/auth/config — public (unauthenticated) surface the login screen
    // reads to decide which sign-in options to show. Carries NO secrets: just
    // whether SSO is on and its button label. Without this the logged-out SPA
    // (which 401s on whoami) could never know an SSO option exists.
    $router->get('/api/auth/config', static function (): void {
        // Non-secret identity surface. The issuer, clientId and scopes are already
        // visible to the browser during the redirect, so exposing them here (for
        // the login screen AND the Config → Identity & SSO panel) leaks nothing;
        // clientSecret / caBundle are deliberately never included. A friendly
        // provider name is inferred from the issuer URL (Keycloak realms etc.).
        $oc       = Oidc::config();
        $issuer   = (string) ($oc['issuer'] ?? '');
        $provider = 'OpenID Connect';
        if ($issuer !== '') {
            if (stripos($issuer, 'keycloak') !== false || stripos($issuer, '/realms/') !== false) {
                $provider = 'Keycloak';
            } elseif (stripos($issuer, 'login.microsoftonline') !== false || stripos($issuer, 'sts.windows') !== false) {
                $provider = 'Microsoft Entra ID';
            } elseif (stripos($issuer, 'accounts.google') !== false) {
                $provider = 'Google';
            }
        }
        // Directory (AD/LDAP) block — surface only whitelisted, non-secret fields.
        $dir     = Auth::directoryConfig();
        $dirOut  = ['enabled' => (bool) ($dir['enabled'] ?? false)];
        foreach (['type', 'realm', 'domain', 'host'] as $k) {
            if (isset($dir[$k]) && is_string($dir[$k]) && $dir[$k] !== '') {
                $dirOut[$k] = $dir[$k];
            }
        }

        Response::ok([
            // Directory (AD) users sign in via SSO; local username/password login
            // (Auth::directoryAuthenticate is a stub for directory accounts) is
            // hidden by default so it isn't a dead-end. Re-enable by setting the
            // fpm-pool env SOLARI_LOCAL_LOGIN=1 (the /api/auth/login endpoint still
            // exists as a hidden break-glass path for any real local account).
            'localEnabled' => getenv('SOLARI_LOCAL_LOGIN') === '1',
            'oidcEnabled'  => Oidc::isEnabled(),
            'oidcLabel'    => Oidc::buttonLabel(),
            'oidcLoginUrl' => '/api/auth/oidc/login',
            // read-only identity detail (safe to expose; no secrets)
            'oidcProvider' => $provider,
            'oidcIssuer'   => $issuer,
            'oidcClientId' => (string) ($oc['clientId'] ?? ''),
            'oidcScopes'   => (string) ($oc['scopes'] ?? ''),
            'directory'    => $dirOut,
        ]);
    }, ['auth' => 'public']);   // login screen reads this before any session exists

    // GET /api/auth/oidc/login — begin the authorization-code flow (302 to IdP).
    // A browser navigation, not an XHR: emits a redirect, not a JSON envelope.
    $router->get('/api/auth/oidc/login', static function (): void {
        Oidc::beginLogin();   // redirects + exits (or fails closed with a redirect)
    }, ['auth' => 'public']);

    // GET /api/auth/oidc/callback — IdP redirect target. Exchanges the code,
    // validates the ID token, establishes the session, then 302s into the app.
    $router->get('/api/auth/oidc/callback', static function (): void {
        Oidc::handleCallback($_GET);   // redirects + exits
    }, ['auth' => 'public']);

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
            // The SPA re-reads its CSRF token here on boot / session resume, so a
            // browser with an existing cookie can write without re-authenticating.
            'csrfToken'   => Auth::csrfToken(),
        ]);
    }, ['auth' => 'session']);
};
