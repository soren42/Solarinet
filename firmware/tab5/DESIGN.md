# SolariNet Companion (Tab5) — Design notes

Security model for on-device secrets, and the notification-transport decision.

---

## 1. On-device secret storage

Two classes of secret live on the Tab5:

- **TOTP seeds** — RFC 6238 Base32 shared secrets. Long-lived, high value: a
  leaked seed lets an attacker mint codes forever.
- **Password-manager key material** — enough to unlock the vault (a KDF-derived
  protected key + cached tokens). Also high value.

### 1.1 Storage layout

Everything is in **NVS**, split across two partitions (`partitions.csv`):

| Partition | Namespace | Contents |
|-----------|-----------|----------|
| `nvs` (default) | `cfg` | non-secret settings: SSID, server URLs, TZ, KDF params |
| `secrets` (dedicated) | `totp`, `pm` | TOTP seeds, PM key blob, Wi-Fi PSK |

A **dedicated `secrets` partition** exists so we can **panic-wipe just the
secrets** (`Config::panicWipeSecrets`) without erasing app or settings, and so
secret access is isolated behind its own NVS handles.

### 1.2 Encryption at rest — ESP32-P4 flash encryption

The protection that matters for a physical desk device is **flash
encryption**: the ESP32-P4 flash controller transparently AES-encrypts flash
contents with a key held in **eFuse** that the CPU cannot read out and that
does not leave the chip. With it enabled, dumping the SPI flash (chip-off or
via the bootloader) yields ciphertext — TOTP seeds and PM keys included.

- **NVS encryption** rides on this: the `secrets` NVS partition is encrypted
  along with the rest of flash. (ESP-IDF also supports a separate NVS-encryption
  scheme with an `nvs_keys` partition; with flash encryption already on, the
  simpler flash-level encryption is sufficient here — see the `partitions.csv`
  TODO if a keys partition turns out to be required by the pinned core.)
- **Secure Boot** (optional, stronger) additionally stops an attacker from
  flashing modified firmware that would dump the decrypted secrets at runtime.
  Recommended for a release unit; note the eFuse burn is effectively one-way.

**Rollout policy (decide before release):**
- *Development mode* flash encryption allows re-flashing with the encryption
  key present — good for iterating. *Release mode* disables the serial
  re-flash path. Burn release-mode eFuses only on a unit whose firmware is
  finished. Document which unit is which; a mis-burn bricks re-flashing.

### 1.3 In-RAM hygiene

- The PM **master password is never persisted.** From it we derive the master
  key, immediately stretch + unwrap the user key, then **zeroize the master key
  buffer** (`vault.cpp` overwrites `s_masterKey`). `Vault::lock()` zeroizes the
  user enc/mac keys and clears decrypted caches.
- TOTP `compute()` wipes the decoded key and HMAC buffers off the stack before
  returning.
- Decrypted **passwords/TOTPs are revealed on demand**, not held in the list.
  The clipboard buffer auto-clears (default 20 s).
- **Auto-lock** (TODO in `ui.cpp`): re-lock the vault after an idle timeout and
  on returning to the home screen, so a walk-away doesn't leave it unlocked.

### 1.4 Threat model boundaries (honest scope)

- **Covered:** flash read-out at rest (encryption), tamper of vault ciphertext
  (AES-CBC **+ HMAC-SHA256** verify before decrypt — `decryptEncString_` checks
  the MAC constant-time and rejects on mismatch), wrong-password rejection
  offline (key unwrap HMAC fails).
- **Not covered by this device alone:** a live attacker with the unlocked
  device in hand; sophisticated hardware glitching of eFuse protections; a
  malicious/self-signed MQTT or PM server if cert verification is left off
  (hence the `setCACert` TODOs — pin the private CA, do **not** ship
  `setInsecure()`).

---

## 2. Notification transport decision

**Chosen: MQTT over TLS**, with a server-side notifyd sender
(`server-shim/mqtt.py`) bridging RabbitMQ → MQTT. The Tab5 subscribes to
`notify/#`.

### Options considered

| Option | On-ESP cost | Server change | Verdict |
|--------|-------------|---------------|---------|
| **AMQP direct** (talk to RabbitMQ) | High — no good lightweight AMQP client for Arduino/ESP; connection + channel + topic semantics are heavy | none | ✗ too heavy on device |
| **HTTP long-poll** | Low client, but needs a new stateful endpoint + poll loop; latency/battery tradeoff; misses if offline | new service | ✗ new server surface, worse latency |
| **WebSocket push** | Medium — WS client + framing + reconnect + a new push service holding per-device connections | new service | ✗ most server work |
| **MQTT/TLS** | **Low** — mature `PubSubClient`, tiny footprint, native pub/sub + auto-reconnect + QoS1 | **one sender plugin** reusing existing notifyd sender interface + a Mosquitto broker | ✓ **chosen** |

### Why MQTT wins here

- **Smallest device code.** MQTT is the de-facto ESP messaging protocol;
  `PubSubClient` is a few KB and handles subscribe/reconnect. AMQP on ESP is a
  poor fit.
- **Smallest server change.** SolariNet's notify service is already a
  fan-out-to-pluggable-senders design (`deploy/notify/senders/`). The bridge is
  **one new sender** (`mqtt.py`) plus a REGISTRY line — `notifyd.py` is
  untouched, and MQTT delivery becomes just another routable channel per
  severity, exactly like `sms`/`push`.
- **Right semantics.** Topic hierarchy (`notify/<severity>`) maps cleanly onto
  the existing routing keys; the Tab5 can subscribe broadly (`notify/#`) or
  narrowly. QoS1 gives at-least-once; the device just displays messages, so
  occasional duplicates are harmless.
- **Reuses the JSON contract.** The shim forwards `notifyd`'s existing message
  dict verbatim (minus `_private` fields), so device and server never diverge on
  schema.

### Cost of the choice

One new daemon: **Mosquitto**, co-located on benzene with RabbitMQ. Two MQTT
users (notifyd publish, tab5 subscribe), TLS with the self-hosted CA. That is
the entire server-side footprint. If a broker is truly unwanted later, the same
`mqtt.py` sender shape could be swapped for an HTTP-push sender without touching
the device's feed/UI logic — but MQTT is the simplest correct path today.
