# SolariNet NFC-card two-factor authentication

Design + scaffold for an NFC card as a **second factor** on top of SolariNet's
existing primary auth (local bcrypt + Keycloak OIDC). Config-gated and inert until
`nfc2fa.enabled` is set — with it off, login behaves exactly as today.

Status (2026-07-06): **design + working scaffold, mock reader only.** No NFC reader
is identifiable on xenon tonight (it has none — see `HARDWARE_TODO.md`); the live
path is the hydrogen USB PC/SC reader.

## Layout

```
DESIGN.md            Threat model, 2FA model, enrol/verify flows, Keycloak options
HARDWARE_TODO.md     Exactly what makes it live + what I observed probing xenon
config/              Example solari-auth.json fragments (nfc2fa + per-user cards)
reader/
  nfc-reader.py      Loopback reader daemon: mock | pcsc | libnfc backends
  README.md          Wire format + run instructions
dashboard/
  INTEGRATION.md     The additive patches to wire it into the live dashboard
  api/lib/Nfc2fa.php     Config gate, salt+hash, challenge/verify, lockout, enrol
  api/routes/nfc2fa.php  enroll begin/complete, verify, list/revoke
  public/nfc2fa.jsx      <TapPrompt> (2FA step) + <EnrollCard> (admin) stubs
systemd/
  solari-nfc-reader.service
```

## Try it now (no hardware)

```bash
# terminal 1 — the reader daemon with a fake card
python3 reader/nfc-reader.py --backend mock --origin https://xenon:9443

# terminal 2 — see the wire format
curl -s localhost:8770/health
curl -s "localhost:8770/read?challenge=test&timeout=2"
```

## Chosen 2FA model (short version)

The card UID is **not a secret** (cloneable), so NFC is treated as a *possession*
factor bound to an **enrolled, revocable** card. Two modes coexist:

- **Mode A (UID-as-identifier)** — ships now, works with any card. UID stored
  salted+hashed; a server-issued **single-use challenge** stops replay; per-account
  lockout. Strictly better than password-only; residual risk = a skimmed+cloned UID.
- **Mode B (challenge-response)** — preferred, needs DESFire/applet cards; defeats
  cloning. Structured for (store, reader, verify all branch on `mode`) but not yet
  implemented.

See `DESIGN.md` for the full threat model, flows, and the Keycloak integration
options (app-side second factor now; Java Authenticator SPI later).
