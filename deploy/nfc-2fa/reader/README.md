# SolariNet NFC reader daemon

Loopback-only HTTP daemon that returns the UID of the card presented to a reader.
The dashboard browser calls it directly during enrolment and 2FA verify. It talks
to **no** SolariNet API or DB — it only reads cards.

## Run

```bash
# tonight, zero hardware:
python3 nfc-reader.py --backend mock --origin https://xenon:9443

# hydrogen USB reader (macOS, PC/SC):
python3 nfc-reader.py --backend pcsc --origin https://xenon:9443

# a positively-identified PN532 (NOT xenon — see HARDWARE_TODO.md):
python3 nfc-reader.py --backend libnfc --libnfc-path 'tty:USB0:pn532'

# smoke test a backend without serving:
python3 nfc-reader.py --backend mock --selftest
```

Defaults: binds `127.0.0.1:8770`. Never bind a non-loopback address — the daemon
performs no authentication of its own.

## Backends

| backend | needs | for |
|---------|-------|-----|
| `mock`   | nothing (stdlib only) | testing the whole flow tonight |
| `pcsc`   | `pip install pyscard` + PC/SC (`pcscd` on Linux, built-in on macOS) | hydrogen USB CCID reader (ACR122U-class) |
| `libnfc` | `pip install nfcpy` | a bare PN532 over UART/I2C/SPI |

## Wire format

```
GET /health
  200 {"ok":true,"backend":"mock","version":"0.1"}

GET /read?timeout=15[&challenge=<opaque>&ticket=<opaque>]
  200 {"ok":true,"uid":"04A1B2C3D4E5F6","atr":"3B8F80..."|null,
       "type":"NTAG215","mode":"uid","crypto":null,
       "ts":"2026-07-06T04:00:00Z","challenge":"…","ticket":"…"}
  204  (empty)                       # no card presented within timeout
  503 {"ok":false,"error":"no_reader"}   # backend has no hardware
  500 {"ok":false,"error":"read_error"}

OPTIONS /read  -> 204 (CORS preflight; origin locked to --origin)
```

- `uid`: uppercase hex, no separators.
- `challenge` / `ticket`: opaque values the *server* issued; the daemon echoes
  them back so the browser can bind the read to its pending login/enrol
  transaction. In Mode B (DESFire), the daemon also feeds `challenge` to the
  card's authenticate command and returns the result in `crypto`.
- `mode`: `"uid"` today; `"crypto"` once a DESFire/applet backend is added.

## Security notes

- Loopback bind + CORS to the dashboard origin only.
- The daemon holds no secrets and cannot by itself grant a login — a read is only
  meaningful when paired with the browser's authenticated session and the
  server-issued, single-use challenge (see `../DESIGN.md` §4.3).
- Optional hardening (follow-up): HMAC the response with a key shared with PHP so
  a rogue localhost process can't feed an arbitrary UID (`readerHmacKeyRef`).
