# SolariNet NFC-Card Two-Factor Authentication — Design

Status: **DESIGN + SCAFFOLD** (2026-07-06). No working reader on-site tonight; see
`HARDWARE_TODO.md` for what makes this live. This document is the authoritative
spec; the scaffold under `dashboard/` and `reader/` implements the inert,
config-gated skeleton described here.

---

## 1. Goal and scope

Add an **NFC card as a second factor** on top of SolariNet's existing *primary*
authentication. NFC never replaces primary auth; it gates the elevation from
"primary verified" to "fully authenticated session."

Primary auth today (unchanged):
- **Local**: `dashboard/api/lib/Auth.php` — bcrypt hashes in `solari-auth.json`,
  server-side PHP session (`solari_sess`, HttpOnly, SameSite=Lax).
- **SSO/OIDC**: `dashboard/api/lib/Oidc.php` — Keycloak realm `akoria` on
  `sso.akoria.org:8443`, authorization-code flow, `Auth::establishSession()`.

The second factor must be:
- **Additive and config-gated** (`nfc2fa.enabled`). When off, login behaves
  exactly as today. A half-configured block reads as *off* (mirrors the OIDC
  `isEnabled()` pattern).
- **Fail-closed** when enabled *for a user who has an enrolled card*: no card, no
  session elevation. Fail-**open is explicitly rejected** as a security posture.
- Revocable per-card, per-user, by an admin.

---

## 2. Threat model

### 2.1 What an NFC card actually is

The dominant deployed card tech and what its "UID" is worth:

| Card type            | "UID" / identifier          | Secret? | Cloneable? |
|----------------------|-----------------------------|---------|------------|
| MIFARE Classic 1K    | 4/7-byte UID                | No      | Trivially (magic/gen1a cards) |
| NTAG213/215/216      | 7-byte UID                  | No      | UID readable by any reader; "magic" NTAGs exist |
| MIFARE Ultralight C  | 7-byte UID + 3DES auth      | Auth key is secret | UID clonable; auth key not (if used) |
| MIFARE DESFire EV1/2/3 | 7-byte UID + AES/3DES app keys | App keys secret | UID clonable; **crypto not** |
| ISO-14443-4 / JavaCard | UID + applet challenge-response | Applet secret | UID clonable; applet not |

**The card UID is NOT a secret.** It is transmitted in the clear in the
anticollision phase of every ISO-14443 tap and is readable by any $30 reader or a
phone. Many cards' UIDs are also directly writable ("magic" cards), so a UID can
be *cloned onto a blank*. Treating the UID as a password is therefore wrong.

### 2.2 What we can and cannot defend against

