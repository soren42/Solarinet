# SolariNet Companion — M5Stack Tab5 firmware

A personal security/ops companion running on an **M5Stack Tab5** (ESP32-P4, 5"
1280×720 capacitive touchscreen; ESP32-C6 co-processor for Wi-Fi). Three
features on one always-on desk device:

1. **TOTP generator** (RFC 6238) — live 6-digit codes with countdown rings,
   seeds stored in encrypted flash, add via QR scan or Base32 entry.
2. **Password-manager client** — Vaultwarden/Bitwarden-compatible, unlock with
   master password/PIN, searchable vault, copy/show fields.
3. **SolariNet notification surface** — subscribes to the SolariNet alert
   stream over MQTT and shows a scrollable feed + toasts for crit/warn,
   complementing the SMS path.

> **Status: scaffold.** This is a buildable, well-structured project skeleton.
> It is **not** flash-ready tonight — it needs a USB-C build host with the
> ESP32-P4 toolchain, and several `TODO(...)` markers (crypto finishing, HTTP
> client, UI entry sheets, camera) must be closed before real secrets go on it.
> Search the tree for `TODO(` to find every open item.

---

## Architecture

```
                       ┌─────────────────────────── Tab5 (ESP32-P4) ──────────────────────────┐
                       │  main.cpp  ── boot, Wi-Fi (via C6), SNTP, event loop                  │
                       │     │                                                                 │
                       │  ui.*  ── screen state machine (home 3 tiles + per-feature screens)   │
                       │   ┌────────────┬───────────────────────┬──────────────────────────┐  │
                       │  totp.*        vault.*                  notify.*                    │  │
                       │  RFC6238       Bitwarden API + crypto   MQTT/TLS subscriber         │  │
                       │  mbedTLS HMAC  mbedTLS PBKDF2/AES/HMAC   PubSubClient               │  │
                       │   │            │                        │                           │  │
                       │  config.*  ── NVS: non-secret (cfg) + encrypted secrets partition   │  │
                       └───┼────────────┼────────────────────────┼───────────────────────────┘
                           │            │                        │
                    RTC RX8130   Vaultwarden/Bitwarden     Mosquitto broker (MQTT/TLS)
                    + SNTP       https://vault.akoria.net       ▲ notify/#
                                                                │
                                             ┌──────────────────┴───────────────────┐
                                             │ notifyd + senders/mqtt.py (the shim)  │
                                             │ consumes RabbitMQ notify.events,      │
                                             │ republishes JSON to MQTT notify/<sev> │
                                             └───────────────────────────────────────┘
```

### Files

| Path | Role |
|------|------|
| `platformio.ini` | Tab5/ESP32-P4 env, libs, flash + partition config |
| `partitions.csv` | 16 MB layout incl. dedicated encrypted `secrets` NVS |
| `src/main.cpp` | boot order + main loop |
| `src/config.*` | settings + secret store (NVS), TOTP seeds, PM key blob |
| `src/totp.*` | RFC 6238 codes (mbedTLS HMAC), Base32, RTC/SNTP time sync |
| `src/vault.*` | Vaultwarden/Bitwarden client + crypto (PBKDF2/Argon2, AES-CBC-HMAC) |
| `src/notify.*` | MQTT/TLS subscriber, feed ring, toast hook |
| `src/ui.*` | M5GFX rendering, three-tile home, per-feature screens |
| `server-shim/mqtt.py` | the notifyd sender plugin that feeds the Tab5 (see below) |

---

## The three features

### 1. TOTP generator
- Seeds are RFC 6238 shared secrets (Base32), stored in the **encrypted
  `secrets` NVS partition** (see DESIGN.md). Never compiled in.
- Codes computed on-device with mbedTLS HMAC-SHA1/256/512; `totp.cpp` also
  carries its own Base32 decoder (no external lib).
- **Time**: the Tab5's **RX8130 RTC** is seeded from system time and kept
  disciplined by **SNTP** once Wi-Fi is up (`totp::beginTimeSync`). At boot we
  seed the system clock *from* the RTC so codes work within seconds, before the
  network. RTC holds UTC; the POSIX TZ handles local display.
- **Add-seed**: QR scan (camera — see open question) parses `otpauth://` URIs;
  manual entry takes issuer/label + Base32. Both paths call
  `Config::addTotpAccount`. *(Entry sheets are TODO-marked in `ui.cpp`.)*

### 2. Password-manager client
- Targets a self-hosted **Vaultwarden / Bitwarden-compatible** server; base URL
  is fully configurable (`Settings::pmBaseUrl`). **Open question: confirm which
  PM the user runs** (see below).
