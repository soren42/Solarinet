# SolariNet Authenticator (Tab5) — Design notes

Security model for on-device secrets, the push-approval signing scheme, the BLE
autotype path, and the transport decisions.

---

## 1. On-device secret storage

Three classes of secret live on the Tab5:

- **TOTP seeds** — RFC 6238 Base32 shared secrets. A leaked seed mints codes
  forever.
- **Push-approval device key** — a P-256 private scalar that signs approval
  responses. A leaked key lets an attacker approve logins. **Highest value.**
- **Password-manager key material** — enough to unlock the vault (a KDF-derived
  protected key + cached tokens).

### 1.1 Storage layout

Everything is in **NVS**, split across two partitions (`partitions.csv`):

| Partition | Namespace | Contents |
|-----------|-----------|----------|
| `nvs` (default) | `cfg` | non-secret: SSID, broker host, device id, PM URL, TZ, KDF params |
| `secrets` (dedicated) | `totp`, `pm`, `dev` | TOTP seeds, PM key blob, **device signing key + pubkey**, Wi-Fi PSK |

A **dedicated `secrets` partition** lets us **panic-wipe just the secrets**
(`Config::panicWipeSecrets` — now also clears `dev`) without erasing app/settings,
and isolates secret access behind its own NVS handles.

### 1.2 Encryption at rest — ESP32-P4 flash encryption

The protection that matters for a physical desk device is **flash encryption**:
the P4 flash controller transparently AES-encrypts flash with a key in **eFuse**
the CPU cannot read out. Dumping the SPI flash then yields ciphertext — TOTP
seeds, PM keys, and the **device signing key** included. NVS encryption rides on
this. **Secure Boot** additionally stops flashing modified firmware that would
dump decrypted secrets at runtime — recommended for a release authenticator; the
eFuse burn is effectively one-way.

**Rollout policy (decide before release):** development-mode encryption allows
re-flashing (good for iterating); release-mode disables the serial re-flash path.
Burn release eFuses only on a finished unit; a mis-burn bricks re-flashing.

### 1.3 In-RAM hygiene

- The push-approval **private scalar** is loaded only to sign, then the heap copy
  is zeroized (`approvals.cpp` wipes the `priv` buffer after each sign/keygen).
- The PM **master password is never persisted**; the master key is stretched,
  used to unwrap the user key, then zeroized. `Vault::lock()` clears derived keys
  and caches.
- TOTP `compute()` wipes decoded key + HMAC buffers off the stack.
- Generated passwords: `pwgen` overwrites its working buffer, and the "last
  generated" value is cleared on `clear()` / idle (see UI auto-lock TODO).
- The on-screen clipboard auto-clears (default 20 s).

### 1.4 Threat model boundaries (honest scope)

- **Covered:** flash read-out at rest (encryption); approval forgery (server
  holds only the public key; responses are ECDSA-signed and nonce-bound);
  replay (per-request nonce + TTL + single-use ids); vault ciphertext tamper
  (AES-CBC + HMAC verify before decrypt); wrong-password rejection offline.
- **Not covered by the device alone:** a live attacker with the *unlocked*
  device in hand (approving on their behalf) — mitigated only by physical
  possession + optional device unlock; sophisticated eFuse glitching; a
  malicious MQTT/PM server if cert verification is left off (hence the
  `setCACert` TODOs — pin the private CA, never ship `setInsecure()`).

---

## 2. Push-approval — flow + signing scheme

### 2.1 Flow

```
dashboard (or Keycloak)          authbrokerd                 Tab5
   |  primary auth OK               |                          |
   |-- POST /auth/request --------->|                          |
   |                                |-- MQTT auth/request/<id>->|  (Approve/Deny + TTL)
   |                                |                          |-- operator taps
   |                                |<- MQTT auth/response/<id>-|  (SIGNED)
   |                                |  verify sig+nonce+ttl     |
   |<------ {decision} -------------|                          |
   |  establish session iff approve |                          |
```

### 2.2 Why this shape

- **Asymmetric, not shared-secret.** The device signs with a P-256 **private**
  key; the broker verifies with the **public** key only. A broker or dashboard
  compromise cannot mint approvals — the possession factor is genuinely bound to
  the Tab5. (A shared HMAC secret would put a forgeable credential on the server.)
- **MQTT, not a new push channel.** SolariNet already has Mosquitto; the device
  already speaks MQTT for notifications. Approvals reuse the **same shared bus**
  (`mqttbus.*`) — one TLS connection, two topic families. No per-device HTTP/WS
  push service to run.