We are protecting an **operator dashboard on a trusted homelab intranet** (this is
the user's own network; NFC/RFID admin here is normal). The realistic threats:

| Threat | Mitigated by |
|--------|--------------|
| Stolen/guessed password alone | NFC possession factor — password alone cannot elevate |
| Attacker with network access replaying a captured verify request | Server-issued single-use **challenge nonce** bound to the pending-2FA session; nonce consumed on use; short TTL |
| Attacker who read a victim's UID (skimming) and cloned it | **Only** defended in *crypto mode* (DESFire/NTAG-with-auth). In *UID mode* this is an accepted residual risk — documented below |
| Brute-forcing which UID is enrolled | UIDs stored **salted+hashed** (never plaintext); constant-time compare; per-account lockout after N failed taps |
| Reader host compromise (the box the card is tapped on) | Out of scope — a compromised reader host can always relay. We bind the reader to localhost and require the operator's authenticated browser session to initiate |
| Enumeration of enrolled users | Verify returns uniform failures; no "this user has no card" oracle when 2FA globally enabled |

### 2.3 Two possession models — pick per card capability

**MODE A — UID-as-identifier (baseline, works with any card incl. MIFARE/NTAG).**
The UID is an *identifier of a physical, enrolled, revocable token*, not a secret.
Security value: an attacker needs **both** the password **and** physical
possession of *a* card whose UID matches an enrolment. Residual risk: a skimmed +
cloned UID defeats it. This is **strictly better than password-only** and is the
tonight-shippable default. It MUST be labelled in the UI as "UID mode" so the
operator knows the assurance level.

**MODE B — Challenge-response (preferred; requires DESFire EV1+/NTAG w/ auth or a
JavaCard applet).** Enrolment provisions/records a per-card AES (DESFire) key or
applet secret. Verify: server issues a random challenge → reader asks the card to
authenticate / sign → response proves the card holds the secret **without
revealing it** and **cannot be replayed** (fresh challenge each time). A cloned
UID is worthless without the key. This defeats skimming/cloning.

The system stores a per-credential `mode` (`uid` | `crypto`) so both can coexist:
low-assurance cards get Mode A, DESFire cards get Mode B, and the verify path
branches on the stored mode. **Design for B, ship A first.**

> Decision: implement Mode A end-to-end now (works with whatever card is on hand),
> structure the store + reader + verify API so Mode B slots in without schema or
> wire-format changes (the reader already returns card `type` and an optional
> `crypto` challenge-response envelope; see §5).

---

## 3. Data model — where enrolments live

Two backends, same logical shape (the coming MariaDB SoR is authoritative once it
exists; the JSON store is the tonight path since local auth already lives there).

### 3.1 JSON store (tonight) — `solari-auth.json`

Add a top-level `nfc2fa` block and a per-user `nfcCards` array:

```jsonc
{
  "version": 1,
  "nfc2fa": {
    "enabled": false,           // master gate; false => system behaves as today
    "enforce": "enrolled",      // "enrolled" = require 2FA only for users with a
                                //   card; "all" = require every user to enrol
                                //   (blocks login until enrolled — admin escape
                                //   hatch documented in HARDWARE_TODO)
    "maxFailures": 5,           // per-account tap failures before lockout
    "lockoutSeconds": 900,      // lockout window
    "challengeTtlSeconds": 60,  // pending-2FA / challenge lifetime
    "readerHmacKeyRef": "env:SOLARI_NFC_READER_KEY"  // optional shared secret
                                //   authenticating the reader daemon to PHP
  },
  "users": [
    {
      "username": "jason",
      "passwordHash": "$2y$...",
      "role": "admin",
      "nfcCards": [
        {
          "id": "card-a1b2",           // opaque handle for revocation/UI
          "mode": "uid",               // "uid" | "crypto"
          "label": "Blue keyfob",
          "uidSalt": "base64...",      // per-card random salt
          "uidHash": "base64...",      // = argon2id/sha256(salt || rawUID)
          "cardType": "NTAG215",
          "enrolledAt": "2026-07-06T04:00:00Z",
          "enrolledBy": "jason",
          "lastUsedAt": null,
          "revoked": false,
          "revokedAt": null,
          // crypto mode only:
          "keyRef": null               // ref to DESFire app key (never inline)
        }
      ]
    }
  ]
}
```

Rationale for hash+salt: the UID is not a password, but hashing it means a leak of
`solari-auth.json` does not immediately hand an attacker the exact bytes to write
onto a magic card. A **per-card random salt** blocks precomputation and cross-user
correlation. (This is defense-in-depth, not a substitute for Mode B.)

### 3.2 MariaDB SoR (when it lands) — `credentials` / `tokens`

Mirror the JSON shape. Suggested DDL (informative; the C server owns writes per
the §11.1 boundary — PHP proposes, `solariCtl` persists):

```sql
CREATE TABLE nfcCredential (
  id             VARCHAR(64) PRIMARY KEY,
  username       VARCHAR(128) NOT NULL,
  mode           ENUM('uid','crypto') NOT NULL DEFAULT 'uid',
  label          VARCHAR(128),
  uid_salt       VARBINARY(32) NOT NULL,
  uid_hash       VARBINARY(64) NOT NULL,       -- argon2id/sha256(salt||uid)
  card_type      VARCHAR(64),
  key_ref        VARCHAR(128),                 -- crypto mode: opaque key handle
  enrolled_at    DATETIME NOT NULL,
  enrolled_by    VARCHAR(128) NOT NULL,
  last_used_at   DATETIME,
  revoked        TINYINT(1) NOT NULL DEFAULT 0,
  revoked_at     DATETIME,
  KEY (username), KEY (uid_hash)
);
CREATE TABLE nfc2faAttempt (              -- lockout / audit
  username    VARCHAR(128) NOT NULL,
  at          DATETIME NOT NULL,
  outcome     ENUM('ok','bad_card','no_match','locked','expired') NOT NULL,
  source_ip   VARCHAR(64),
  KEY (username, at)
);
```

**Never store the raw UID or any DESFire key in plaintext at rest.** DESFire app
keys, if we diversify per-card, are derived from a master key held only by the C
server / an HSM-style keystore, referenced by `key_ref`.

---

## 4. Flows

### 4.1 Enrolment (admin binds a card to a user)

Preconditions: caller has an **elevated** admin session (primary + already
2FA-satisfied if 2FA is on — you cannot bootstrap a weaker session into enrolling
tokens). Reader daemon reachable on localhost.

```
Admin UI  ──POST /api/auth/nfc/enroll/begin {username}──▶ PHP
                                                          │  create enroll ticket
                                                          │  (server nonce, TTL),
                                                          │  bind to admin session
  ◀──────────────── {ticket, expiresAt} ─────────────────┘
Admin taps the NEW card on the reader
Admin UI  ──GET reader: /read?ticket=…──▶ Reader daemon ──▶ returns {uid,type[,crypto]}
Admin UI  ──POST /api/auth/nfc/enroll/complete {ticket, uid, type[, crypto]}──▶ PHP
                                                          │  verify ticket + admin,
                                                          │  reject if UID already
                                                          │  enrolled to anyone,
                                                          │  salt+hash, persist via
                                                          │  solariCtl (or JSON),
                                                          │  audit
  ◀──────────── {cardId, label, mode} ──────────────────┘
```

Rules:
- A given UID may be enrolled to **at most one** user (reject duplicates — else two
  people share a factor).
- Enrolment is an **operator-role** action (`Operator::requireOperator()`), audited
  with `enrolledBy`.
- For Mode B, `enroll/complete` also runs the DESFire key provisioning / records
  the applet public key; `mode` becomes `crypto`.
- The raw UID is used only transiently to compute the hash and is never logged.

### 4.2 Verification (login second step)

Primary auth changes: on successful **primary** verification, if
`nfc2fa.enabled` and the user has ≥1 active card (or `enforce:"all"`), the session
is put in a **`pending_2fa`** state instead of fully authenticated. The gate in
`index.php` treats a `pending_2fa` session as *unauthenticated for everything
except the NFC verify + logout endpoints*.

```
Browser  ──POST /api/auth/login {user,pass}──▶ PHP  (primary OK)
              │  user has active card & nfc2fa on
              │  session.state = pending_2fa
              │  issue challenge nonce (TTL), store on session
  ◀── 200 {stage:"nfc_required", challenge, mode:"uid"|"crypto"} ──┘
Browser shows "Tap your card"
Browser  ──GET reader /read?challenge=…──▶ Reader daemon ──▶ {uid,type[,crypto]}
Browser  ──POST /api/auth/nfc/verify {challenge, uid[, crypto]}──▶ PHP
              │  1. session.state == pending_2fa ?           else 409
              │  2. challenge matches session & not expired  else 401 (replay/expired)
              │  3. consume challenge (single-use)
              │  4. hash presented uid with each active card's salt;
              │     constant-time compare; Mode B: verify challenge-response
              │  5. match & not revoked → elevate: state = authenticated
              │     no match → record failure, maybe lock account
  ◀── 200 {stage:"authenticated"}  |  401 {stage:"nfc_failed"} ────┘
Browser reloads → whoami now returns a full principal
```

OIDC path is identical after the callback: `Oidc::handleCallback()` currently calls
`Auth::establishSession()`. When 2FA is on it instead calls
`Auth::establishPending2fa()` (same principal, `pending_2fa` state) and the SPA is
redirected to the tap prompt. See §6 for the Keycloak-native alternative.

### 4.3 Replay, lockout, session hardening

- **Challenge nonce**: 32 random bytes, base64, stored on the *server session*,
  single-use, `challengeTtlSeconds` TTL. A verify without a matching live challenge
  is rejected. This stops replay of a captured `{uid}` body — the old challenge is
  already consumed/expired.
- **Session fixation**: `session_regenerate_id(true)` on *both* the pending
  transition (already done in login) **and** on elevation to `authenticated`, so a
  pending-2FA session id can't be reused post-elevation.
- **Lockout**: per-account failure counter (`maxFailures` / `lockoutSeconds`).
  During lockout, verify returns a uniform failure regardless of card correctness
  (no oracle). Counter resets on success.
- **Uniform errors**: no distinction between "no such enrolment", "wrong card",
  "revoked" — all `nfc_failed`, to avoid enumeration.
- **Reader↔PHP authenticity (optional, Mode-A hardening)**: the reader daemon may
  HMAC its response with `readerHmacKeyRef` so a rogue localhost process can't
  feed PHP an arbitrary UID. In the tonight scaffold this is off by default and
  documented as a follow-up (the reader and browser are same-origin/localhost).

---

## 5. Reader abstraction (summary; full spec in `reader/README.md`)

A small local daemon `nfc-reader.py` with pluggable backends
(`pcsc` | `libnfc` | `mock`) exposing a **localhost-only HTTP** interface (default
`127.0.0.1:8770`) — chosen over a Unix socket because the *browser* must reach it
directly during enrol/verify, and browsers can't open Unix sockets. CORS is
locked to the dashboard origin; the daemon binds loopback only.

Wire format (JSON):

```
GET /health                      → {ok:true, backend:"mock", version:"0.1"}
GET /read?timeout=15[&challenge=…&ticket=…]
    (blocks up to timeout s for a card, or returns immediately if one is present)
    200 → {
      ok: true,
      uid: "04A1B2C3D4E5F6",        // uppercase hex, no separators
      atr: "3B8F80...",             // PC/SC ATR if available, else null
      type: "NTAG215",              // best-effort card type string
      mode: "uid",                  // "uid" | "crypto"
      crypto: null,                 // Mode B: {challenge, response, alg} envelope
      ts: "2026-07-06T04:00:00Z"
    }
    204 → no card presented within timeout
    503 → {ok:false, error:"no_reader"}  // backend has no hardware
```

`challenge`/`ticket` are opaque pass-throughs the daemon echoes into its response
(and, in Mode B, feeds to the card's authenticate command). The daemon never
talks to the dashboard API or DB — it only reads cards and answers the browser.

Backends:
- **pcsc** — `pyscard`, PC/SC. For the **hydrogen USB reader** (ACR122U-class CCID)
  and any CCID reader on any host. Reads UID via the PC/SC "get data" APDU
  `FF CA 00 00 00`. This is the recommended production path.
- **libnfc** — `nfcpy`/`libnfc` for a bare **PN532** (UART/I2C/SPI). Present for the
  "built-in reader" case *if* one is ever positively identified (it is **not**
  present on xenon — see HARDWARE_TODO).
- **mock** — emits a fixed UID (`04DEADBEEF0102`, type `MOCK`) so the enrol/verify
  flow and UI can be exercised with zero hardware. **This is what runs tonight.**

---

## 6. Hooking Keycloak (OIDC path)

Two viable approaches; SolariNet should prefer the **app-side** one first because
it needs no realm-admin deploy and keeps the factor inside SolariNet's control.

**(a) App-side second factor (recommended first).** Keycloak remains the *primary*
IdP only. After `Oidc::handleCallback()` validates the ID token, SolariNet puts the
session in `pending_2fa` and runs its own NFC verify (§4.2). Pro: no Keycloak SPI,
works for local-and-SSO users uniformly, one code path. Con: the NFC factor is not
visible to *other* Keycloak clients (fine — SolariNet is the only consumer).

**(b) Keycloak-native custom authenticator (later, if NFC must be realm-wide).**
Implement a Keycloak **Authenticator SPI** (Java) that runs as an execution in the
`akoria` browser flow after the password step, plus a **Required Action** for
enrolment. The authenticator would call an internal SolariNet verify service (the
NFC daemon can't be reached from Keycloak directly — the card is at the user's
browser). This means a browser-side JS in Keycloak's login theme talks to the
localhost reader daemon and posts the result to the authenticator. Deliverables if
we go this route:
  - `keycloak/solari-nfc-authenticator/` — Java SPI jar (AuthenticatorFactory +
    Authenticator + a FreeMarker form template that includes the reader-daemon JS).
  - A required-action `nfc-enroll` for first-time binding.
  - Realm flow change: `akoria` browser flow → add "SolariNet NFC" as REQUIRED
    after "Username Password Form".
  - Credential stored as a Keycloak custom credential (`user credential` of type
    `nfc-card`), hashed as in §3.

For tonight we scaffold **(a)** only and document **(b)** as the realm-wide upgrade.

---

## 7. What is scaffolded vs pending

Scaffolded (this delivery):
- `reader/nfc-reader.py` — full daemon, `mock` backend working, `pcsc`/`libnfc`
  backends implemented but require their libs + hardware.
- `dashboard/api/lib/Nfc2fa.php` — config gate, salt+hash, challenge issue/verify,
  lockout, enrolment persistence to the JSON store (MariaDB path stubbed).
- `dashboard/api/routes/nfc2fa.php` — enroll begin/complete, verify, list/revoke.
- Hooks: `login` returns `stage:"nfc_required"` when appropriate; `index.php` gate
  recognises `pending_2fa`. Delivered as **patch snippets** in
  `dashboard/INTEGRATION.md` (non-destructive — the live files are not overwritten).
- `dashboard/public/nfc2fa.jsx` — enrolment UI stub + "tap your card" prompt.

Pending (needs hardware / decisions — see HARDWARE_TODO.md):
- Positive identification of xenon's alleged built-in reader (evidence says there
  is none; the SMBus devices are motherboard housekeeping).
- Real PC/SC read on hydrogen (macOS) end-to-end.
- Mode B (DESFire challenge-response) — structured for but not implemented.
- MariaDB `nfcCredential` writes via `solariCtl`.
- Keycloak-native authenticator (approach (b)).
- Reader↔PHP HMAC authenticity.
