# SolariNet Authenticator — M5Stack Tab5 firmware

A **dedicated authentication & authorization device** running on an **M5Stack
Tab5** (ESP32-P4, 5" 1280×720 capacitive touchscreen; ESP32-C6 co-processor for
Wi-Fi/BLE). This is the single-purpose "SolariNet Authenticator": it replaces
the earlier companion role (network monitoring has moved to another device).

Five features, one always-on desk device:

1. **TOTP generator** (RFC 6238) — live 6-digit codes with countdown rings,
   seeds in encrypted flash, RTC + SNTP time.
2. **Push-approval** (2FA / access approval) — when someone authenticates to the
   SolariNet dashboard or Keycloak, an **Approve / Deny** prompt appears here
   with a short TTL. Responses are **cryptographically signed** by a device key;
   the server verifies. This is the core new mission.
3. **Vault** — a **client to a self-hosted password manager** the user runs
   (Bitwarden/Vaultwarden-compatible; type + URL configurable). The Tab5 is *not*
   a password manager, it holds a PM client.
4. **Secure password generator** — strong passwords from the hardware CSPRNG,
   configurable length + character classes; feeds the vault and BLE autotype.
5. **BLE-keyboard autotype** — the device acts as a Bluetooth HID keyboard and
   **types** a password or a live TOTP code into the paired host, so the secret
   never touches an OS clipboard.

> **Status: scaffold.** Buildable, well-structured project skeleton. It is **not**
> flash-ready tonight — it needs a USB-C build host with the ESP32-P4 toolchain,
> and several `TODO(...)` markers (device-key enrollment UI, PM HTTP client, UI
> entry sheets, NimBLE wiring) must be closed before real secrets go on it.
> Search the tree for `TODO(` to find every open item. The **server-side broker**
> (`deploy/authbroker/`), by contrast, is runnable and live-tested today.

---

## Architecture

```
        ┌──────────────────────── Tab5 (ESP32-P4) ─────────────────────────┐
        │  main.cpp  ── boot, Wi-Fi (via C6), SNTP, event loop              │
        │     │                                                             │
        │  ui.*  ── home: 3 tiles (TOTP/Approvals/Vault) + generator + BLE  │
        │  ┌────────┬───────────┬─────────┬──────────┬───────────────────┐ │
        │ totp.*   approvals.*  vault.*   pwgen.*    blehid.*             │ │
        │ RFC6238  ECDSA-P256   PM client CSPRNG     BLE HID keyboard     │ │
        │          sign resp.   (mbedTLS)            (NimBLE via C6)      │ │
        │  └───┬────────┬──────────┬────────────────────┬───────────────┘ │
        │  config.*     mqttbus.*  ── one shared MQTT/TLS connection       │
        │  (NVS: cfg +  │                                                  │
        │   secrets:    │  approvals + notify ride the same bus            │
        │   totp/pm/dev)│                                                  │
        └───────────────┼──────────────────────────────────────────────────┘
                        │ MQTT
              ┌─────────┴──────────┐
              │ Mosquitto (benzene)│  auth/request/#  auth/response/#  notify/#
              └─────────┬──────────┘
        ┌───────────────┴───────────────┐        ┌───────────────────────────┐
        │ authbrokerd (deploy/authbroker)│        │ notifyd mqtt sender (shim)│
        │ dashboard/Keycloak ⇄ approvals │        │ RabbitMQ → notify/<sev>   │
        └────────────────────────────────┘        └───────────────────────────┘
```

### Files

| Path | Role |
|------|------|
| `platformio.ini` | Tab5/ESP32-P4 env, libs, flash + partition config, feature flags |
| `partitions.csv` | 16 MB layout incl. dedicated encrypted `secrets` NVS |
| `src/main.cpp` | boot order + main loop |
| `src/config.*` | settings + secret store (NVS): TOTP seeds, PM key, **device key** |
| `src/mqttbus.*` | **shared** MQTT/TLS transport (approvals + notifications) |
| `src/totp.*` | RFC 6238 codes (mbedTLS HMAC), Base32, RTC/SNTP |
| `src/approvals.*` | **push-approval**: subscribe `auth/request/#`, sign + publish response |
| `src/vault.*` | PM client (Bitwarden/Vaultwarden API) + crypto |
| `src/pwgen.*` | **secure password generator** (esp_random CSPRNG) |
| `src/blehid.*` | **BLE HID keyboard** autotype (US layout) |
| `src/notify.*` | background crit/warn toasts (secondary; rides the bus) |
| `src/ui.*` | M5GFX rendering, home tiles + per-feature screens |
| `server-shim/mqtt.py` | notifyd sender that feeds the background notification toasts |