- **Unlock**: master password or PIN → KDF (PBKDF2-SHA256 or Argon2id, per the
  server's account params) → stretch → unwrap the protected symmetric key.
  Wrong password fails the HMAC check on the key unwrap, so no server round-trip
  is needed to reject it. Supports an **offline unlock** against a cached,
  still-encrypted vault when Wi-Fi is down.
- **Vault list**: names/usernames/URIs decrypted for search; passwords and
  item-TOTPs decrypted **on demand** to minimize plaintext dwell.
- **Copy**: the Tab5 has no OS clipboard, so "copy" holds the value in an
  on-screen buffer with a **20 s auto-clear** timer.
- Crypto uses **mbedTLS** (bundled in the IDF) — see `vault.cpp` and DESIGN.md.
  The Argon2id path and the HTTP client are `TODO`-marked.

### 3. SolariNet notification surface
- Subscribes to **MQTT** topic `notify/#` over TLS. Payload is the exact JSON
  contract `notifyd` already uses (`{ts, severity, source, title, body, ...}`).
- Scrollable newest-first feed (bounded ring, 200 entries) + **toast for
  crit/warn**; info accumulates silently. Complements — does not replace — the
  SMS path (both are just senders on the same `notify.events` stream).

---

## Server-side shim (notifications)

The Tab5 does **not** speak AMQP. Instead, a tiny notifyd **sender plugin**
(`server-shim/mqtt.py`) republishes each `notify.events` message to an MQTT
broker; the Tab5 subscribes there. This reuses SolariNet's existing pluggable
sender architecture (`deploy/notify/senders/`) — no changes to `notifyd.py`.

**One new infra piece**: a Mosquitto broker (can run on benzene next to
RabbitMQ), with two users (`notifyd` publish, `tab5` subscribe) and TLS.

Install (on the notifyd host):
1. `cp firmware/tab5/server-shim/mqtt.py deploy/notify/senders/mqtt.py`
2. Add `"mqtt": "senders.mqtt",` to the `REGISTRY` in
   `deploy/notify/senders/__init__.py`.
3. In `notify.conf`: add `mqtt` to `[senders] enabled`, to the `[routing]`
   severities you want mirrored, and a `[sender.mqtt]` section (host/port/creds/
   TLS/`topic_prefix`). Full example is in the shim's docstring.
4. `pip install paho-mqtt` into notifyd's venv.
5. Stand up Mosquitto + the two users + broker CA.

Rationale for MQTT over AMQP/HTTP-poll/WebSocket is in **DESIGN.md**.

---

## Flashing steps (USB-C)

> Requires a build host with USB-C to the Tab5 and the ESP32-P4 toolchain.
> None of this can be done from the SolariNet server remotely — the device must
> be cabled to the machine running PlatformIO.

1. **Install PlatformIO** (`pip install platformio` or the VS Code extension).
2. **Cable** the Tab5 to the host with **USB-C**. The ESP32-P4 exposes a native
   USB-Serial/JTAG, so it enumerates as `/dev/ttyACM*` (Linux) / `COMx` — no
   external UART bridge needed. If the board doesn't appear, hold **BOOT** while
   tapping **RESET** to force download mode.
3. **Resolve the board id.** `platformio.ini` uses `board = m5stack-tab5` on the
   `pioarduino` platform fork. On the build host run:
   ```
   pio pkg install
   pio boards | grep -i -E 'tab5|esp32-?p4'
   ```
   and set `board` to whatever P4/Tab5 def is actually present (fallback:
   `esp32-p4-evboard` with explicit `board_build.*`). **TODO** in the ini.
4. **Build**: `pio run -e tab5`
5. **Flash**: `pio run -e tab5 -t upload` (esptool under the hood). If port
   autodetect fails, set `upload_port` in `platformio.ini`.
6. **Filesystem** (LittleFS, for the vault cache): `pio run -e tab5 -t uploadfs`
7. **Monitor**: `pio device monitor -b 115200`

**Flash encryption** (protects secrets at rest) is enabled as a *second* step
after first boot works — see DESIGN.md. Enable it deliberately; on real
hardware it is typically a one-way eFuse burn.

## Wi-Fi / secret provisioning

Nothing secret is compiled in. On first boot the device is *unprovisioned* and
shows the **setup wizard** (Wi-Fi join, MQTT host+creds, PM base URL+email,
master password) — wizard steps are `TODO`-marked in `ui.cpp`. Values persist
to NVS: non-secret in the `cfg` namespace, secrets in the encrypted `secrets`
partition. For dev you may add a gitignored `src/secrets.local.h` guarded with
`__has_include` — **never commit it**.

Assumed environment: Tab5 at **10.5.74.57** (DHCP) on the **akoria** Wi-Fi;
`akoria.net`/`akoria.org` resolve; SolariNet dashboard at `https://xenon:9443`.

---

## Open questions / hardware TODO

- **Which password manager?** Design assumes a Bitwarden-API-compatible server
  (Vaultwarden most likely). **Confirm the actual PM + base URL** before
  finishing `vault.cpp`. A non-Bitwarden PM would need a different client.
- **Does this Tab5 SKU have the camera?** The QR add-seed path needs the
  MIPI-CSI camera (SC2356). `SOLARI_HAS_CAMERA=0` by default — flip it on and
  finish the camera/QR path only if the camera is populated. Manual Base32
  entry always works as a fallback.
- **Board id / platform versions** — re-resolve on the build host (step 3).
- **Broker + TLS CA** — stand up Mosquitto and provision the broker CA on the
  device (`notify.cpp` `setCACert` TODO).
- **KDF: Argon2id** — mbedTLS lacks Argon2; needs the IDF PSA crypto or a small
  argon2 component (`vault.cpp` TODO). PBKDF2 accounts work without it.
- **RTC accuracy across long offline spells** — the RX8130 is good, but confirm
  drift is acceptable for TOTP if the device is off-network for days.
- **Flash-encryption rollout** — decide dev vs. release eFuse policy (DESIGN.md).
