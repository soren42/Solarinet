# Wiring the Tab5 push-approval into the dashboard login

Additive, config-gated patches — mirrors how `deploy/nfc-2fa/` and `lib/Oidc.php`
are integrated. With `authbroker.enabled=false` (default) the dashboard behaves
exactly as today; nothing below changes the logged-out or single-factor paths.

## 1. Drop in the class

```
cp deploy/authbroker/dashboard/AuthBroker.php  dashboard/api/lib/AuthBroker.php
```

`lib/bootstrap.php` already autoloads everything in `lib/`, so no wiring is
needed for the class itself. It reads the same `solari-auth.json` store as
`Auth`/`Oidc` (no new config file).

## 2. Add the config block

In `solari-auth.json` (next to `users`, `directory`, `oidc`):

```json
"authbroker": {
  "enabled": true,
  "url": "http://127.0.0.1:9444",
  "token": "PASTE the [http] token from authbroker.conf",
  "enforce": "all",
  "timeoutSeconds": 60,
  "device": ""
}
```

## 3. Gate the two login paths

Both places already produce a fully-verified `$principal` *before* establishing
the session — insert the approval gate between verification and session
establishment so the second factor is mandatory.

### a) Local login — `dashboard/api/routes/auth.php`, `POST /api/auth/login`

`Auth::login()` both verifies AND establishes the session, so switch to the
two-step form to interpose the gate:

```php
$router->post('/api/auth/login', static function (): void {
    $body = solari_json_body();
    $user = (string) ($body['username'] ?? '');
    $pass = (string) ($body['password'] ?? '');

    $principal = Auth::attempt($user, $pass);          // verify only
    if ($principal === null) {
        Response::error('invalid_credentials', 'Invalid username or password.', 401);
    }

    // --- Tab5 push-approval second factor (no-op unless enabled) ---
    if (AuthBroker::isRequiredFor($principal)) {
        $ip = (string) ($_SERVER['REMOTE_ADDR'] ?? '');
        if (!AuthBroker::requireApproval($principal, $ip)) {
            Response::error('approval_denied',
                'Login was not approved on your authenticator.', 401);
        }
    }

    $principal = Auth::establishSession($principal);   // now start the session
    Response::ok([
        'operator' => $principal['username'], 'role' => $principal['role'],
        'displayName' => $principal['displayName'], 'source' => $principal['source'],
    ]);
});
```

(`Auth::attempt()` and `Auth::establishSession()` already exist — the latter is
the same session-establish used by the OIDC callback, so the two factors share
one session model.)

### b) OIDC callback — `dashboard/api/lib/Oidc.php`, `handleCallback()`

After the ID token is validated and the `$principal` is built, but before the
`Auth::establishSession($principal)` call, add the same gate:

```php
if (AuthBroker::isRequiredFor($principal)
    && !AuthBroker::requireApproval($principal, (string)($_SERVER['REMOTE_ADDR'] ?? ''))) {
    // redirect back to /login with ?error=approval_denied (match existing
    // fail-closed redirect style in handleCallback)
    self::failRedirect('approval_denied');
    return;
}
```

## 4. (Optional) Keycloak-native path

To require Tab5 approval for *every* Keycloak login (not just dashboard logins),
add a Keycloak **custom authenticator** (SPI) that, in its `authenticate()`,
calls the same `authbrokerd` HTTP API (`POST /auth/request` with the Keycloak
username + client as `subject`/`detail`) and only calls `context.success()` on
`decision == "approve"`. That reuses the identical broker + device; see the
broker README "Keycloak custom-authenticator path". The dashboard gate above is
the faster win and covers dashboard local login today.

## Notes

- `requireApproval()` **fails closed**: broker unreachable ⇒ login denied when
  approval is required. Keep `authbrokerd` on the dashboard host (localhost HTTP)
  so it is not an internet dependency.
- The call blocks up to `timeoutSeconds`; the login POST is already synchronous,
  so the browser simply waits for the tap. Consider a front-end "check your
  authenticator…" spinner (out of scope here).