---

## The features

### 1. TOTP generator
Unchanged from the earlier scaffold: seeds in the encrypted `secrets` NVS,
codes via mbedTLS HMAC-SHA1/256/512, own Base32 decoder. Time from the RX8130
RTC, disciplined by SNTP; the system clock is seeded from the RTC at boot so
codes work within seconds, pre-network. A per-code **"Type"** action can send
the code over BLE-HID (feature 5).

### 2. Push-approval (the core mission)
- Subscribes to `auth/request/#` on the shared MQTT bus. Each request carries
  `{action, subject, detail, source_ip, nonce, ttl}` (see the broker README for
  the full contract).
- Renders a full-screen **Approve / Deny** prompt with a TTL countdown ring.
- On the operator's tap, publishes a **signed** response to `auth/response/<id>`:
  ECDSA-P256/SHA-256 over the canonical string
  `"<v>\n<id>\n<decision>\n<nonce>\n<device_id>"`, DER, base64.
- The **device private key** (P-256 scalar) lives in the encrypted `secrets` NVS
  (`dev/privkey`). The broker holds only the matching **public** key, so it can
  verify but never forge. Enroll the public key into the broker once (below).
- Anti-replay: the server nonce is echoed + signed; the device de-dupes by id
  and expires stale requests after their TTL.

### 3. Vault (PM client)
- A **client** to a self-hosted, Bitwarden-API-compatible password manager
  (Vaultwarden most likely). `pmType` selects the protocol; `pmBaseUrl` is
  configurable. **Confirm-later**: which PM the user runs + its URL.
- Bitwarden unlock/crypto chain (KDF → stretch → unwrap user key → AES-CBC+HMAC
  decrypt) is laid out in `vault.cpp` with the crypto primitives; HTTP client +
  Argon2 are `TODO`-marked. Reveal-on-demand, on-screen clipboard with
  auto-clear. Can also **type** a credential over BLE (feature 5) or receive a
  freshly **generated** password (feature 4).

### 4. Secure password generator
- `pwgen.*`. Uses **`esp_random`** (ESP32-P4 hardware TRNG) via unbiased
  rejection sampling — never `rand()`.
- Configurable **length** (8–64) and class toggles **A-Z / a-z / 0-9 / symbols**,
  plus **avoid ambiguous characters**. Guarantees ≥1 char from every enabled
  class, then Fisher-Yates shuffles so guaranteed chars aren't front-loaded.
- Screen shows the value with **Regenerate**, **Type it** (BLE), **Save to
  Vault** (vault write path is `TODO`).

### 5. BLE-keyboard autotype
- `blehid.*`. The device advertises as a BLE HID keyboard and types a selected
  password / live TOTP straight into the paired host.
- **US-QWERTY** ASCII→HID keymap (with Shift for uppercase + shifted symbols) is
  complete in `blehid.cpp`.
- **Hardware wrinkle (honest):** the Tab5's ESP32-P4 has no native radio — BLE
  runs on the **ESP32-C6 co-processor** over the esp-hosted link. This needs C6
  **hosted firmware with the BT/HCI controller enabled** (a Wi-Fi-only hosted
  build will *not* expose BLE) plus NimBLE-Arduino. Gated behind
  `SOLARI_HAS_BLE` (default 0): when off/unavailable, the "Type" actions are
  disabled and the rest of the app is unaffected. See DESIGN.md
  "BLE on the co-processor".

---

## Server-side broker (push-approval)

`deploy/authbroker/` — a small, **runnable** Python service (`authbrokerd.py`)
that bridges dashboard/Keycloak auth to this device over MQTT and verifies the
signed decision. It ships with a config example, systemd unit, an enrollment
helper, a **live loopback test**, and additive dashboard wiring
(`dashboard/AuthBroker.php` + `INTEGRATION.md`). See `deploy/authbroker/README.md`
for the message contract, signing scheme, and setup.

## Server-side shim (background notifications)

`server-shim/mqtt.py` is the notifyd sender that republishes SolariNet alerts to
`notify/<severity>`; the Tab5 subscribes for background crit/warn **toasts**
(there is no Alerts tile on the Authenticator — notifications are secondary).
Install per the docstring; unchanged from before.

