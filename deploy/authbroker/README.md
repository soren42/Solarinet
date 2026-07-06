# authbrokerd — SolariNet Authenticator approval broker

Bridges dashboard / Keycloak authentication events to the **M5Stack Tab5
("SolariNet Authenticator")** over MQTT and waits for a signed push-approval.
This is the server half of the Tab5's push-approval (2FA / access-approval)
feature; the device half is in `firmware/tab5/` (`approvals.cpp`).

```
 dashboard login ─HTTP POST /auth/request─▶ authbrokerd ─MQTT auth/request/<id>─▶ Tab5
        proceeds ◀──── {decision} ───────── authbrokerd ◀─MQTT auth/response/<id>─ Tab5
                                             (verifies the device signature)      (Approve/Deny)
```

The broker holds only each device's **public** key, so a broker compromise
cannot forge an approval — only the Tab5, holding the private key in encrypted
NVS, can sign a valid response.

## Files

| Path | Role |
|------|------|
| `authbrokerd.py` | the daemon: MQTT client + tiny HTTP API + signature verify |
| `enroll_device.py` | mint an ECDSA-P256 keypair for a Tab5 (private → device, public → config) |
| `loopback_test.py` | live round-trip test against the real Mosquitto (no device needed) |
| `authbroker.conf.example` | config template (copy → `authbroker.conf`, gitignored) |
| `authbroker.service` | systemd unit |
| `dashboard/AuthBroker.php` + `INTEGRATION.md` | additive, config-gated dashboard wiring |

## Message contract

All payloads are compact JSON. `<id>` is a random hex request id.

**Request** — broker → device, topic `auth/request/<id>` (unsigned; not secret):
```json
{ "v":1, "id":"<hex>", "ts":1720000000, "ttl":60,
  "action":"login", "subject":"jason", "detail":"SolariNet dashboard",
  "source_ip":"10.5.2.20", "nonce":"<base64 32B, server random>",
  "device":"tab5-desk" }          // "device" optional: target one unit
```

**Response** — device → broker, topic `auth/response/<id>` (signed):
```json
{ "v":1, "id":"<hex>", "ts":1720000005, "decision":"approve",
  "device_id":"tab5-desk", "nonce":"<echoed>", "sig":"<base64>" }
```

## Signing scheme

- Curve **secp256r1 (NIST P-256)**, **ECDSA / SHA-256**, signature **DER**,
  transported **base64** in `sig`.
- The signed bytes are a fixed canonical string (UTF-8), built identically on
  both sides:
  ```
  "<v>\n<id>\n<decision>\n<nonce>\n<device_id>"
  ```
- The broker accepts a response only if ALL hold: `v==1`, topic id == body id,
  `nonce` matches the outstanding request (anti-replay), `decision ∈
  {approve,deny}`, within TTL, `device_id` is enrolled (and matches the target
  if one was set), and the signature verifies against that device's public key.
  Any failure ⇒ the response is ignored and the login times out (fails closed).

## HTTP API (what the dashboard calls)

Bind to localhost/dashboard host only — never expose it. Auth via
`Authorization: Bearer <token>` (the `[http] token` in the config).

- `POST /auth/request` `{action,subject,detail,source_ip,device?,ttl?}` →
  blocks up to `ttl` → `{ "decision":"approve"|"deny"|"timeout", "id","device_id" }`.
  `200` for approve/deny, `202` for timeout, `401` for bad token.
- `GET /healthz` → `{ "ok":true, "connected":<mqtt up?> }`.

## Setup

```bash
cp deploy/authbroker/authbroker.conf.example deploy/authbroker/authbroker.conf
$EDITOR deploy/authbroker/authbroker.conf     # MQTT pw, HTTP token
chmod 600 deploy/authbroker/authbroker.conf

python3 -m venv deploy/authbroker/.venv
deploy/authbroker/.venv/bin/pip install paho-mqtt cryptography

# enroll a device: private scalar goes into the Tab5 (NVS dev/privkey),
# the printed [device.<id>] block goes into authbroker.conf
deploy/authbroker/.venv/bin/python deploy/authbroker/enroll_device.py tab5-desk

deploy/authbroker/.venv/bin/python deploy/authbroker/authbrokerd.py \
    --config deploy/authbroker/authbroker.conf --check   # sanity, then run without --check
```

systemd: see the header of `authbroker.service`.

Then wire the dashboard: `dashboard/INTEGRATION.md`.

## Live loopback test

Proves the whole MQTT + signature path against the **real Mosquitto**
(`10.5.2.50:1883`, user `solari`) with a simulated Tab5 — no dashboard, no HTTP,
no hardware:

```bash
deploy/authbroker/.venv/bin/python deploy/authbroker/loopback_test.py            # approve
deploy/authbroker/.venv/bin/python deploy/authbroker/loopback_test.py --decision deny
```

It mints an ephemeral device keypair, starts a broker that trusts it, spawns a
fake device that signs+publishes a response, then calls `broker.request()` and
asserts the returned decision. **Verified 2026-07-06** against benzene's
Mosquitto: approve → `approve`, deny → `deny`, and a response signed with the
WRONG key is rejected (`SIGNATURE INVALID`, login times out).

## Security notes

- Private keys never touch the broker host — only public keys in the config.
- `nonce` is 32 random bytes per request; replays are rejected by nonce
  mismatch and by TTL. Request ids are single-use (dropped once resolved).
- MQTT is plaintext `:1883` for now; move to `:8883` TLS (broker CA) as the
  documented follow-up — set `tls=true` + `ca_cert` here and provision the CA
  on the Tab5 (`mqttbus.cpp` `setCACert` TODO).
- The HTTP `token` is a shared secret between dashboard and broker; keep both on
  the same host and the API bound to localhost.