- **Broker mediates, device stays simple.** The dashboard makes one blocking
  HTTP call; the broker owns request state, nonce, TTL, and verification. The
  device just renders a prompt and signs.

### 2.3 Signing details

- Curve **secp256r1 (P-256)**, **ECDSA / SHA-256**, DER signature, base64 in
  `sig`. P-256 ECDSA is present in the IDF's bundled mbedTLS (no extra
  component), and Python `cryptography` verifies it in three lines — hence chosen
  over Ed25519 (whose on-device mbedTLS support is not guaranteed on the pinned
  core).
- **Canonical signed bytes** (both sides build identically, UTF-8):
  `"<v>\n<id>\n<decision>\n<nonce>\n<device_id>"`. Fixed field order, newline
  delimited, no JSON-canonicalization ambiguity.
- **Anti-replay:** the broker generates a 32-byte random `nonce` per request; the
  device echoes it inside the signed bytes. The broker checks the nonce matches
  the outstanding request, the id is still pending (single-use), and the response
  arrived within TTL. Any mismatch ⇒ ignored ⇒ login times out (fails closed).
- **Key custody:** generated on-device (`mbedtls_ecp_gen_keypair` seeded from the
  hardware TRNG via CTR-DRBG), stored as the 32-byte scalar in `secrets` NVS
  (`dev/privkey`). Only the public point is exported for enrollment.

The server half, the exact contract, and the live-test evidence are in
`deploy/authbroker/README.md`.

---

## 3. Password generator — randomness

`pwgen.*` draws from **`esp_random`**, the ESP32-P4 hardware TRNG (also feeds the
mbedTLS entropy pool used for signing). Indices are drawn by **rejection
sampling** (`uniform(n)` discards the biased tail of the 32-bit draw) so every
character and every shuffle swap is unbiased — no modulo bias, never `rand()`.
The policy guarantees ≥1 character per enabled class, then a Fisher-Yates
shuffle removes positional bias from those guaranteed characters.

---

## 4. BLE on the co-processor (autotype)

The Tab5's ESP32-P4 has **no native radio**. Wi-Fi *and* BLE are provided by the
on-board **ESP32-C6** over the **esp-hosted** link (SDIO/UART). Consequences for
BLE HID:

- BLE runs on the **C6**; the P4 drives it as an HCI host over the hosted
  **VHCI** transport. Standard NimBLE-Arduino then works from P4 application code
  — *if* the controller is exposed.
- This requires the **C6 hosted firmware to include the Bluetooth controller /
  HCI**. Many esp-hosted builds are **Wi-Fi-only** and expose no BT — on such a
  build BLE HID cannot work until the C6 is reflashed with a hosted build that
  includes BT. This is the honest wrinkle: `blehid.cpp` is guarded by
  `SOLARI_HAS_BLE` (default 0) and `begin()` reports `Unavailable` unless the
  controller comes up, so the app degrades gracefully (the "Type" actions
  disable) rather than failing to boot.
- Build side: enable NimBLE-Arduino (`platformio.ini` lib, commented until
  ready) and the IDF `CONFIG_BT_ENABLED` / `CONFIG_BT_NIMBLE_ENABLED` +
  esp-hosted BT transport. `TODO(hosted-bt)` / `TODO(nimble)` mark the exact
  spots.
- **Keymap:** US-QWERTY. `asciiToHid()` maps printable ASCII to HID usage codes
  and applies Left-Shift (0x02) for uppercase and shifted symbols; unmappable
  characters are skipped rather than mistyped. Reports are the 8-byte boot
  keyboard format with a press/release pair and a small pacing delay.

---

## 5. MQTT transport decision (recap)

**Chosen: MQTT over TLS**, one **shared** client (`mqttbus.*`) for both
push-approval and background notifications. MQTT is the de-facto ESP messaging
protocol (`PubSubClient`, a few KB, native pub/sub + auto-reconnect); AMQP on ESP
is a poor fit and HTTP/WS push would mean a new stateful per-device service.
Notifications reuse SolariNet's pluggable notifyd sender (`server-shim/mqtt.py`);
approvals reuse the same broker with a distinct topic family. The only new infra
is Mosquitto on benzene (already assumed) + the `authbrokerd` bridge. Moving to
`:8883` TLS with the self-hosted CA is the documented follow-up.