---

## Flashing steps (USB-C)

> Requires a build host cabled to the Tab5 with USB-C and the ESP32-P4
> toolchain. Cannot be done remotely from the SolariNet server.

1. **Install PlatformIO** (`pip install platformio` or the VS Code extension).
2. **Cable** the Tab5 (native USB-Serial/JTAG → `/dev/ttyACM*` / `COMx`). If it
   doesn't enumerate, hold **BOOT** + tap **RESET** for download mode.
3. **Resolve the board id**: `pio pkg install`; `pio boards | grep -iE 'tab5|esp32-?p4'`
   and set `board` accordingly (`platformio.ini` TODO).
4. **Build**: `pio run -e tab5`
5. **Flash**: `pio run -e tab5 -t upload`
6. **Filesystem** (LittleFS, for the vault cache): `pio run -e tab5 -t uploadfs`
7. **Monitor**: `pio device monitor -b 115200`

**Flash encryption** (protects secrets, incl. the device signing key, at rest)
is a deliberate second step after first boot — see DESIGN.md. On real hardware
the release-mode eFuse burn is effectively one-way.

## Device-key enrollment (push-approval)

The signing key is generated **on-device** and never leaves it:

1. First boot mints a P-256 keypair into `secrets` NVS (`Approvals::begin` →
   `provisionDeviceKey`). The setup screen shows the **public key** (hex).
2. Generate/register the matching entry on the broker. Easiest path tonight:
   run `deploy/authbroker/enroll_device.py tab5-desk` to mint a pair, put the
   **private scalar** on the device (NVS `dev/privkey`) and the **public** block
   in `authbroker.conf`. Alternatively read the device-shown pubkey and convert
   it to a `[device.<id>]` PEM. (`TODO(enroll-ui)`: a one-tap "show/QR my pubkey"
   + on-device import to finalize this.)
3. `device_id` in settings must match the broker's `[device.<id>]` section
   (defaults to `tab5-<efuse-mac>`).

## Wi-Fi / secret provisioning

Nothing secret is compiled in. First boot is *unprovisioned* → **setup wizard**
(Wi-Fi join, MQTT broker host+creds, device id, PM base URL/email) — wizard
steps are `TODO`-marked in `ui.cpp`. Non-secret settings persist to the `cfg`
NVS namespace; secrets (TOTP seeds, PM key, **device key**) to the encrypted
`secrets` partition. For dev you may add a gitignored `src/secrets.local.h`
guarded with `__has_include` — **never commit it**.

Assumed environment: Mosquitto on benzene `10.5.2.50:1883` (user `solari`;
`:8883` TLS a follow-up); Keycloak realm `akoria` at `sso.akoria.org:8443`;
dashboard at `https://xenon:9443`; `akoria` names resolve.

---

## Wiring the dashboard login to require approval

See `deploy/authbroker/dashboard/INTEGRATION.md`. In short: drop
`AuthBroker.php` into `dashboard/api/lib/`, add an `authbroker` block to
`solari-auth.json`, and interpose `AuthBroker::requireApproval()` between
credential verification and session establishment in both `POST /api/auth/login`
and the OIDC callback. It **fails closed** and is inert unless enabled.

## Open questions / hardware TODO

- **BLE controller on the C6** — confirm this unit's hosted firmware exposes a
  BT/HCI controller; if Wi-Fi-only, reflash the C6 before autotype works. Then
  set `SOLARI_HAS_BLE=1` and enable NimBLE in `platformio.ini`.
- **Which password manager?** Confirm the PM + base URL before finishing
  `vault.cpp` (`pmType`/`pmBaseUrl`).
- **Board id / platform versions** — re-resolve on the build host.
- **Broker + TLS CA** — move MQTT to `:8883` and provision the broker CA on the
  device (`mqttbus.cpp` `setCACert` TODO).
- **KDF: Argon2id** — mbedTLS lacks Argon2; needs IDF PSA crypto or an argon2
  component (`vault.cpp` TODO). PBKDF2 accounts work without it.
- **Camera** — optional QR add-seed path; `SOLARI_HAS_CAMERA=0` by default.
- **Flash-encryption / secure-boot rollout** — dev vs. release eFuse policy
  (DESIGN.md). The device signing key makes this especially worth enabling.
