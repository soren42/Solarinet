# Wiring NFC 2FA into the live dashboard

The files under `deploy/nfc-2fa/dashboard/` are a **scaffold** — they are NOT
copies-in-place of the live `dashboard/` tree and nothing here overwrites a live
file. To activate, copy the new files and apply the small additive patches below.
Every change is guarded so that with `nfc2fa.enabled=false` the dashboard behaves
exactly as today.

## 1. Copy the new files (no existing file is replaced)

```
dashboard/api/lib/Nfc2fa.php          <- deploy/nfc-2fa/dashboard/api/lib/Nfc2fa.php
dashboard/api/routes/nfc2fa.php       <- deploy/nfc-2fa/dashboard/api/routes/nfc2fa.php
dashboard/public/nfc2fa.jsx           <- deploy/nfc-2fa/dashboard/public/nfc2fa.jsx
```

## 2. Patch `dashboard/api/index.php`

Load the class and register the routes alongside the existing auth routes, and
teach the auth gate about the pending-2FA state.

```php
// after: require_once __DIR__ . '/lib/Oidc.php';
require_once __DIR__ . '/lib/Nfc2fa.php';

// after: $registerAuth($router);
$registerNfc = require __DIR__ . '/routes/nfc2fa.php';
$registerNfc($router);
```

The auth gate already lets `/api/auth/*` through, so `/api/auth/nfc/verify` is
reachable by a pending-2FA session with no gate change. A pending session is NOT
elevated (Nfc2fa stores the principal in a transaction key, not `$_SESSION['solari']`),
so `Auth::current()` still returns null and every non-auth endpoint stays 401
until the card is verified. **No gate edit is required** — the pending state is
invisible to `requireSession()` by construction.

## 3. Patch `dashboard/api/routes/auth.php` — the login step

Make the local login pause for the card when required. Replace the body of the
`POST /api/auth/login` success branch:

```php
$principal = Auth::attempt($user, $pass);   // was: Auth::login(...)
if ($principal === null) {
    Response::error('invalid_credentials', 'Invalid username or password.', 401);
}
if (Nfc2fa::isRequiredFor($principal['username'])) {
    // Primary OK, card still required: begin the second factor (session stays
    // pending; NOT elevated). The SPA shows the "tap your card" prompt.
    Response::ok(Nfc2fa::beginVerify($principal));   // {stage:"nfc_required",challenge,...}
}
// No 2FA required (feature off, or user has no card under enforce=enrolled):
$principal = Auth::login($user, $pass);     // establishes the full session as today
Response::ok([
    'stage' => 'authenticated',
    'operator' => $principal['username'], 'role' => $principal['role'],
    'displayName' => $principal['displayName'], 'source' => $principal['source'],
]);
```

Note `Auth::attempt()` already exists (public) and only verifies credentials
without establishing a session — exactly what the pending path needs.

Also add `'nfcEnabled' => Nfc2fa::isEnabled()` to the `GET /api/auth/config`
payload so the login screen knows the second factor may appear.

## 4. Patch `dashboard/api/lib/Oidc.php` — the SSO path (approach (a))

In `handleCallback()`, where it currently calls `Auth::establishSession($principal)`,
branch on 2FA:

```php
if (Nfc2fa::isRequiredFor($principal['username'])) {
    Nfc2fa::beginVerify($principal);            // session pending, not elevated
    self::redirect(self::config()['postLoginRedirect'] . '?nfc=1');  // SPA shows tap prompt
    return;
}
Auth::establishSession($principal);             // unchanged when 2FA off
```

The SPA, seeing `?nfc=1` (or a whoami that reports pending), renders the tap
prompt and POSTs to `/api/auth/nfc/verify`. See DESIGN.md §6(b) for the
Keycloak-native alternative (a Java Authenticator SPI) if the factor must be
enforced realm-wide rather than app-side.

## 5. Small helper to add to `dashboard/api/lib/Auth.php` (optional)

`Nfc2fa::persist()` calls `Auth::forgetStore()` (guarded by `method_exists`) after
writing the credential file, so a same-request read sees the update. Login and
verify are separate requests, so this is only needed if you ever enrol and read
back in one request. To add it:

```php
/** Drop the cached credential store so the next read re-loads from disk. */
public static function forgetStore(): void { self::$store = null; }
```

## 6. Front-end

Include `nfc2fa.jsx` after `app.jsx` in the dashboard HTML. It exports
`window.SolariNfc` with `<TapPrompt>` (2FA step) and `<EnrollCard>` (admin UI).
`app.jsx`'s `LoginScreen.submit` should, instead of `window.location.reload()`,
check the login response `stage`:

```js
api.login(u, p).then((res) => {
  if (res && res.stage === "nfc_required") {
    setNfc({ challenge: res.challenge, expiresIn: res.expiresIn });  // show <TapPrompt>
  } else {
    window.location.reload();   // fully authenticated
  }
});
```

`<TapPrompt>` reads the card from the local daemon
(`http://127.0.0.1:8770/read?challenge=…`) and POSTs `{challenge, uid}` to
`/api/auth/nfc/verify`; on `stage:"authenticated"` it reloads.
