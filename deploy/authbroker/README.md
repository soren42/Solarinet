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

The broker holds each device's **public** response key and a separate private
key that signs requests. A compromised broker can forge a prompt, but cannot
forge an approval: only the Tab5 holds its response private key in encrypted
NVS.

## Files

| Path | Role |
|------|------|
| `authbrokerd.py` | the daemon: MQTT client + tiny HTTP API + signature verify |
| `enroll_device.py` | convert a Tab5-generated public point to broker enrollment PEM |
| `loopback_test.py` | MQTT round-trip test against an explicitly selected broker (no device needed) |
| `authbroker.conf.example` | config template (copy → `authbroker.conf`, gitignored) |
| `authbroker.service` | systemd unit |
| `dashboard/AuthBroker.php` + `INTEGRATION.md` | additive, config-gated dashboard wiring |

## Message contract

All payloads are compact JSON. `<id>` is a random hex request id.

**Request** — broker → device, topic `auth/request/<id>` (signed before publish):
```json
{ "v":2, "kid":"primary", "id":"<hex>", "ts":1720000000, "ttl":60,
  "action":"login", "subject":"jason", "detail":"SolariNet dashboard",
  "source_ip":"10.5.2.20", "nonce":"<base64 32B, server random>",
  "device":"tab5-desk", "sig":"<base64>" }
```
`device` may be empty to target any enrolled unit. The Tab5 verifies `sig`
against the imported public key selected by `kid` before rendering any field.

**Response** — device → broker, topic `auth/response/<id>` (signed):
```json
{ "v":2, "id":"<hex>", "ts":1720000005, "decision":"approve",
  "device_id":"tab5-desk", "nonce":"<echoed>",
  "req_digest":"<sha256 of canonical request>", "sig":"<base64>" }
```

## Signing scheme

- Curve **secp256r1 (NIST P-256)**, **ECDSA / SHA-256**, signature **DER**,
  transported **base64** in `sig`.
- Requests and responses use distinct domain separators followed by their
  ordered UTF-8 fields. Each field is prefixed with a four-byte unsigned
  big-endian byte length. The request order is `v,kid,id,ts,ttl,action,subject,
  detail,source_ip,nonce,device`; the response order is
  `v,id,decision,nonce,device_id,req_digest`.
- The broker accepts a response only if ALL hold: `v==2`, topic id == body id,
  `nonce` matches the outstanding request (anti-replay), `decision ∈
  {approve,deny}`, within TTL, `device_id` is enrolled (and matches the target
  if one was set), `req_digest` matches the exact published request, and the
  signature verifies against that device's public key.
  Any failure ⇒ the response is ignored and the login times out (fails closed).

## HTTP API (what the dashboard calls)

Bind to localhost/dashboard host only — never expose it. Auth via
`Authorization: Bearer <token>` (the `[http] token` in the config).
The daemon refuses to start when the token is empty or still set to the example
value. Missing and incorrect bearer credentials are always rejected.

- `POST /auth/request` `{action,subject,detail,source_ip,device?,ttl?}` →
  blocks up to `ttl` → `{ "decision":"approve"|"deny"|"timeout", "id","device_id" }`.
  `200` for approve/deny, `202` for timeout, `401` for missing or bad token.
- `GET /healthz` → `{ "ok":true, "connected":<mqtt up?> }`.

## Setup

```bash
cp deploy/authbroker/authbroker.conf.example deploy/authbroker/authbroker.conf
$EDITOR deploy/authbroker/authbroker.conf     # MQTT pw, HTTP token
chmod 600 deploy/authbroker/authbroker.conf

python3 -m venv deploy/authbroker/.venv
deploy/authbroker/.venv/bin/pip install -r deploy/authbroker/requirements.txt

# Copy the public point shown by SOLARI:SHOW_KEYS into a local file first.
deploy/authbroker/.venv/bin/python deploy/authbroker/enroll_device.py tab5-desk \
    --public-point-hex-file /path/to/tab5-desk-public-point.txt

deploy/authbroker/.venv/bin/python deploy/authbroker/authbrokerd.py \
    --config deploy/authbroker/authbroker.conf --check   # sanity, then run without --check
```

systemd: see the header of `authbroker.service`.

Broker request signing requires `request_signing_key_id` and
`request_signing_key_file`. Generate a separate P-256 broker key on the broker
host as the service account, store it outside the repository with mode `0600`,
and import its public point on the Tab5 over the physically attached USB serial console using
`SOLARI:SET_BROKER_KEY:<kid>:<hex>`. For rotation, import a second key ID, switch
the broker to that ID, verify operation, then remove the old ID with
`SOLARI:REMOVE_BROKER_KEY:<kid>`.

For the service unit's current `User=jason` setting, one way to create the key
without printing its private material is:

```bash
install -d -m 0700 /home/jason/.config/solarinet
umask 077
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
    -out /home/jason/.config/solarinet/authbroker-request-signing.pem
openssl pkey -in /home/jason/.config/solarinet/authbroker-request-signing.pem \
    -pubout -out /dev/shm/authbroker-request-public.pem
openssl pkey -pubin -in /dev/shm/authbroker-request-public.pem -outform DER \
    | tail -c 65 | xxd -p -c 130
```

Copy the final public point through an independently trusted path before
importing it. Update `request_signing_key_file` in the private broker config to
the generated path. The generated public PEM in `/dev/shm` is not private.

Then wire the dashboard: `dashboard/INTEGRATION.md`.

## Isolated MQTT loopback test

Proves the MQTT + signature path with a simulated Tab5. By default the test
uses anonymous MQTT on `127.0.0.1`; it does not contain credentials or default
to a live broker. Run it against a disposable local Mosquitto instance:

```bash
deploy/authbroker/.venv/bin/python deploy/authbroker/loopback_test.py --host 127.0.0.1 --port 18883
deploy/authbroker/.venv/bin/python deploy/authbroker/loopback_test.py --host 127.0.0.1 --port 18883 --decision deny
```

It mints an ephemeral device keypair, starts a broker that trusts it, spawns a
fake device that signs+publishes a response, then calls `broker.request()` and
asserts the returned decision. CI starts a throwaway broker bound only to
loopback and exercises both approve and deny. The test accepts explicit host
and credential arguments for controlled fixtures; point it only at a
disposable broker.

## Security notes

- Device response private keys never leave the Tab5. A separate broker request
  signing key is held in an owner-only file readable by the broker service.
- `nonce` is 32 random bytes per request; replays are rejected by nonce
  mismatch and by TTL. Request ids are single-use (dropped once resolved).
- MQTT uses CA-verified TLS outside explicitly enabled loopback development.
  The Tab5 refuses to connect until its CA is imported.
- The HTTP `token` is a shared secret between dashboard and broker; keep both on
  the same host and the API bound to localhost.
